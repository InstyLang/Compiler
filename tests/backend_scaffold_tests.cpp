// Unit tests for the x86-64 backend scaffold: ABI descriptors, live-interval
// computation, linear-scan allocation, and a small end-to-end lowering that
// produces a real object via the Fadec encoder.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <backend/abi.hpp>
#include <backend/coff_writer.hpp>
#include <backend/elf_writer.hpp>
#include <backend/lower.hpp>
#include <backend/machine_code.hpp>
#include <backend/machine_ir.hpp>
#include <backend/regalloc.hpp>

using namespace Backend;

static int g_failures = 0;
static void check(bool cond, const char* what) {
    std::printf(cond ? "ok: %s\n" : "FAIL: %s\n", what);
    if (!cond) ++g_failures;
}

// ---------------------------------------------------------------------------
static void testAbi() {
    AbiInfo sysv = makeAbi(Abi::SystemV);
    check(sysv.intArgRegs.size() == 6, "SysV has 6 integer arg registers");
    check(sysv.intArgRegs[0] == PhysReg::RDI, "SysV arg0 is RDI");
    check(sysv.intArgRegs[2] == PhysReg::RDX, "SysV arg2 is RDX");
    check(sysv.intReturnReg == PhysReg::RAX, "SysV return in RAX");
    check(sysv.shadowSpace == 0, "SysV has no shadow space");

    AbiInfo win = makeAbi(Abi::Win64);
    check(win.intArgRegs.size() == 4, "Win64 has 4 integer arg registers");
    check(win.intArgRegs[0] == PhysReg::RCX, "Win64 arg0 is RCX");
    check(win.intArgRegs[3] == PhysReg::R9, "Win64 arg3 is R9");
    check(win.shadowSpace == 32, "Win64 reserves 32B shadow space");

    // RSP/RBP must never be allocatable.
    auto notAllocatable = [](const AbiInfo& a, PhysReg r) {
        for (PhysReg x : a.allocatable)
            if (x == r) return false;
        return true;
    };
    check(notAllocatable(sysv, PhysReg::RSP) && notAllocatable(sysv, PhysReg::RBP),
          "SysV allocatable excludes RSP/RBP");
    check(notAllocatable(win, PhysReg::RSP) && notAllocatable(win, PhysReg::RBP),
          "Win64 allocatable excludes RSP/RBP");
    check(notAllocatable(sysv, PhysReg::R10) && notAllocatable(sysv, PhysReg::R11),
          "SysV allocatable excludes R10/R11 scratch");
    check(notAllocatable(win, PhysReg::R10) && notAllocatable(win, PhysReg::R11),
          "Win64 allocatable excludes R10/R11 scratch");
    for (const auto& abi : {sysv, win}) {
        bool scratchExcluded = true;
        for (auto x : abi.xmmAllocatable)
            if (x == XmmReg::XMM4 || x == XmmReg::XMM5) scratchExcluded = false;
        check(scratchExcluded, "XMM allocatable excludes XMM4/XMM5 lowering scratch");
    }

    check(abiForTriple("x86_64-pc-windows-msvc") == Abi::Win64,
          "triple windows -> Win64");
    check(abiForTriple("x86_64-pc-linux-gnu") == Abi::SystemV,
          "triple linux -> SystemV");
}

