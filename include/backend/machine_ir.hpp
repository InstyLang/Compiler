#pragma once

// Low-level Machine IR for the custom x86-64 backend.
//
// This is the representation the register allocator operates on. It sits below
// any high-level/SSA IR and above instruction encoding: instruction selection
// produces an MFunction of virtual-register machine instructions; the register
// allocator rewrites virtual registers to physical ones (inserting spills);
// finally lowering emits the physical instructions via the Fadec Encoder.
//
// Operands carry explicit def/use roles so the allocator can compute live
// intervals without opcode-specific knowledge. Calls list their clobbered
// physical registers explicitly.

#include <cstdint>
#include <string>
#include <vector>

#include <backend/abi.hpp>
#include <backend/asm_block.hpp>
#include <backend/reg.hpp>

namespace Backend {

// A virtual register: a dense integer id assigned by instruction selection.
using VReg = std::uint32_t;
constexpr VReg kInvalidVReg = 0xFFFFFFFFu;

enum class OperandKind : std::uint8_t {
    None,
    VirtReg,   // a virtual register (allocator assigns a PhysReg)
    PhysReg,   // a fixed physical register (ABI constraint, clobber, etc.)
    Imm,       // an integer immediate
    FrameSlot, // a stack slot index (for explicit locals / spills)
    Symbol,    // a named symbol (call target, global address)
    Label      // a basic-block label index (branch target)
};

// How an operand participates: read, written, or both. Drives liveness.
enum class OperandRole : std::uint8_t { Use, Def, UseDef };

struct MOperand {
    OperandKind kind = OperandKind::None;
    OperandRole role = OperandRole::Use;

    VReg vreg = kInvalidVReg;             // VirtReg
    PhysReg phys = PhysReg::None;         // PhysReg (or assigned for VirtReg)
    XmmReg xmm = XmmReg::None;            // assigned XMM (for XMM-class VirtReg/PhysReg)
    std::int64_t imm = 0;                 // Imm
    std::uint32_t frameSlot = 0;          // FrameSlot
    std::string symbol;                   // Symbol
    std::uint32_t label = 0;              // Label

    static MOperand useVReg(VReg v) { return {OperandKind::VirtReg, OperandRole::Use, v}; }
    static MOperand defVReg(VReg v) { return {OperandKind::VirtReg, OperandRole::Def, v}; }
    static MOperand useDefVReg(VReg v) { return {OperandKind::VirtReg, OperandRole::UseDef, v}; }
    static MOperand usePhys(PhysReg p) {
        MOperand o; o.kind = OperandKind::PhysReg; o.role = OperandRole::Use; o.phys = p; return o;
    }
    static MOperand defPhys(PhysReg p) {
        MOperand o; o.kind = OperandKind::PhysReg; o.role = OperandRole::Def; o.phys = p; return o;
    }
    // Fixed XMM physical operands (ABI arg/return registers, clobbers).
    static MOperand usePhysXmm(XmmReg x) {
        MOperand o; o.kind = OperandKind::PhysReg; o.role = OperandRole::Use; o.xmm = x; return o;
    }
    static MOperand defPhysXmm(XmmReg x) {
        MOperand o; o.kind = OperandKind::PhysReg; o.role = OperandRole::Def; o.xmm = x; return o;
    }
    static MOperand immediate(std::int64_t v) {
        MOperand o; o.kind = OperandKind::Imm; o.imm = v; return o;
    }
    static MOperand sym(std::string name) {
        MOperand o; o.kind = OperandKind::Symbol; o.symbol = std::move(name); return o;
    }
    static MOperand slot(std::uint32_t idx) {
        MOperand o; o.kind = OperandKind::FrameSlot; o.frameSlot = idx; return o;
    }
    static MOperand lbl(std::uint32_t idx) {
        MOperand o; o.kind = OperandKind::Label; o.label = idx; return o;
    }
};

// A small, deliberately minimal opcode set for the scaffold. Operands are held
// generically in MInst::operands; the opcode fixes their count/meaning.
enum class MOpcode : std::uint16_t {
    // data movement
    MovRR,        // def0 = use1            (reg/reg or reg<-imm via Imm operand)
    MovRI,        // def0 = imm
    Load,         // def0 = sext/zext [frameSlot1] to 64 bits  (width/signed in MInst)
    Store,        // [frameSlot0] = low `width` bytes of use1
    StoreOutgoing,// [rsp + imm0] = use1    (outgoing stack argument, 8 bytes)
    Ext,          // usedef0 = sext/zext low `width` bytes of usedef0 to 64 bits
    Lea,          // def0 = address-of symbol1
    LeaSlot,      // def0 = address-of frameSlot1 (lea reg,[rbp+off]) -- &local
    LeaDisp,      // def0 = base1 + imm2  (lea reg,[base + disp]) -- &base[const]
    LoadInd,      // def0 = sext/zext [base1 + imm2] to 64 bits (width/signed) -- *p
    StoreInd,     // [base0 + imm1] = low `width` bytes of use2 -- *p = v
    LeaIndex,     // def0 = base1 + index2*scale + imm3  (lea reg,[base+idx*sc+disp]).
                  // Folds scaled-index + constant displacement into one SIB lea.
                  // scale (1/2/4/8) is in MInst.scale; imm3 is the byte displacement.

