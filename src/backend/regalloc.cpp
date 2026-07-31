#include <backend/regalloc.hpp>

#include <algorithm>
#include <limits>
#include <set>

namespace Backend {

namespace {

// Visit every operand of every instruction in linearized order, calling
// `fn(programPoint, inst, operand)`. The program point increases by one per
// instruction across all blocks.
template <typename Fn>
void forEachOperand(const MFunction& fn, Fn&& visit) {
    int pp = 0;
    for (const auto& bb : fn.blocks()) {
        for (const auto& inst : bb.insts) {
            for (const auto& op : inst.operands) {
                visit(pp, inst, op);
            }
            ++pp;
        }
    }
}

// Successors of a block, derived from its terminator. `Jmp` goes only to its
// label; `Jcc` goes to its label or falls through to the next block; `Ret` has
// none; any other final instruction falls through to the next block.
std::vector<std::uint32_t> successorsOf(const MFunction& fn, std::uint32_t bi) {
    std::vector<std::uint32_t> succ;
    const auto& blocks = fn.blocks();
    const auto& bb = blocks[bi];
    const std::uint32_t fallthrough = bi + 1;
    const bool hasFallthrough = fallthrough < blocks.size();

    if (bb.insts.empty()) {
        if (hasFallthrough) succ.push_back(fallthrough);
        return succ;
    }
    const MInst& term = bb.insts.back();
    switch (term.op) {
        case MOpcode::Jmp:
            succ.push_back(term.operands[0].label);
            break;
        case MOpcode::Jcc:
            succ.push_back(term.operands[0].label);
            if (hasFallthrough) succ.push_back(fallthrough);
            break;
        case MOpcode::Ret:
            break;  // no successors
        default:
            if (hasFallthrough) succ.push_back(fallthrough);
            break;
    }
    return succ;
}

}  // namespace

std::vector<LiveInterval> LinearScanAllocator::computeIntervals(
    const MFunction& fn) const {
    const std::uint32_t n = fn.numVRegs();
    const std::size_t numBlocks = fn.blocks().size();

    // --- Program-point numbering: one point per instruction, contiguous, with
    // each block's [start,end) range recorded. -----------------------------
    std::vector<int> blockStart(numBlocks, 0);
    std::vector<int> blockEnd(numBlocks, 0);  // exclusive
    {
        int pp = 0;
        for (std::uint32_t b = 0; b < numBlocks; ++b) {
            blockStart[b] = pp;
            pp += static_cast<int>(fn.blocks()[b].insts.size());
            blockEnd[b] = pp;
        }
    }

    // --- Per-block use/def sets for dataflow. A vreg is in `use` if it is read
    // before being written within the block; in `def` if it is written in the
    // block at all. -------------------------------------------------------
    std::vector<std::vector<bool>> useSet(numBlocks, std::vector<bool>(n, false));
    std::vector<std::vector<bool>> defSet(numBlocks, std::vector<bool>(n, false));
    for (std::uint32_t b = 0; b < numBlocks; ++b) {
        std::vector<bool> written(n, false);
        for (const auto& inst : fn.blocks()[b].insts) {
            // Reads first (an operand that both uses and defs reads the old val).
            for (const auto& op : inst.operands) {
                if (op.kind != OperandKind::VirtReg || op.vreg >= n) continue;
                const bool reads = op.role == OperandRole::Use ||
                                   op.role == OperandRole::UseDef;
                if (reads && !written[op.vreg] && !defSet[b][op.vreg]) {
                    useSet[b][op.vreg] = true;
                }
            }
            for (const auto& op : inst.operands) {
                if (op.kind != OperandKind::VirtReg || op.vreg >= n) continue;
                const bool writes = op.role == OperandRole::Def ||
                                    op.role == OperandRole::UseDef;
                if (writes) { defSet[b][op.vreg] = true; written[op.vreg] = true; }
            }
        }
    }

    // --- Successors / predecessors. --------------------------------------
    std::vector<std::vector<std::uint32_t>> succ(numBlocks);
    for (std::uint32_t b = 0; b < numBlocks; ++b) succ[b] = successorsOf(fn, b);

    // --- Backward dataflow fixpoint for liveIn/liveOut. ------------------
    std::vector<std::vector<bool>> liveIn(numBlocks, std::vector<bool>(n, false));
    std::vector<std::vector<bool>> liveOut(numBlocks, std::vector<bool>(n, false));
    bool changed = true;
    while (changed) {
        changed = false;
        // Iterate blocks in reverse for faster convergence on forward CFGs.
        for (std::uint32_t bi = numBlocks; bi-- > 0;) {
            // liveOut[b] = union of liveIn[s] over successors s.
            std::vector<bool> newOut(n, false);
            for (std::uint32_t s : succ[bi]) {
                for (std::uint32_t v = 0; v < n; ++v)
                    if (liveIn[s][v]) newOut[v] = true;
            }
            // liveIn[b] = use[b] ∪ (liveOut[b] − def[b]).
            std::vector<bool> newIn(n, false);
            for (std::uint32_t v = 0; v < n; ++v) {
                if (useSet[bi][v] || (newOut[v] && !defSet[bi][v])) newIn[v] = true;
            }
            if (newOut != liveOut[bi]) { liveOut[bi] = std::move(newOut); changed = true; }
            if (newIn != liveIn[bi]) { liveIn[bi] = std::move(newIn); changed = true; }
        }
    }

    // --- Build intervals over the linear program points. -----------------
    // For each vreg, the interval must cover:
    //   * every actual def/use program point, and
    //   * the full extent of any block where it is live-in or live-out (this is
    //     what makes loop-carried values correct: a value live across a
    //     back-edge stays live for the whole loop body).
    std::vector<int> lo(n, std::numeric_limits<int>::max());
    std::vector<int> hi(n, std::numeric_limits<int>::min());
    auto extend = [&](VReg v, int a, int b) {
        if (a < lo[v]) lo[v] = a;
        if (b > hi[v]) hi[v] = b;
    };

    // Per-block liveness extents.
    for (std::uint32_t b = 0; b < numBlocks; ++b) {
        if (blockEnd[b] == blockStart[b]) continue;  // empty block
        const int first = blockStart[b];
        const int last = blockEnd[b] - 1;
        for (std::uint32_t v = 0; v < n; ++v) {
            // Live throughout the block (passes straight through).
            if (liveIn[b][v] && liveOut[b][v]) { extend(v, first, last); continue; }
            // Live-in: live from block entry until its last use here.
            if (liveIn[b][v]) extend(v, first, first);
            // Live-out: live from its def here through block exit.
            if (liveOut[b][v]) extend(v, last, last);
        }
    }

    // Precise def/use points (also covers vregs entirely local to one block).
    forEachOperand(fn, [&](int pp, const MInst&, const MOperand& op) {
        if (op.kind != OperandKind::VirtReg || op.vreg >= n) return;
        extend(op.vreg, pp, pp);
    });

    // Emit intervals for vregs that were referenced, sorted by start point.
    std::vector<LiveInterval> live;
    live.reserve(n);
    for (std::uint32_t v = 0; v < n; ++v) {
        if (hi[v] >= lo[v]) {  // referenced at least once
            LiveInterval iv;
            iv.vreg = v;
            iv.start = lo[v];
            iv.end = hi[v];
            live.push_back(iv);
        }
    }
    std::sort(live.begin(), live.end(),
              [](const LiveInterval& a, const LiveInterval& b) {
                  if (a.start != b.start) return a.start < b.start;
                  return a.vreg < b.vreg;
              });
    return live;
}

Allocation LinearScanAllocator::run(MFunction& fn) {
    const std::uint32_t n = fn.numVRegs();
    Allocation alloc;
    alloc.vregToPhys.assign(n, PhysReg::None);
    alloc.vregToXmm.assign(n, XmmReg::None);
    alloc.vregToSlot.assign(n, kInvalidVReg);

    std::vector<LiveInterval> intervals = computeIntervals(fn);

    // Precompute, per program point, which physical registers are occupied by
    // fixed PhysReg operands or call clobbers, so we never hand those out while
    // they are live. GP and XMM files are tracked in separate masks. For the
    // scaffold we treat a fixed physreg as busy only at its own program point
    // (straight-line code); this is conservative enough for ABI arg/return
    // placement and call clobbers as currently modeled.
    int numPoints = 0;
    for (const auto& bb : fn.blocks()) numPoints += static_cast<int>(bb.insts.size());
    std::vector<std::uint16_t> gpBusyAt(numPoints > 0 ? numPoints : 1, 0);
    std::vector<std::uint16_t> xmmBusyAt(numPoints > 0 ? numPoints : 1, 0);
    {
        int pp = 0;
        for (const auto& bb : fn.blocks()) {
            for (const auto& inst : bb.insts) {
                for (const auto& op : inst.operands) {
                    if (op.kind == OperandKind::PhysReg) {
                        if (op.phys != PhysReg::None)
                            gpBusyAt[pp] |= (1u << regIndex(op.phys));
                        if (op.xmm != XmmReg::None)
                            xmmBusyAt[pp] |= (1u << xmmIndex(op.xmm));
                    }
                }
                for (PhysReg c : inst.clobbers) {
                    gpBusyAt[pp] |= (1u << regIndex(c));
                }
                // A call clobbers all caller-saved XMM registers.
                if (inst.op == MOpcode::Call) {
                    for (XmmReg x : abi_.xmmCallerSaved)
                        xmmBusyAt[pp] |= (1u << xmmIndex(x));
                }
                ++pp;
            }
        }
    }
    auto gpBusyDuring = [&](PhysReg r, int start, int end) {
        const std::uint16_t bit = (1u << regIndex(r));
        for (int p = start; p <= end && p < static_cast<int>(gpBusyAt.size()); ++p)
            if (gpBusyAt[p] & bit) return true;
        return false;
    };
    auto xmmBusyDuring = [&](XmmReg r, int start, int end) {
        const std::uint16_t bit = (1u << xmmIndex(r));
        for (int p = start; p <= end && p < static_cast<int>(xmmBusyAt.size()); ++p)
            if (xmmBusyAt[p] & bit) return true;
        return false;
    };

    // Active intervals, ordered by increasing end point. One set per class so
    // the two register files are allocated independently.
    struct ActiveCmp {
        bool operator()(const LiveInterval& a, const LiveInterval& b) const {
            if (a.end != b.end) return a.end < b.end;
            return a.vreg < b.vreg;
        }
    };
    std::set<LiveInterval, ActiveCmp> activeGP, activeXMM;

    auto tryAssignGP = [&](const LiveInterval& iv) -> PhysReg {
        std::uint16_t inUse = 0;
        for (const auto& a : activeGP) {
            PhysReg p = alloc.vregToPhys[a.vreg];
            if (p != PhysReg::None) inUse |= (1u << regIndex(p));
        }
        for (PhysReg r : abi_.allocatable) {
            if (inUse & (1u << regIndex(r))) continue;
            if (gpBusyDuring(r, iv.start, iv.end)) continue;
            return r;
        }
        return PhysReg::None;
    };
    auto tryAssignXMM = [&](const LiveInterval& iv) -> XmmReg {
        std::uint16_t inUse = 0;
        for (const auto& a : activeXMM) {
            XmmReg x = alloc.vregToXmm[a.vreg];
            if (x != XmmReg::None) inUse |= (1u << xmmIndex(x));
        }
        for (XmmReg r : abi_.xmmAllocatable) {
            if (inUse & (1u << xmmIndex(r))) continue;
            if (xmmBusyDuring(r, iv.start, iv.end)) continue;
            return r;
        }
        return XmmReg::None;
    };

    auto expire = [](std::set<LiveInterval, ActiveCmp>& active, int start) {
        for (auto it = active.begin(); it != active.end();) {
            if (it->end < start) it = active.erase(it);
            else ++it;
        }
    };

    for (const auto& iv : intervals) {
        const bool isXmm = fn.vregClass(iv.vreg) == RegClass::XMM;
        if (isXmm) {
            expire(activeXMM, iv.start);
            XmmReg reg = tryAssignXMM(iv);
            if (reg != XmmReg::None) {
                alloc.vregToXmm[iv.vreg] = reg;
                activeXMM.insert(iv);
            } else if (!activeXMM.empty()) {
                auto last = std::prev(activeXMM.end());
                if (last->end > iv.end) {
                    XmmReg stolen = alloc.vregToXmm[last->vreg];
                    alloc.vregToXmm[last->vreg] = XmmReg::None;
                    alloc.vregToSlot[last->vreg] = fn.addFrameSlot(8, 8, true);
                    alloc.anySpilled = true;
                    alloc.vregToXmm[iv.vreg] = stolen;
                    activeXMM.erase(last);
                    activeXMM.insert(iv);
                } else {
                    alloc.vregToSlot[iv.vreg] = fn.addFrameSlot(8, 8, true);
                    alloc.anySpilled = true;
                }
            } else {
                alloc.vregToSlot[iv.vreg] = fn.addFrameSlot(8, 8, true);
                alloc.anySpilled = true;
            }
            continue;
        }
        // GP class.
        expire(activeGP, iv.start);
        PhysReg reg = tryAssignGP(iv);
        if (reg != PhysReg::None) {
            alloc.vregToPhys[iv.vreg] = reg;
            activeGP.insert(iv);
        } else if (!activeGP.empty()) {
            auto last = std::prev(activeGP.end());
            if (last->end > iv.end) {
                PhysReg stolen = alloc.vregToPhys[last->vreg];
                alloc.vregToPhys[last->vreg] = PhysReg::None;
                alloc.vregToSlot[last->vreg] = fn.addFrameSlot(8, 8, /*isSpill=*/true);
                alloc.anySpilled = true;
                alloc.vregToPhys[iv.vreg] = stolen;
                activeGP.erase(last);
                activeGP.insert(iv);
            } else {
                alloc.vregToSlot[iv.vreg] = fn.addFrameSlot(8, 8, true);
                alloc.anySpilled = true;
            }
        } else {
            alloc.vregToSlot[iv.vreg] = fn.addFrameSlot(8, 8, true);
            alloc.anySpilled = true;
        }
    }

    // Write assignments back into every VirtReg operand (phys for GP, xmm for
    // XMM-class vregs).
    for (auto& bb : fn.blocks()) {
        for (auto& inst : bb.insts) {
            for (auto& op : inst.operands) {
                if (op.kind == OperandKind::VirtReg && op.vreg < n) {
                    if (fn.vregClass(op.vreg) == RegClass::XMM)
                        op.xmm = alloc.vregToXmm[op.vreg];
                    else
                        op.phys = alloc.vregToPhys[op.vreg];
                }
            }
        }
    }

    return alloc;
}

}  // namespace Backend