// ---------------------------------------------------------------------------
static void testIntervalsAndAlloc() {
    // Build a straight-line function using 3 simultaneously-live vregs.
    //   v0 = 1
    //   v1 = 2
    //   v2 = 3
    //   v0 = v0 + v1
    //   v0 = v0 + v2
    MFunction fn("test", Abi::SystemV);
    std::uint32_t bb = fn.addBlock("entry");
    VReg v0 = fn.newVReg(), v1 = fn.newVReg(), v2 = fn.newVReg();

    auto& B = fn.block(bb);
    B.insts.push_back({MOpcode::MovRI, {MOperand::defVReg(v0), MOperand::immediate(1)}});
    B.insts.push_back({MOpcode::MovRI, {MOperand::defVReg(v1), MOperand::immediate(2)}});
    B.insts.push_back({MOpcode::MovRI, {MOperand::defVReg(v2), MOperand::immediate(3)}});
    B.insts.push_back({MOpcode::Add, {MOperand::useDefVReg(v0), MOperand::useVReg(v1)}});
    B.insts.push_back({MOpcode::Add, {MOperand::useDefVReg(v0), MOperand::useVReg(v2)}});

    LinearScanAllocator ra(makeAbi(Abi::SystemV));
    auto intervals = ra.computeIntervals(fn);
    check(intervals.size() == 3, "three live intervals computed");
    // v0 spans the whole function (def at 0, last use at 4).
    bool v0ok = false;
    for (auto& iv : intervals)
        if (iv.vreg == v0) v0ok = (iv.start == 0 && iv.end == 4);
    check(v0ok, "v0 interval is [0,4]");

    Allocation alloc = ra.run(fn);
    // With 3 live values and a large allocatable pool, none should spill and
    // each must receive a distinct physical register.
    check(!alloc.anySpilled, "no spills with ample registers");
    PhysReg p0 = alloc.vregToPhys[v0], p1 = alloc.vregToPhys[v1], p2 = alloc.vregToPhys[v2];
    check(p0 != PhysReg::None && p1 != PhysReg::None && p2 != PhysReg::None,
          "all three vregs got a register");
    check(p0 != p1 && p0 != p2 && p1 != p2, "distinct registers for overlapping vregs");
}

// ---------------------------------------------------------------------------
static void testSpilling() {
    // Force spilling: create more simultaneously-live vregs than the pool has,
    // by defining N vregs then summing all of them into v0. With a tiny custom
    // ABI pool we guarantee pressure.
    AbiInfo tiny = makeAbi(Abi::SystemV);
    tiny.allocatable = {PhysReg::RAX, PhysReg::RCX, PhysReg::RDX};  // only 3 regs

    MFunction fn("spill", Abi::SystemV);
    std::uint32_t bb = fn.addBlock("entry");
    auto& B = fn.block(bb);

    const int N = 6;
    std::vector<VReg> vs;
    for (int i = 0; i < N; ++i) {
        VReg v = fn.newVReg();
        vs.push_back(v);
        B.insts.push_back({MOpcode::MovRI, {MOperand::defVReg(v), MOperand::immediate(i)}});
    }
    // Use them all late so they are simultaneously live.
    for (int i = 1; i < N; ++i) {
        B.insts.push_back({MOpcode::Add, {MOperand::useDefVReg(vs[0]), MOperand::useVReg(vs[i])}});
    }

    LinearScanAllocator ra(tiny);
    Allocation alloc = ra.run(fn);
    check(alloc.anySpilled, "spilling occurs under register pressure");
    int spilled = 0;
    for (VReg v : vs)
        if (alloc.vregToPhys[v] == PhysReg::None) ++spilled;
    check(spilled > 0, "at least one vreg spilled to a frame slot");
    check(fn.frameSlots().size() == static_cast<std::size_t>(spilled),
          "a frame slot was created per spill");
}

// ---------------------------------------------------------------------------
static void testEndToEndLowering() {
    // int f() { return 1 + 2; }  built in Machine IR, allocated + lowered.
    MFunction fn("f", Abi::SystemV);
    std::uint32_t bb = fn.addBlock("entry");
    auto& B = fn.block(bb);
    VReg a = fn.newVReg();
    VReg b = fn.newVReg();
    // The ABI return register is RAX; pin the result vreg there for the return.
    B.insts.push_back({MOpcode::MovRI, {MOperand::defVReg(a), MOperand::immediate(1)}});
    B.insts.push_back({MOpcode::MovRI, {MOperand::defVReg(b), MOperand::immediate(2)}});
    B.insts.push_back({MOpcode::Add, {MOperand::useDefVReg(a), MOperand::useVReg(b)}});
    // Move result into RAX (return reg) then ret.
    B.insts.push_back({MOpcode::MovRR, {MOperand::defPhys(PhysReg::RAX), MOperand::useVReg(a)}});
    B.insts.push_back({MOpcode::Ret, {}});

    LinearScanAllocator ra(makeAbi(Abi::SystemV));
    Allocation alloc = ra.run(fn);

    MachineCode code;
    Lowering low(code, makeAbi(Abi::SystemV));
    std::string err;
    bool ok = low.emit(fn, alloc, err);
    check(ok, "lowering succeeded");
    if (!ok) std::printf("  error: %s\n", err.c_str());
    check(!code.text.bytes.empty(), "lowering produced text bytes");
    check(code.findSymbol("f") >= 0, "function symbol 'f' defined");

    // Write both object formats so external tools can inspect the result.
    std::string e2;
    check(CoffWriter::write(code, "scaffold_f.obj", e2), "wrote COFF object");
    check(ElfWriter::write(code, "scaffold_f.o", e2), "wrote ELF object");
}