    // integer arithmetic (two-address: def0 is also a use)
    Add,      // usedef0 += use1
    Sub,      // usedef0 -= use1
    IMul,     // usedef0 *= use1
    And,      // usedef0 &= use1
    Or,       // usedef0 |= use1
    Xor,      // usedef0 ^= use1
    Cmp,      // use0 ? use1 (sets flags)

    // unary (in-place: usedef0)
    Neg,      // usedef0 = -usedef0
    Not,      // usedef0 = ~usedef0

    // SetCC: def0 = (flags satisfy `cond`) ? 1 : 0, zero-extended to 64 bits.
    // Must follow a Cmp; the condition is in `cond`.
    SetCC,

    // shifts: usedef0 = usedef0 <shift> use1 (shift count); lowering parks the
    // count in CL. `isSigned` selects arithmetic vs logical right shift.
    Shl,      // usedef0 <<= use1
    Shr,      // usedef0 >>= use1  (logical if !isSigned, arithmetic if isSigned)

    // division: def0 = use1 / use2 (Div) or use1 % use2 (Mod). Lowering uses the
    // RDX:RAX dividend convention; `isSigned` picks idiv/cqo vs div/xor-edx.
    // Clobbers RAX and RDX (recorded in `clobbers`).
    Div,      // def0 = use1 / use2
    Mod,      // def0 = use1 % use2

    // unsigned 64x64 -> high 64 bits: def0 = (use1 * use2) >> 64. Lowering uses
    // `mul` (RDX:RAX), taking the RDX high word. Used to build 128-bit multiply.
    // Clobbers RAX and RDX (recorded in `clobbers`).
    UMulHi,

    // control flow
    Jmp,      // -> label0
    Jcc,      // cond -> label0  (cond stored in `cond`)
    Call,     // call symbol0 ; clobbers caller-saved (in `clobbers`)
    CallImport,// call qword [rip + IAT(symbol0)] ; symbol1 = dll name. Indirect
              // call to a DLL-imported function; clobbers caller-saved.
    CallIndirect,// call qword [rbp + frameSlot0] ; indirect call through a target
              // address spilled to a frame slot (fnCall intrinsic). The target is
              // materialized to its own slot before arg marshalling so it cannot be
              // clobbered by argument-register setup. Clobbers caller-saved.
    Syscall,  // syscall ; args preloaded into rax/rdi/rsi/rdx/r10/r8/r9, result rax;
              // clobbers caller-saved (in `clobbers`; kernel clobbers rcx/r11)
    Ret,      // return (value already in the ABI return reg)

