// Unit tests for the Machine-IR optimization passes (peephole, dead-code
// elimination, branch simplification) driven by optimizeFunction().

#include <cstdint>
#include <cstdio>

#include <backend/machine_ir.hpp>
#include <backend/mir_opt.hpp>

using namespace Backend;

static int g_failures = 0;
static void check(bool cond, const char* what) {
    std::printf(cond ? "ok: %s\n" : "FAIL: %s\n", what);
    if (!cond) ++g_failures;
}

static std::size_t instCount(const MFunction& fn) {
    std::size_t n = 0;
    for (const auto& bb : fn.blocks()) n += bb.insts.size();
    return n;
}

// ---------------------------------------------------------------------------
// optLevel 0 leaves the function completely untouched.
static void testOptLevelZeroIsNoOp() {
    MFunction fn("noop", Abi::SystemV);
    std::uint32_t bb = fn.addBlock("entry");
    VReg v0 = fn.newVReg();
    auto& B = fn.block(bb);
    B.insts.push_back({MOpcode::MovRI, {MOperand::defVReg(v0), MOperand::immediate(7)}});
    B.insts.push_back({MOpcode::MovRR, {MOperand::defVReg(v0), MOperand::useVReg(v0)}});
    B.insts.push_back({MOpcode::Ret, {}});

    std::size_t before = instCount(fn);
    unsigned removed = optimizeFunction(fn, 0);
    check(removed == 0, "optLevel 0 removes nothing");
    check(instCount(fn) == before, "optLevel 0 preserves instruction count");
}

// ---------------------------------------------------------------------------
// Self-move `MovRR v, v` is removed by the peephole pass.
static void testPeepholeSelfMove() {
    MFunction fn("selfmove", Abi::SystemV);
    std::uint32_t bb = fn.addBlock("entry");
    VReg v0 = fn.newVReg();
    auto& B = fn.block(bb);
    B.insts.push_back({MOpcode::MovRI, {MOperand::defVReg(v0), MOperand::immediate(5)}});
    B.insts.push_back({MOpcode::MovRR, {MOperand::defVReg(v0), MOperand::useVReg(v0)}});
    // keep v0 live so the MovRI is not itself DCE'd
    B.insts.push_back({MOpcode::Store, {MOperand::slot(0), MOperand::useVReg(v0)}});
    B.insts.push_back({MOpcode::Ret, {}});

    optimizeFunction(fn, 1);
    bool hasSelfMove = false;
    for (const auto& in : fn.block(bb).insts) {
        if (in.op == MOpcode::MovRR) hasSelfMove = true;
    }
    check(!hasSelfMove, "peephole removes self-move MovRR v,v");
    check(instCount(fn) == 3, "self-move test: MovRI + Store + Ret remain");
}

// ---------------------------------------------------------------------------
// A pure instruction whose defined vreg is never used is removed by DCE; one
// whose result is used is kept.
static void testDeadCodeElim() {
    MFunction fn("dce", Abi::SystemV);
    std::uint32_t bb = fn.addBlock("entry");
    VReg dead = fn.newVReg();
    VReg live = fn.newVReg();
    auto& B = fn.block(bb);
    B.insts.push_back({MOpcode::MovRI, {MOperand::defVReg(dead), MOperand::immediate(99)}});
    B.insts.push_back({MOpcode::MovRI, {MOperand::defVReg(live), MOperand::immediate(1)}});
    B.insts.push_back({MOpcode::Store, {MOperand::slot(0), MOperand::useVReg(live)}});
    B.insts.push_back({MOpcode::Ret, {}});

    optimizeFunction(fn, 1);
    bool deadGone = true, liveKept = false;
    for (const auto& in : fn.block(bb).insts) {
        if (in.op == MOpcode::MovRI && !in.operands.empty() &&
            in.operands[0].vreg == dead) deadGone = false;
        if (in.op == MOpcode::MovRI && !in.operands.empty() &&
            in.operands[0].vreg == live) liveKept = true;
    }
    check(deadGone, "DCE removes unused def");
    check(liveKept, "DCE keeps used def");
}

// ---------------------------------------------------------------------------
// DCE chains: a Def-only move consumed only by another dead Def-only move
// becomes dead once the consumer is removed (handled by the fixpoint loop).
// (Two-address UseDef ops are intentionally NOT chained away — see
// testTwoAddressKept — because their operand0 self-read keeps the vreg live.)
static void testDeadChain() {
    MFunction fn("deadchain", Abi::SystemV);
    std::uint32_t bb = fn.addBlock("entry");
    VReg a = fn.newVReg();
    VReg b = fn.newVReg();
    auto& B = fn.block(bb);
    B.insts.push_back({MOpcode::MovRI, {MOperand::defVReg(a), MOperand::immediate(3)}});
    // b = a   (Def-only copy; b is never used)
    B.insts.push_back({MOpcode::MovRR, {MOperand::defVReg(b), MOperand::useVReg(a)}});
    B.insts.push_back({MOpcode::Ret, {}});

    optimizeFunction(fn, 1);
    check(instCount(fn) == 1, "DCE removes a whole dead chain (only Ret remains)");
}