// ---------------------------------------------------------------------------
// Multi-block dataflow liveness: a value defined in the entry block and used
// only after a loop must stay live across the entire loop body, even though it
// is never referenced inside the loop. A correct dataflow allocator keeps its
// register reserved for the whole loop (interval spans entry..post-loop); a
// naive first-ref..last-ref scan over linear order would still span it here,
// but the key property we assert is that the loop-carried accumulator's
// interval covers the back-edge region (header..body), not just its textual
// references.
static void testMultiBlockLiveness() {
    // CFG (block indices = program layout order):
    //   0 entry:  acc = 7            ; i = 0           ; jmp 1
    //   1 header: cmp i, limit       ; jcc>= 3 (exit)
    //   2 body:   i = i + 1          ; jmp 1 (back-edge)
    //   3 exit:   result = acc + i   ; ret
    MFunction fn("looplive", Abi::SystemV);
    std::uint32_t entry = fn.addBlock("entry");
    std::uint32_t header = fn.addBlock("header");
    std::uint32_t body = fn.addBlock("body");
    std::uint32_t exit = fn.addBlock("exit");

    VReg acc = fn.newVReg();    // defined in entry, used only in exit
    VReg i = fn.newVReg();      // loop induction var (live across the loop)
    VReg limit = fn.newVReg();  // loop-invariant, live across the loop
    VReg res = fn.newVReg();

    auto& E = fn.block(entry);
    E.insts.push_back({MOpcode::MovRI, {MOperand::defVReg(acc), MOperand::immediate(7)}});
    E.insts.push_back({MOpcode::MovRI, {MOperand::defVReg(i), MOperand::immediate(0)}});
    E.insts.push_back({MOpcode::MovRI, {MOperand::defVReg(limit), MOperand::immediate(5)}});
    E.insts.push_back({MOpcode::Jmp, {MOperand::lbl(header)}});

    auto& H = fn.block(header);
    H.insts.push_back({MOpcode::Cmp, {MOperand::useVReg(i), MOperand::useVReg(limit)}});
    MInst jcc{MOpcode::Jcc, {MOperand::lbl(exit)}};
    jcc.cond = Cond::GE;
    H.insts.push_back(jcc);

    auto& Bdy = fn.block(body);
    Bdy.insts.push_back({MOpcode::Add, {MOperand::useDefVReg(i), MOperand::useVReg(limit)}});
    Bdy.insts.push_back({MOpcode::Jmp, {MOperand::lbl(header)}});

    auto& X = fn.block(exit);
    X.insts.push_back({MOpcode::MovRR, {MOperand::defVReg(res), MOperand::useVReg(acc)}});
    X.insts.push_back({MOpcode::Add, {MOperand::useDefVReg(res), MOperand::useVReg(i)}});
    X.insts.push_back({MOpcode::Ret, {}});

    LinearScanAllocator ra(makeAbi(Abi::SystemV));
    auto intervals = ra.computeIntervals(fn);

    // Program points: entry 0..3, header 4..5, body 6..7, exit 8..10.
    auto find = [&](VReg v) -> LiveInterval {
        for (auto& iv : intervals) if (iv.vreg == v) return iv;
        return {};
    };
    LiveInterval ivAcc = find(acc), ivI = find(i), ivLimit = find(limit);

    // acc: defined at pp0, used at pp8 -> must span [0,8] (alive across the loop).
    check(ivAcc.start == 0 && ivAcc.end >= 8, "acc live from entry through exit use");
    // i: induction var is live across the back-edge: live-out of body, live-in to
    // header. Its interval must cover the loop region (header pp4 .. body pp7) and
    // the post-loop use at pp9.
    check(ivI.start <= 4 && ivI.end >= 9, "induction var spans loop and post-loop use");
    // limit: loop-invariant used in header (pp4) and body (pp6); must stay live
    // across the whole loop, from its def in entry (pp2) to its last use (pp6).
    check(ivLimit.start <= 2 && ivLimit.end >= 6, "loop-invariant live across loop");

    // Allocate: with the full allocatable pool, the simultaneously-live values
    // (acc, i, limit across the loop) must each get a distinct register.
    Allocation alloc = ra.run(fn);
    check(!alloc.anySpilled, "no spill with ample registers");
    PhysReg pa = alloc.vregToPhys[acc], pi = alloc.vregToPhys[i],
            pl = alloc.vregToPhys[limit];
    check(pa != PhysReg::None && pi != PhysReg::None && pl != PhysReg::None,
          "loop-live vregs all assigned registers");
    check(pa != pi && pa != pl && pi != pl,
          "loop-live vregs get distinct registers (no clobber across the loop)");
}