    // --- atomics / memory ordering / fixed inline asm ----------------------
    // x86-64 naturally-aligned <=8-byte loads/stores are already atomic, so
    // atomicLoad/atomicStore reuse LoadInd/StoreInd; atomicStore additionally
    // emits a Fence for sequential-consistency. The ops below cover the cases
    // that need real lock-prefixed encodings or a fence.
    Fence,    // mfence (full barrier); also used by atomicStore/atomicFence.
    AtomicXAdd,  // lock xadd [base1 + imm2], val(usedef3); def0 = old value.
                 // `width` selects 1/2/4/8. The value operand (operands[3]) is
                 // use-def: holds the addend on input, the fetched value on output.
    AtomicCmpXchg,// lock cmpxchg [base2 + imm3], desired(use4):
                 //   expected(operand1, usedef) is moved into RAX; on success the
                 //   memory takes `desired`; def0 = ZF (1 on success) zero-extended.
                 // Clobbers RAX (recorded in `clobbers`).
    AsmFixed, // a recognized fixed inline-asm template, selected by `imm` of
              // operand0: 0=nop, 1=syscall, 2=int3, 3=mfence, 4=ud2, 5=pause,
              // 6=cpuid, 7=hlt. Carries no value (void).
    AsmBlock, // a raw `asm( ... )` block. `imm` of operand0 indexes
              // MFunction::asmBlocks. The instructions are assembled at lowering
              // time, once frame slots have addresses; `clobbers` carries the
              // registers the block is allowed to destroy.