// ---------------------------------------------------------------------------
// A two-address op (UseDef operand0) defining an otherwise-unused vreg is kept,
// because its operand0 is also a read of that vreg — removing it soundly would
// require per-point liveness. This documents the conservative boundary.
static void testTwoAddressKept() {
    MFunction fn("twoaddr", Abi::SystemV);
    std::uint32_t bb = fn.addBlock("entry");
    VReg a = fn.newVReg();
    VReg b = fn.newVReg();
    auto& B = fn.block(bb);
    B.insts.push_back({MOpcode::MovRI, {MOperand::defVReg(a), MOperand::immediate(3)}});
    B.insts.push_back({MOpcode::MovRR, {MOperand::defVReg(b), MOperand::useVReg(a)}});
    B.insts.push_back({MOpcode::Add, {MOperand::useDefVReg(b), MOperand::useVReg(a)}});
    B.insts.push_back({MOpcode::Ret, {}});

    optimizeFunction(fn, 1);
    bool hasAdd = false;
    for (const auto& in : fn.block(bb).insts) {
        if (in.op == MOpcode::Add) hasAdd = true;
    }
    check(hasAdd, "DCE conservatively keeps two-address op with unused result");
}

// ---------------------------------------------------------------------------
// Memory writes, calls, compares and clobbering ops are never removed even when
// they define an unused vreg.
static void testSideEffectsPreserved() {
    MFunction fn("side", Abi::SystemV);
    std::uint32_t bb = fn.addBlock("entry");
    VReg unusedCmp = fn.newVReg();
    VReg p = fn.newVReg();
    VReg quotient = fn.newVReg();  // result of a Div, never used
    auto& B = fn.block(bb);
    // Store to memory: must survive.
    B.insts.push_back({MOpcode::MovRI, {MOperand::defVReg(p), MOperand::immediate(0)}});
    B.insts.push_back({MOpcode::StoreInd,
                       {MOperand::useVReg(p), MOperand::immediate(0), MOperand::useVReg(p)}});
    // Cmp produces flags consumed positionally: must survive even though it has
    // no vreg def.
    B.insts.push_back({MOpcode::Cmp, {MOperand::useVReg(p), MOperand::immediate(0)}});
    // Div clobbers RAX/RDX; quotient is unused but the op must survive.
    MInst div{MOpcode::Div,
              {MOperand::defVReg(quotient), MOperand::useVReg(p), MOperand::useVReg(p)}};
    div.clobbers = {PhysReg::RAX, PhysReg::RDX};
    B.insts.push_back(div);
    B.insts.push_back({MOpcode::Ret, {}});
    (void)unusedCmp;

    std::size_t before = instCount(fn);
    optimizeFunction(fn, 1);
    bool hasStore = false, hasCmp = false, hasDiv = false;
    for (const auto& in : fn.block(bb).insts) {
        if (in.op == MOpcode::StoreInd) hasStore = true;
        if (in.op == MOpcode::Cmp) hasCmp = true;
        if (in.op == MOpcode::Div) hasDiv = true;
    }
    check(hasStore, "DCE preserves memory store");
    check(hasCmp, "DCE preserves flag-producing Cmp");
    check(hasDiv, "DCE preserves clobbering Div with unused result");
    check(instCount(fn) == before, "side-effect test: nothing removed");
}

// ---------------------------------------------------------------------------
// A Jmp / Jcc to the immediately following block is redundant with fall-through
// and is dropped; a Jmp to a non-adjacent block is kept.
static void testBranchSimplify() {
    MFunction fn("branch", Abi::SystemV);
    std::uint32_t b0 = fn.addBlock("b0");
    std::uint32_t b1 = fn.addBlock("b1");  // index 1 (fall-through of b0)
    std::uint32_t b2 = fn.addBlock("b2");  // index 2

    // b0: jmp b1  (== fall-through -> removed)
    fn.block(b0).insts.push_back({MOpcode::Jmp, {MOperand::lbl(b1)}});
    // b1: jmp b0  (backward, non-adjacent -> kept)
    fn.block(b1).insts.push_back({MOpcode::Jmp, {MOperand::lbl(b0)}});
    // b2: ret
    fn.block(b2).insts.push_back({MOpcode::Ret, {}});

    optimizeFunction(fn, 1);
    check(fn.block(b0).insts.empty(), "branch: Jmp to next block removed");
    check(fn.block(b1).insts.size() == 1 &&
              fn.block(b1).insts[0].op == MOpcode::Jmp,
          "branch: Jmp to non-adjacent block kept");
}

// ---------------------------------------------------------------------------
int main() {
    testOptLevelZeroIsNoOp();
    testPeepholeSelfMove();
    testDeadCodeElim();
    testDeadChain();
    testTwoAddressKept();
    testSideEffectsPreserved();
    testBranchSimplify();

    if (g_failures == 0) {
        std::printf("\nall mir-opt tests passed\n");
        return 0;
    }
    std::printf("\n%d mir-opt test(s) FAILED\n", g_failures);
    return 1;
}