// Execute the small MIR subset used below against the allocation's actual
// register/slot locations. Calls destroy volatile registers, and stores record
// observable values. This catches lost values rather than just checking that
// the allocator chose a particular preferred register. Spill reload/store
// semantics are modeled here; encoding and real spill lowering remain covered
// by the separate lowering/integration tests.
static std::vector<std::int64_t> executeAllocated(
    const MFunction& fn, const Allocation& alloc, const AbiInfo& abi,
    int observeCallArg = 0) {
    std::int64_t gp[16] = {}, xmm[16] = {};
    std::vector<std::int64_t> slots(fn.frameSlots().size(), 0), observed;
    auto location = [&](const MOperand& op) -> std::int64_t& {
        if (op.kind == OperandKind::VirtReg) {
            if (alloc.vregToSlot[op.vreg] != kInvalidVReg)
                return slots[alloc.vregToSlot[op.vreg]];
            if (fn.vregClass(op.vreg) == RegClass::XMM)
                return xmm[xmmIndex(alloc.vregToXmm[op.vreg])];
            return gp[regIndex(alloc.vregToPhys[op.vreg])];
        }
        if (op.xmm != XmmReg::None) return xmm[xmmIndex(op.xmm)];
        return gp[regIndex(op.phys)];
    };
    auto value = [&](const MOperand& op) -> std::int64_t {
        return op.kind == OperandKind::Imm ? op.imm : location(op);
    };
    std::size_t b = 0, i = 0;
    bool equal = false;
    for (int steps = 0; steps < 1000 && b < fn.blocks().size(); ++steps) {
        if (i == fn.blocks()[b].insts.size()) { ++b; i = 0; continue; }
        const auto& inst = fn.blocks()[b].insts[i++];
        const auto& ops = inst.operands;
        switch (inst.op) {
            case MOpcode::MovRI: case MOpcode::MovRR:
            case MOpcode::FConst: case MOpcode::FMovRR:
                location(ops[0]) = value(ops[1]); break;
            case MOpcode::Add:
                location(ops[0]) += value(ops[1]); break;
            case MOpcode::Cmp:
                equal = value(ops[0]) == value(ops[1]); break;
            case MOpcode::Store: case MOpcode::FStore:
                slots[ops[0].frameSlot] = value(ops[1]);
                observed.push_back(value(ops[1])); break;
            case MOpcode::Call: case MOpcode::CallImport: case MOpcode::CallIndirect:
                if (observeCallArg == 1) observed.push_back(gp[regIndex(abi.intArgRegs[0])]);
                if (observeCallArg == 2) observed.push_back(xmm[xmmIndex(abi.xmmArgRegs[0])]);
                for (auto r : abi.callerSaved) gp[regIndex(r)] = -999;
                for (auto r : abi.xmmCallerSaved) xmm[xmmIndex(r)] = -999;
                break;
            case MOpcode::Jcc:
                check(inst.cond == Cond::EQ || inst.cond == Cond::NE,
                      "MIR test executor supports branch condition");
                if ((inst.cond == Cond::EQ) == equal) { b = ops[0].label; i = 0; }
                break;
            case MOpcode::Jmp: b = ops[0].label; i = 0; break;
            case MOpcode::Ret: return observed;
            default:
                check(false, "MIR test executor supports opcode"); return observed;
        }
        // Non-call clobbers model scratch writes independently of operands.
        for (auto r : inst.clobbers) gp[regIndex(r)] = -999;
    }
    check(false, "allocated MIR execution terminates with Ret");
    return observed;
}