    // --- floating point (double / f64) -------------------------------------
    // Operate on XMM-class virtual registers. Scalar float; the MInst `width`
    // field selects precision: width==8 -> double (movsd/addsd/...), width==4 ->
    // single (movss/addss/...). FConst's imm holds the raw bit pattern (64- or
    // 32-bit) of the constant for that precision.
    FLoad,    // def0(xmm) = [frameSlot1]            (movsd xmm, [rbp+off])
    FStore,   // [frameSlot0] = use1(xmm)            (movsd [rbp+off], xmm)
    FLoadInd, // def0(xmm) = [base1 + imm2]          (movsd xmm, [base+disp])
    FStoreInd,// [base0 + imm1] = use2(xmm)          (movsd [base+disp], xmm)
    FStoreOutgoing,// [rsp + imm0] = use1(xmm)       (outgoing stack arg, double)
    FMovRR,   // def0(xmm) = use1(xmm)               (movsd xmm, xmm)
    FConst,   // def0(xmm) = bit-pattern imm1        (load double constant)
    FAdd,     // usedef0(xmm) += use1(xmm)
    FSub,     // usedef0(xmm) -= use1(xmm)
    FMul,     // usedef0(xmm) *= use1(xmm)
    FDiv,     // usedef0(xmm) /= use1(xmm)
    FNeg,     // usedef0(xmm) = -usedef0  (xor sign bit)
    FCmp,     // use0(xmm) ? use1(xmm)  (ucomisd/ucomiss; sets flags) -> SetCC
    CvtI2F,   // def0(xmm) = (float)  use1(gpr)      (cvtsi2sd / cvtsi2ss by width)
    CvtF2I,   // def0(gpr) = (i64)    use1(xmm)      (cvttsd2si / cvttss2si, trunc)
    CvtF2F,   // def0(xmm) = convert  use1(xmm)      (cvtss2sd/cvtsd2ss; width=DEST)
    CvtF16ToF32, // def0(xmm:f32) = (f32) use1(xmm: f16 in low 16 bits) (vcvtph2ps)
    CvtF32ToF16, // def0(xmm: f16 in low 16 bits) = (f16) use1(xmm:f32) (vcvtps2ph,imm=0)
    StoreXmmLo16,// [frameSlot0] = low 16 bits of use1(xmm)  (movd->gpr, mov16 [rbp])
    FMovToGpr,// def0(gpr) = bitcast use1(xmm)       (movq gpr, xmm)
    FMovFromGpr,// def0(xmm) = bitcast use1(gpr)      (movq xmm, gpr)
    PXorRR,   // usedef0(xmm) ^= use1(xmm)           (pxor xmm, xmm; 128-bit)
    AesEncRR  // usedef0(xmm) = AES round of xmm0 with round key use1(xmm) (aesenc)
};

// Condition codes. The signed forms (LT/LE/GT/GE) lower to jl/jle/jg/jge and
// setl/setle/setg/setge; the unsigned forms (ULT/ULE/UGT/UGE) lower to
// jb/jbe/ja/jae and setb/setbe/seta/setae. EQ/NE are sign-agnostic.
enum class Cond : std::uint8_t { EQ, NE, LT, LE, GT, GE, ULT, ULE, UGT, UGE };

// Inverts a condition (true <-> false), used to branch on the negated test.
inline Cond invertCond(Cond c) {
    switch (c) {
        case Cond::EQ:  return Cond::NE;
        case Cond::NE:  return Cond::EQ;
        case Cond::LT:  return Cond::GE;
        case Cond::LE:  return Cond::GT;
        case Cond::GT:  return Cond::LE;
        case Cond::GE:  return Cond::LT;
        case Cond::ULT: return Cond::UGE;
        case Cond::ULE: return Cond::UGT;
        case Cond::UGT: return Cond::ULE;
        case Cond::UGE: return Cond::ULT;
    }
    return Cond::EQ;
}

struct MInst {
    MOpcode op;
    std::vector<MOperand> operands;
    Cond cond = Cond::EQ;                 // for Jcc
    std::vector<PhysReg> clobbers;        // for Call: caller-saved registers
    // For width-aware ops (Load/Store/Ext): the value's width in bytes (1/2/4/8)
    // and whether it is signed (movsx vs movzx). Defaults model a full 64-bit
    // signed value, matching the original 8-byte-only behavior.
    std::uint8_t width = 8;
    bool isSigned = true;
    // For LeaIndex: the SIB scale factor (1/2/4/8) folded into the lea.
    std::uint8_t scale = 0;
};

struct MBasicBlock {
    std::string label;                    // assembler-visible label (optional)
    std::vector<MInst> insts;
};

// A stack slot reserved in the frame (explicit local or a spill slot).
struct FrameSlot {
    std::uint32_t index = 0;
    unsigned size = 8;        // bytes
    unsigned align = 8;       // bytes
    bool isSpill = false;     // created by the allocator vs. an explicit local
    bool isIncoming = false;  // caller-frame arg slot (rbpOffset preset, positive)
    // Filled in by frame layout: offset from RBP (negative, below the saved RBP).
    std::int64_t rbpOffset = 0;
};

// Computed by frame layout; consumed by prologue/epilogue + lowering.
struct FrameLayout {
    bool laidOut = false;
    std::int64_t localsSize = 0;          // bytes for slots
    std::int64_t outgoingSize = 0;        // bytes reserved for outgoing call args
    std::int64_t frameSize = 0;           // total subtracted from RSP (aligned)
    std::vector<PhysReg> savedCalleeRegs; // GP callee-saved actually used
    std::vector<XmmReg> savedXmmRegs;     // XMM callee-saved actually used
    // RBP-relative offset of the first saved-XMM slot (each is 8 bytes, growing
    // downward: savedXmmRegs[k] lives at savedXmmBaseOffset - 8*k).
    std::int64_t savedXmmBaseOffset = 0;
};

class MFunction {
public:
    explicit MFunction(std::string name, Abi abi)
        : name_(std::move(name)), abi_(abi) {}

    const std::string& name() const { return name_; }
    Abi abi() const { return abi_; }

    // --- Function attributes (from source `[...]` decorations) ----------------
    // Symbol linkage requested for this function's defined symbol. `Default`
    // lets the lowering pick (Global if exported/no explicit choice, else its
    // usual rule). Maps onto Backend::SymbolBinding at emit time.
        // LinkOnce is for a body every module emits identically (a monomorphized
    // generic): the linker folds the copies. Weak is the different, overridable
    // kind, where a strong definition elsewhere wins.
    enum class Linkage { Default, Internal, External, Weak, LinkOnce };

