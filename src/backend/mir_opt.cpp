#include <backend/mir_opt.hpp>

#include <vector>

namespace Backend {

namespace {

// --- instruction classification ----------------------------------------------

// True if the instruction has an effect beyond defining its Def/UseDef vreg
// operands — i.e. it must never be removed even when its defined vreg is unused.
// This covers memory writes, control flow, calls, fences, atomics, inline asm,
// and the flag-producing compares (whose result is consumed positionally by a
// following SetCC/Jcc, not through a vreg).
bool hasSideEffects(const MInst& in) {
    switch (in.op) {
        // memory stores
        case MOpcode::Store:
        case MOpcode::StoreOutgoing:
        case MOpcode::StoreInd:
        case MOpcode::FStore:
        case MOpcode::FStoreInd:
        case MOpcode::FStoreOutgoing:
        // control flow / calls
        case MOpcode::Jmp:
        case MOpcode::Jcc:
        case MOpcode::Call:
        case MOpcode::CallImport:
        case MOpcode::CallIndirect:
        case MOpcode::Syscall:
        case MOpcode::Ret:
        // ordering / atomics / asm
        case MOpcode::Fence:
        case MOpcode::AtomicXAdd:
        case MOpcode::AtomicCmpXchg:
        case MOpcode::AsmFixed:
        // flag producers (result consumed by the next SetCC/Jcc via EFLAGS)
        case MOpcode::Cmp:
        case MOpcode::FCmp:
            return true;
        default:
            return false;
    }
}

// True if the instruction writes a physical register (ABI return reg, RDX:RAX
// for div, etc.) or carries clobbers. Such instructions are never removed by
// DCE because their physical-register effect may be depended on positionally
// (e.g. a MovRR into the ABI return register before Ret).
bool definesPhysOrClobbers(const MInst& in) {
    if (!in.clobbers.empty()) return true;
    for (const auto& op : in.operands) {
        if (op.kind == OperandKind::PhysReg &&
            (op.role == OperandRole::Def || op.role == OperandRole::UseDef)) {
            return true;
        }
    }
    return false;
}

// --- fixpoint helpers --------------------------------------------------------

// Peephole: remove trivially redundant register-to-register moves where source
// and destination are the same virtual register (`MovRR v, v` / `FMovRR v, v`).
// These arise from selection patterns that route a value through an explicit
// copy that selection later proves identical. Returns count removed.
unsigned peephole(MFunction& fn) {
    unsigned removed = 0;
    for (auto& bb : fn.blocks()) {
        auto& insts = bb.insts;
        for (std::size_t i = 0; i < insts.size();) {
            const MInst& in = insts[i];
            bool drop = false;
            if ((in.op == MOpcode::MovRR || in.op == MOpcode::FMovRR) &&
                in.operands.size() == 2 &&
                in.operands[0].kind == OperandKind::VirtReg &&
                in.operands[1].kind == OperandKind::VirtReg &&
                in.operands[0].vreg == in.operands[1].vreg) {
                drop = true;
            }
            if (drop) {
                insts.erase(insts.begin() + static_cast<std::ptrdiff_t>(i));
                ++removed;
            } else {
                ++i;
            }
        }
    }
    return removed;
}

// Dead-code elimination: remove an instruction whose sole effect is defining a
// virtual register that is never used anywhere in the function, provided it has
// no side effects and touches no physical registers/clobbers.
//
// Liveness here is whole-function (a vreg is "used" if any operand reads it via
// Use/UseDef in any block). This is conservative but sound: SSA-like single
// assignment is not assumed, so a vreg defined twice and used once keeps both
// defs — we only drop defs of vregs with zero uses across the function. Iterated
// by the driver loop so a removed instruction's own operand reads can expose
// further dead defs.
unsigned deadCodeElim(MFunction& fn) {
    const std::uint32_t n = fn.numVRegs();
    std::vector<bool> used(n, false);
    for (const auto& bb : fn.blocks()) {
        for (const auto& in : bb.insts) {
            for (const auto& op : in.operands) {
                if (op.kind == OperandKind::VirtReg &&
                    (op.role == OperandRole::Use || op.role == OperandRole::UseDef) &&
                    op.vreg < n) {
                    used[op.vreg] = true;
                }
            }
        }
    }

    unsigned removed = 0;
    for (auto& bb : fn.blocks()) {
        auto& insts = bb.insts;
        for (std::size_t i = 0; i < insts.size();) {
            const MInst& in = insts[i];
            if (hasSideEffects(in) || definesPhysOrClobbers(in)) {
                ++i;
                continue;
            }
            // Find the (single) vreg this instruction defines, if any. The
            // instruction is dead iff it defines exactly the set of vregs that
            // are all unused and reads nothing that has side effects.
            bool definesSomething = false;
            bool allDefsUnused = true;
            for (const auto& op : in.operands) {
                if (op.kind == OperandKind::VirtReg &&
                    (op.role == OperandRole::Def || op.role == OperandRole::UseDef)) {
                    definesSomething = true;
                    if (op.vreg >= n || used[op.vreg]) allDefsUnused = false;
                }
            }
            if (definesSomething && allDefsUnused) {
                insts.erase(insts.begin() + static_cast<std::ptrdiff_t>(i));
                ++removed;
            } else {
                ++i;
            }
        }
    }
    return removed;
}

// Branch simplification: a terminating `Jmp`/`Jcc` whose target is the block
// that immediately follows in program order is redundant with fall-through.
//   * `Jmp -> next`  : drop it entirely (control falls through).
//   * `Jcc -> next`  : the branch and the fall-through have the same target, so
//                      the conditional jump is a no-op and is dropped. (The flag-
//                      producing Cmp before it is left intact; it is harmless and
//                      removing it would require proving no other consumer.)
// Block ordering is preserved (no blocks are reordered or merged), so all other
// fall-through edges remain valid.
unsigned simplifyBranches(MFunction& fn) {
    unsigned removed = 0;
    const std::size_t numBlocks = fn.blocks().size();
    for (std::size_t b = 0; b < numBlocks; ++b) {
        auto& insts = fn.blocks()[b].insts;
        if (insts.empty()) continue;
        MInst& term = insts.back();
        const std::uint32_t next = static_cast<std::uint32_t>(b + 1);
        if ((term.op == MOpcode::Jmp || term.op == MOpcode::Jcc) &&
            !term.operands.empty() &&
            term.operands[0].kind == OperandKind::Label &&
            term.operands[0].label == next && next < numBlocks) {
            insts.pop_back();
            ++removed;
        }
    }
    return removed;
}

}  // namespace

unsigned optimizeFunction(MFunction& fn, int optLevel) {
    if (optLevel <= 0) return 0;

    unsigned total = 0;
    // Iterate the local passes to a fixpoint: peephole and branch cleanup can
    // expose newly dead code, and DCE can expose redundant moves, etc. Bounded
    // by a generous cap to guarantee termination regardless of pass interaction.
    constexpr unsigned kMaxRounds = 16;
    for (unsigned round = 0; round < kMaxRounds; ++round) {
        unsigned r = 0;
        r += peephole(fn);
        r += deadCodeElim(fn);
        r += simplifyBranches(fn);
        total += r;
        if (r == 0) break;
    }
    return total;
}

}  // namespace Backend