static void testConditionalBackedgeLiveness() {
    // The only backedge is the Jcc before a final Jmp. carried's last textual
    // use precedes temporary's definition, but its next dynamic use follows it.
    MFunction fn("conditional_backedge", Abi::SystemV);
    const auto entry = fn.addBlock("entry"), header = fn.addBlock("header");
    const auto body = fn.addBlock("body"), exit = fn.addBlock("exit");
    const auto carried = fn.newVReg(), count = fn.newVReg(), temporary = fn.newVReg();
    const auto output = fn.addFrameSlot(8, 8, false);
    fn.block(entry).insts = {
        {MOpcode::MovRI, {MOperand::defVReg(carried), MOperand::immediate(42)}},
        {MOpcode::MovRI, {MOperand::defVReg(count), MOperand::immediate(2)}},
        {MOpcode::Jmp, {MOperand::lbl(header)}}};
    fn.block(header).insts = {
        {MOpcode::Store, {MOperand::slot(output), MOperand::useVReg(carried)}},
        {MOpcode::Jmp, {MOperand::lbl(body)}}};
    MInst backedge{MOpcode::Jcc, {MOperand::lbl(header)}};
    backedge.cond = Cond::NE;
    fn.block(body).insts = {
        {MOpcode::MovRI, {MOperand::defVReg(temporary), MOperand::immediate(99)}},
        {MOpcode::Store, {MOperand::slot(output), MOperand::useVReg(temporary)}},
        {MOpcode::Add, {MOperand::useDefVReg(count), MOperand::immediate(-1)}},
        {MOpcode::Cmp, {MOperand::useVReg(count), MOperand::immediate(0)}},
        backedge, {MOpcode::Jmp, {MOperand::lbl(exit)}}};
    fn.block(exit).insts = {{MOpcode::Ret, {}}};
    auto abi = makeAbi(Abi::SystemV);
    abi.allocatable = {PhysReg::RBX, PhysReg::R12, PhysReg::R13};
    LinearScanAllocator ra(abi);
    bool spansBackedge = false;
    for (const auto& iv : ra.computeIntervals(fn))
        if (iv.vreg == carried) spansBackedge = iv.end >= 10;
    check(spansBackedge, "Jcc;Jmp keeps carried value live through backedge");
    const auto alloc = ra.run(fn);
    check(!alloc.anySpilled, "conditional-backedge test has enough registers");
    check(executeAllocated(fn, alloc, abi) == std::vector<std::int64_t>({42, 99, 42, 99}),
          "both loop iterations observe carried value despite body register writes");
}