    // `[naked]`: emit the body verbatim with no compiler-generated prologue,
    // epilogue, or incoming-argument spills. The function is responsible for its
    // own stack frame and return (typically all-inline-asm).
    bool naked = false;
    // `[section("name")]`: place this function's machine code in a named code
    // section instead of the primary .text. Empty => primary .text.
    std::string customSection;
    // Raw `asm( ... )` blocks in this function, referenced by index from an
    // MOpcode::AsmBlock instruction. Held here rather than in the instruction
    // because assembling needs frame-slot addresses, which only exist after
    // lowering computes the frame layout.
    std::vector<AsmProgram> asmBlocks;
    // Requested symbol linkage (see Linkage).
    Linkage linkage = Linkage::Default;
    // True when the source marked the function exported (drives Default linkage).
    bool exported = false;

    VReg newVReg() { vregClass_.push_back(RegClass::GPR); return nextVReg_++; }
    VReg newVReg(RegClass cls) { vregClass_.push_back(cls); return nextVReg_++; }
    std::uint32_t numVRegs() const { return nextVReg_; }
    RegClass vregClass(VReg v) const {
        return v < vregClass_.size() ? vregClass_[v] : RegClass::GPR;
    }

    std::uint32_t addBlock(std::string label = "") {
        blocks_.push_back(MBasicBlock{std::move(label), {}});
        return static_cast<std::uint32_t>(blocks_.size() - 1);
    }
    MBasicBlock& block(std::uint32_t i) { return blocks_[i]; }
    const std::vector<MBasicBlock>& blocks() const { return blocks_; }
    std::vector<MBasicBlock>& blocks() { return blocks_; }

    std::uint32_t addFrameSlot(unsigned size, unsigned align, bool isSpill) {
        FrameSlot s;
        s.index = static_cast<std::uint32_t>(slots_.size());
        s.size = size;
        s.align = align;
        s.isSpill = isSpill;
        slots_.push_back(s);
        return s.index;
    }
    std::vector<FrameSlot>& frameSlots() { return slots_; }
    const std::vector<FrameSlot>& frameSlots() const { return slots_; }

    FrameLayout& layout() { return layout_; }
    const FrameLayout& layout() const { return layout_; }

    // Bytes a callee will read above [rsp] for stack-passed outgoing args at the
    // largest call site (excludes register args; includes Win64 shadow space).
    // Selection records the maximum so frame layout can reserve it once.
    std::int64_t maxOutgoingArgBytes() const { return maxOutgoing_; }
    void noteOutgoingArgBytes(std::int64_t bytes) {
        if (bytes > maxOutgoing_) maxOutgoing_ = bytes;
    }

    // String literals referenced by this function. Each is a NUL-terminated byte
    // sequence the lowering interns into .rodata (deduped by content) and defines
    // a local symbol for; a `Lea def, symbol(symbol)` instruction loads its
    // RIP-relative address. `bytes` excludes the implicit trailing NUL.
    struct StringConstant {
        std::string symbol;  // backend symbol name (e.g. ".Lstr.0")
        std::string bytes;   // raw literal contents (NUL appended at emit time)
        unsigned align = 1;  // required alignment of the blob in .rodata
        bool appendNul = true;  // append a single 0 byte after `bytes` at emit time
    };
    std::uint32_t addStringConstant(std::string symbol, std::string bytes) {
        strings_.push_back(StringConstant{std::move(symbol), std::move(bytes), 1, true});
        return static_cast<std::uint32_t>(strings_.size() - 1);
    }
    // Intern a raw blob (e.g. a UTF-16 wide string whose bytes already include
    // their own terminator) with a specific alignment and no implicit NUL.
    std::uint32_t addRawConstant(std::string symbol, std::string bytes, unsigned align) {
        strings_.push_back(
            StringConstant{std::move(symbol), std::move(bytes), align, /*appendNul=*/false});
        return static_cast<std::uint32_t>(strings_.size() - 1);
    }
    const std::vector<StringConstant>& stringConstants() const { return strings_; }

private:
    std::string name_;
    Abi abi_;
    VReg nextVReg_ = 0;
    std::vector<RegClass> vregClass_;
    std::vector<MBasicBlock> blocks_;
    std::vector<FrameSlot> slots_;
    FrameLayout layout_;
    std::int64_t maxOutgoing_ = 0;
    std::vector<StringConstant> strings_;
};

}  // namespace Backend