static void testClobberPressure() {
    // One register: old dies before the clobber, incoming survives it. The
    // clobber makes that register illegal for incoming even under pressure.
    // With a non-clobbering version, a shorter incoming interval should still
    // be able to evict a longer old one (exercise the successful steal path).
    for (bool clobber : {false, true}) {
        MFunction fn("clobber_pressure", Abi::SystemV);
        const auto b = fn.addBlock("entry");
        const auto old = fn.newVReg(), incoming = fn.newVReg();
        const auto output = fn.addFrameSlot(8, 8, false);
        MInst scratch{MOpcode::MovRI,
            {MOperand::defPhys(PhysReg::R10), MOperand::immediate(0)}};
        if (clobber) scratch.clobbers = {PhysReg::RAX};
        const auto first = clobber ? old : incoming;
        const auto last = clobber ? incoming : old;
        fn.block(b).insts = {
            {MOpcode::MovRI, {MOperand::defVReg(old), MOperand::immediate(17)}},
            {MOpcode::MovRI, {MOperand::defVReg(incoming), MOperand::immediate(25)}},
            {MOpcode::Store, {MOperand::slot(output), MOperand::useVReg(first)}},
            scratch,
            {MOpcode::Store, {MOperand::slot(output), MOperand::useVReg(last)}},
            // Kill the unrelated implicit return-register live-in.
            {MOpcode::MovRI, {MOperand::defPhys(PhysReg::RAX), MOperand::immediate(0)}},
            {MOpcode::Ret, {}}};
        auto abi = makeAbi(Abi::SystemV);
        abi.allocatable = {PhysReg::RAX};
        const auto alloc = LinearScanAllocator(abi).run(fn);
        const auto spilled = clobber ? incoming : old;
        check(alloc.anySpilled && alloc.vregToSlot[spilled] != kInvalidVReg,
              clobber ? "clobber collision forces incoming GP value to spill"
                      : "shorter GP value evicts longer active value");
        check(alloc.vregToPhys[clobber ? old : incoming] == PhysReg::RAX,
              "non-colliding GP interval retains the sole register");
        check(executeAllocated(fn, alloc, abi) == (clobber
                  ? std::vector<std::int64_t>({17, 25})
                  : std::vector<std::int64_t>({25, 17})),
              "GP values survive forced spilling and scratch clobbers");
    }
}

static MInst testCall(MFunction& fn, MOpcode op, const AbiInfo& abi) {
    MInst call{op, {MOperand::sym("callee")}};
    if (op == MOpcode::CallImport) call.operands.push_back(MOperand::sym("test.dll"));
    if (op == MOpcode::CallIndirect)
        call.operands = {MOperand::slot(fn.addFrameSlot(8, 8, false))};
    call.clobbers = abi.callerSaved;
    return call;
}

static void testXmmClobberPressure() {
    for (bool collision : {false, true}) {
        auto abi = makeAbi(Abi::SystemV);
        abi.xmmAllocatable = {XmmReg::XMM8};
        MFunction fn("xmm_pressure", Abi::SystemV);
        const auto b = fn.addBlock("entry");
        const auto old = fn.newVReg(RegClass::XMM), incoming = fn.newVReg(RegClass::XMM);
        const auto output = fn.addFrameSlot(8, 8, false);
        fn.block(b).insts = {
            {MOpcode::FConst, {MOperand::defVReg(old), MOperand::immediate(17)}},
            {MOpcode::FConst, {MOperand::defVReg(incoming), MOperand::immediate(25)}},
            {MOpcode::FStore, {MOperand::slot(output), MOperand::useVReg(collision ? old : incoming)}},
            {MOpcode::FConst, {MOperand::defPhysXmm(collision ? XmmReg::XMM8 : XmmReg::XMM9),
                              MOperand::immediate(-999)}},
            {MOpcode::FStore, {MOperand::slot(output), MOperand::useVReg(collision ? incoming : old)}},
            {MOpcode::Ret, {}}};
        const auto alloc = LinearScanAllocator(abi).run(fn);
        check(alloc.vregToSlot[collision ? incoming : old] != kInvalidVReg,
              collision ? "fixed XMM write forces colliding incoming interval to spill"
                        : "shorter XMM interval evicts longer active interval");
        check(alloc.vregToXmm[collision ? old : incoming] == XmmReg::XMM8,
              "non-colliding XMM interval retains the sole register");
        check(executeAllocated(fn, alloc, abi) == (collision
                  ? std::vector<std::int64_t>({17, 25})
                  : std::vector<std::int64_t>({25, 17})),
              "XMM values survive pressure and fixed-register writes");
    }
}

static void testXmmCallLiveness() {
    for (auto convention : {Abi::SystemV, Abi::Win64}) {
        for (auto op : {MOpcode::Call, MOpcode::CallImport, MOpcode::CallIndirect}) {
            const auto abi = makeAbi(convention);
            MFunction fn("xmm_call_live", convention);
            const auto b = fn.addBlock("entry");
            const auto x = fn.newVReg(RegClass::XMM);
            const auto y = fn.newVReg(RegClass::XMM);
            const auto output = fn.addFrameSlot(8, 8, false);
            fn.block(b).insts = {
                {MOpcode::FConst, {MOperand::defVReg(x), MOperand::immediate(0x3ff0000000000000LL)}},
                {MOpcode::FConst, {MOperand::defVReg(y), MOperand::immediate(0x4000000000000000LL)}},
                testCall(fn, op, abi),
                {MOpcode::FStore, {MOperand::slot(output), MOperand::useVReg(x)}},
                {MOpcode::FStore, {MOperand::slot(output), MOperand::useVReg(y)}},
                {MOpcode::Ret, {}}};
            std::printf("XMM call case: %s / %s\n",
                convention == Abi::SystemV ? "SysV" : "Win64",
                op == MOpcode::Call ? "direct" : op == MOpcode::CallImport ? "import" : "indirect");
            const auto alloc = LinearScanAllocator(abi).run(fn);
            if (convention == Abi::SystemV) {
                check(alloc.vregToSlot[x] != kInvalidVReg && alloc.vregToSlot[y] != kInvalidVReg,
                      "SysV live-across-call floats both spill");
            } else {
                check(!alloc.anySpilled && alloc.vregToXmm[x] != alloc.vregToXmm[y],
                      "Win64 floats use distinct preserved XMM registers");
            }
            check(executeAllocated(fn, alloc, abi) == std::vector<std::int64_t>(
                      {0x3ff0000000000000LL, 0x4000000000000000LL}),
                  "both floating values survive volatile XMM destruction at call");
        }
    }
}

static void testFixedArgumentLiveness() {
    for (bool floating : {false, true}) {
        for (bool acrossBlock : {false, true}) {
            auto abi = makeAbi(Abi::SystemV);
            abi.allocatable = {PhysReg::RDI};
            abi.xmmAllocatable = {XmmReg::XMM0};
            MFunction fn("fixed_argument", Abi::SystemV);
            const auto entry = fn.addBlock("entry");
            const auto consume = acrossBlock ? fn.addBlock("consume") : entry;
            const auto v = fn.newVReg(floating ? RegClass::XMM : RegClass::GPR);
            const auto output = fn.addFrameSlot(8, 8, false);
            const auto def = floating ? MOperand::defPhysXmm(XmmReg::XMM0)
                                      : MOperand::defPhys(PhysReg::RDI);
            const auto use = floating ? MOperand::usePhysXmm(XmmReg::XMM0)
                                      : MOperand::usePhys(PhysReg::RDI);
            const auto mov = floating ? MOpcode::FConst : MOpcode::MovRI;
            const auto store = floating ? MOpcode::FStore : MOpcode::Store;
            fn.block(entry).insts = {
                {mov, {def, MOperand::immediate(42)}},
                {mov, {MOperand::defVReg(v), MOperand::immediate(99)}},
                {store, {MOperand::slot(output), MOperand::useVReg(v)}}};
            if (acrossBlock)
                fn.block(entry).insts.push_back({MOpcode::Jmp, {MOperand::lbl(consume)}});
            auto& tail = fn.block(consume).insts;
            // Cross-block case tests an explicit fixed use; same-block case
            // relies solely on the call's implicit ABI consumption.
            if (acrossBlock) tail.push_back({store, {MOperand::slot(output), use}});
            tail.push_back(testCall(fn, MOpcode::Call, abi));
            tail.push_back({MOpcode::Ret, {}});
            const auto alloc = LinearScanAllocator(abi).run(fn);
            check(alloc.vregToSlot[v] != kInvalidVReg,
                  "temporary wholly inside fixed argument lifetime spills");
            const auto expected = acrossBlock ? std::vector<std::int64_t>({99, 42, 42})
                                              : std::vector<std::int64_t>({99, 42});
            check(executeAllocated(fn, alloc, abi, floating ? 2 : 1) == expected,
                  "fixed GP/XMM argument survives intervening temporary, including CFG edge");
        }
    }
}

int main() {
    testAbi();
    testIntervalsAndAlloc();
    testSpilling();
    testMultiBlockLiveness();
    testConditionalBackedgeLiveness();
    testClobberPressure();
    testXmmClobberPressure();
    testXmmCallLiveness();
    testFixedArgumentLiveness();
    testEndToEndLowering();
    std::printf("\n%s (%d failure(s))\n", g_failures == 0 ? "PASSED" : "FAILED",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
