#include <backend/wasm_emit.hpp>

#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <backend/abi.hpp>
#include <backend/const_eval.hpp>
#include <backend/isel.hpp>
#include <backend/machine_ir.hpp>
#include <backend/mir_opt.hpp>
#include <backend/wasm_writer.hpp>
#include <sema/checker.hpp>

namespace Backend::Wasm {

namespace {

// WASI preview1 is the OS interface for a hosted wasm target; a "command"
// module imports from this pseudo-module and exports `_start`.
constexpr const char* kWasiModule = "wasi_snapshot_preview1";

// --- linear memory layout --------------------------------------------------
//
//   0         .. kDataBase   reserved. Nothing is allocated here so a null
//                            dereference lands on an address no object owns.
//                            wasm does not fault on address 0, so this is a
//                            debugging aid rather than a guarantee.
//   kDataBase .. dataEnd     static data: string literals then module globals.
//                            Initialized bytes come first as one data segment;
//                            zero-initialized globals follow and need no
//                            segment at all, since wasm zeroes memory for us
//                            (this is the .bss equivalent, for free).
//   dataEnd   .. stackTop    shadow stack, growing DOWN from stackTop.
//   stackTop  ..             heap, bump-allocated upward, growing linear memory
//                            with memory.grow as needed.
constexpr std::uint32_t kDataBase = 1024;
constexpr std::uint32_t kStackSize = 64 * 1024;
constexpr std::uint32_t kPageSize = 65536;
constexpr std::uint32_t kStackAlign = 16;
// The allocator hands out at least this alignment, matching what mmap and
// HeapAlloc guarantee on the native targets.
constexpr std::uint32_t kHeapAlign = 16;

std::uint32_t alignUp(std::uint32_t value, std::uint32_t align) {
    if (align <= 1) return value;
    return (value + align - 1) / align * align;
}

// Where every data symbol lives, and how large memory has to be.
struct DataLayout {
    std::unordered_map<std::string, std::uint32_t> symbolAddress;
    // Bytes to place at kDataBase as a single active data segment.
    std::vector<std::uint8_t> initialData;
    std::uint32_t dataEnd = kDataBase;
    std::uint32_t stackTop = 0;
    std::uint32_t heapBase = 0;
    std::uint32_t memoryPages = 1;

    bool addressOf(const std::string& symbol, std::uint32_t& out) const {
        auto it = symbolAddress.find(symbol);
        if (it == symbolAddress.end()) return false;
        out = it->second;
        return true;
    }
};

// The load/store instruction for an access of `width` bytes producing/consuming
// an i64. The narrow loads sign- or zero-extend as part of the access, so no
// separate extension step is needed.
bool loadOpFor(std::uint8_t width, bool isSigned, Op& out) {
    switch (width) {
        case 1: out = isSigned ? Op::I64Load8S : Op::I64Load8U; return true;
        case 2: out = isSigned ? Op::I64Load16S : Op::I64Load16U; return true;
        case 4: out = isSigned ? Op::I64Load32S : Op::I64Load32U; return true;
        case 8: out = Op::I64Load; return true;
        default: return false;
    }
}

bool storeOpFor(std::uint8_t width, Op& out) {
    switch (width) {
        case 1: out = Op::I64Store8; return true;
        case 2: out = Op::I64Store16; return true;
        case 4: out = Op::I64Store32; return true;
        case 8: out = Op::I64Store; return true;
        default: return false;
    }
}

// Where one piece of one argument arrives. System V splits arguments across two
// register files and spills the rest to the caller's frame, so a slot names which
// of the three it is.
struct ArgSlot {
    bool isXmm = false;
    bool isStack = false;
    PhysReg gp = PhysReg::None;
    XmmReg xmm = XmmReg::None;
    // For a stack argument: its index among the stack arguments, and the byte
    // offset the selector uses for it relative to the frame base.
    std::uint32_t stackIndex = 0;
    std::uint32_t stackOffset = 0;

    static ArgSlot integer(PhysReg r) {
        ArgSlot s;
        s.gp = r;
        return s;
    }
    static ArgSlot sse(XmmReg r) {
        ArgSlot s;
        s.isXmm = true;
        s.xmm = r;
        return s;
    }
    // wasm passes everything as parameters, but the selector has already decided
    // these travel through memory, so the emitter has to follow the same layout:
    // the caller writes them into its outgoing area and the callee reads them
    // back from what x86 would call the caller's frame.
    static ArgSlot stack(std::uint32_t index, std::uint32_t offset) {
        ArgSlot s;
        s.isStack = true;
        s.stackIndex = index;
        s.stackOffset = offset;
        return s;
    }
};

// An explanation for the constructs wasm genuinely cannot express, so the error
// says what the constraint is and what to reach for instead, rather than naming an
// internal opcode the user has never heard of. Returns nullptr when there is
// nothing better to say than the opcode name.
const char* unsupportedReason(MOpcode op) {
    switch (op) {
        case MOpcode::Syscall:
            return "a kernel syscall has no WebAssembly equivalent: a wasm module reaches "
                   "its host through imports, not a trap instruction. Use the "
                   "wasi::sys bindings directly, or std::io / std::fs, which pick "
                   "their platform with `#if @targetIs(...)`. Note that @print and "
                   "@println also lower to a syscall";
        case MOpcode::Fence:
        case MOpcode::AtomicXAdd:
        case MOpcode::AtomicCmpXchg:
            return "atomics and memory fences require the WebAssembly threads "
                   "proposal, which this backend does not emit yet. Single-threaded "
                   "code does not need them: a wasm module has no concurrent "
                   "observer unless the host provides one";
        case MOpcode::AesEncRR:
            return "internal: hardware AES was requested, but the wasm path always "
                   "selects the portable hash";
        case MOpcode::StoreOutgoing:
        case MOpcode::FStoreOutgoing:
            return "internal: an outgoing stack argument was emitted for a function "
                   "with no frame";
        default:
            return nullptr;
    }
}

// Everything needed to emit a call to one function.
//
// wasm passes arguments on the operand stack, but the selector has already
// distributed them into ABI argument registers before the call. `paramRegs`
// replays that assignment so the emitter can read each argument back out of the
// local standing in for its register, in parameter order.
struct Callee {
    std::uint32_t funcIndex = 0;
    FuncType signature;
    std::vector<ArgSlot> paramRegs;
    bool returnsValue = false;
    ValType returnType = ValType::I32;
    bool returnSigned = true;
    // An aggregate returned in registers occupies this many of them, and produces
    // that many wasm results. 1 for a scalar, 0 for void.
    unsigned returnRegisterCount = 0;
    // Where each result is delivered, parallel to signature.results.
    std::vector<ArgSlot> returnRegs;
    // The return is a hidden pointer the caller supplied (sret).
    bool sretReturn = false;
    // A scalar floating-point return, delivered in the SSE return register.
    bool returnIsFloat = false;
};

// The registers an aggregate return is delivered in, in order. Matches the
// selector, which packs integer eightbyte 0 into RAX and 1 into RDX, and SSE
// eightbyte 0 into XMM0 and 1 into XMM1.
const PhysReg kReturnRegs[2] = {PhysReg::RAX, PhysReg::RDX};
const XmmReg kSseReturnRegs[2] = {XmmReg::XMM0, XmmReg::XMM1};

// Machine-IR opcode names, for diagnostics. Worth the boilerplate: an
// unsupported opcode is the expected outcome for most programs at this stage,
// so the message needs to say precisely which construct is missing.
const char* opcodeName(MOpcode op) {
    switch (op) {
        case MOpcode::MovRR: return "MovRR";
        case MOpcode::MovRI: return "MovRI";
        case MOpcode::Load: return "Load";
        case MOpcode::Store: return "Store";
        case MOpcode::StoreOutgoing: return "StoreOutgoing";
        case MOpcode::Ext: return "Ext";
        case MOpcode::Lea: return "Lea";
        case MOpcode::LeaSlot: return "LeaSlot";
        case MOpcode::LeaDisp: return "LeaDisp";
        case MOpcode::LoadInd: return "LoadInd";
        case MOpcode::StoreInd: return "StoreInd";
        case MOpcode::LeaIndex: return "LeaIndex";
        case MOpcode::Add: return "Add";
        case MOpcode::Sub: return "Sub";
        case MOpcode::IMul: return "IMul";
        case MOpcode::And: return "And";
        case MOpcode::Or: return "Or";
        case MOpcode::Xor: return "Xor";
        case MOpcode::Cmp: return "Cmp";
        case MOpcode::Neg: return "Neg";
        case MOpcode::Not: return "Not";
        case MOpcode::SetCC: return "SetCC";
        case MOpcode::Shl: return "Shl";
        case MOpcode::Shr: return "Shr";
        case MOpcode::Div: return "Div";
        case MOpcode::Mod: return "Mod";
        case MOpcode::UMulHi: return "UMulHi";
        case MOpcode::Jmp: return "Jmp";
        case MOpcode::Jcc: return "Jcc";
        case MOpcode::Call: return "Call";
        case MOpcode::CallImport: return "CallImport";
        case MOpcode::CallIndirect: return "CallIndirect";
        case MOpcode::Syscall: return "Syscall";
        case MOpcode::Ret: return "Ret";
        case MOpcode::Fence: return "Fence";
        case MOpcode::AtomicXAdd: return "AtomicXAdd";
        case MOpcode::AtomicCmpXchg: return "AtomicCmpXchg";
        case MOpcode::AsmFixed: return "AsmFixed";
        case MOpcode::AsmBlock: return "AsmBlock";
        case MOpcode::FLoad: return "FLoad";
        case MOpcode::FStore: return "FStore";
        case MOpcode::FLoadInd: return "FLoadInd";
        case MOpcode::FStoreInd: return "FStoreInd";
        case MOpcode::FStoreOutgoing: return "FStoreOutgoing";
        case MOpcode::FMovRR: return "FMovRR";
        case MOpcode::FConst: return "FConst";
        case MOpcode::FAdd: return "FAdd";
        case MOpcode::FSub: return "FSub";
        case MOpcode::FMul: return "FMul";
        case MOpcode::FDiv: return "FDiv";
        case MOpcode::FNeg: return "FNeg";
        case MOpcode::FCmp: return "FCmp";
        case MOpcode::CvtI2F: return "CvtI2F";
        case MOpcode::CvtF2I: return "CvtF2I";
        case MOpcode::CvtF2F: return "CvtF2F";
        case MOpcode::CvtF16ToF32: return "CvtF16ToF32";
        case MOpcode::CvtF32ToF16: return "CvtF32ToF16";
        case MOpcode::StoreXmmLo16: return "StoreXmmLo16";
        case MOpcode::FMovToGpr: return "FMovToGpr";
        case MOpcode::FMovFromGpr: return "FMovFromGpr";
        case MOpcode::PXorRR: return "PXorRR";
        case MOpcode::AesEncRR: return "AesEncRR";
    }
    return "<unknown>";
}

// Maps an Insty type to the wasm value type used at an ABI boundary.
//
// Integers narrower than 64 bits become i32, matching what a wasm toolchain
// would produce for the equivalent C signature; the body still computes in i64
// and narrows at the boundary. Aggregates are rejected -- they are passed by
// pointer, which needs linear memory.
bool boundaryType(Types::TypeRef type, ValType& out, bool& isVoid, bool& isSigned,
                  std::string& why) {
    isVoid = false;
    isSigned = true;
    if (!type) {
        isVoid = true;
        return true;
    }
    switch (type->kind) {
        case Types::Kind::Void:
            isVoid = true;
            return true;
        case Types::Kind::Bool:
            out = ValType::I32;
            isSigned = false;
            return true;
        case Types::Kind::Int:
            if (type->bitWidth > 64) {
                why = "128-bit integers are not supported on wasm yet";
                return false;
            }
            isSigned = type->isSigned;
            out = (type->bitWidth > 32) ? ValType::I64 : ValType::I32;
            return true;
        case Types::Kind::Float:
            if (type->bitWidth == 16) {
                // wasm has no f16. The selector already keeps a half packed in
                // the low 16 bits of an SSE register and computes in f32, so the
                // boundary carries those raw bits as an i32.
                out = ValType::I32;
                isSigned = false;
                return true;
            }
            if (type->bitWidth == 32) {
                out = ValType::F32;
                return true;
            }
            if (type->bitWidth == 64) {
                out = ValType::F64;
                return true;
            }
            why = "only f16, f32 and f64 are supported on wasm";
            return false;
        case Types::Kind::Enum:
            out = ValType::I32;
            return true;
        case Types::Kind::Pointer:
        case Types::Kind::Text:
            // Addresses are 32-bit on wasm32 even though the selector models
            // them as 64-bit internally.
            out = ValType::I32;
            isSigned = false;
            return true;
        default:
            why = "aggregate and slice types are not supported on wasm yet";
            return false;
    }
}

// Derives a function's wasm signature and the argument registers the selector
// places each piece of each parameter in.
//
// This has to reproduce the selector's assignment exactly, or a call would read
// its arguments out of the wrong locals. The mapping is:
//
//   scalar                        one register, one wasm parameter
//   aggregate in registers        one register and one wasm parameter per
//                                 eightbyte (so a 16-byte struct or a slice
//                                 becomes two i64 parameters)
//   aggregate in memory           one register holding a hidden pointer
//   aggregate return in memory    a hidden pointer prepended to the parameters;
//                                 the callee returns that pointer
//   aggregate return in registers one result per eightbyte (two results uses
//                                 wasm's multi-value returns)
//
// Because the emitted module is self-contained -- no linker, no other object to
// agree with -- reusing the selector's System V classification is sound. It only
// has to be internally consistent, not match the standard wasm C ABI.
// A 128-bit integer. Not an "aggregate" to the selector, but passed the same way:
// as a pair of eightbytes in consecutive registers.
bool isInt128Type(Types::TypeRef t) {
    return t != nullptr && t->kind == Types::Kind::Int && t->bitWidth > 64;
}

bool describeCallee(const Sema::FunctionInfo& info, const AbiInfo& abi,
                    const InstructionSelector& isel, Callee& out, std::string& why) {
    std::size_t gpCursor = 0;
    std::size_t xmmCursor = 0;
    std::uint32_t stackCursor = 0;
    // Mirrors the selector: the k-th stack argument sits at shadowSpace + 8k from
    // the frame base, in an 8-byte slot regardless of its declared width.
    auto takeStack = [&](ValType vt) {
        const std::uint32_t offset =
            static_cast<std::uint32_t>(abi.shadowSpace) + 8u * stackCursor;
        out.signature.params.push_back(vt);
        out.paramRegs.push_back(ArgSlot::stack(stackCursor, offset));
        ++stackCursor;
    };
    auto takeGp = [&](ValType vt) -> bool {
        if (gpCursor >= abi.intArgRegs.size()) {
            takeStack(vt);
            return true;
        }
        out.signature.params.push_back(vt);
        out.paramRegs.push_back(ArgSlot::integer(abi.intArgRegs[gpCursor++]));
        // On Win64 the two register files share one positional index.
        if (abi.sharedArgRegIndex) ++xmmCursor;
        return true;
    };
    auto takeSse = [&](ValType vt) -> bool {
        const std::size_t index = abi.sharedArgRegIndex ? gpCursor : xmmCursor;
        if (index >= abi.xmmArgRegs.size()) {
            takeStack(vt);
            if (abi.sharedArgRegIndex) ++gpCursor;
            ++xmmCursor;
            return true;
        }
        out.signature.params.push_back(vt);
        out.paramRegs.push_back(ArgSlot::sse(abi.xmmArgRegs[index]));
        if (abi.sharedArgRegIndex) {
            ++gpCursor;
            ++xmmCursor;
        } else {
            ++xmmCursor;
        }
        return true;
    };

    // An aggregate return classified in memory is passed a hidden pointer in the
    // first argument register, shifting every explicit parameter down by one.
    const bool aggregateReturn = isel.isAggregateType(info.returnType);
    InstructionSelector::AggregateAbi returnCls;
    if (aggregateReturn) {
        returnCls = isel.classifyAggregate(info.returnType);
        if (returnCls.inMemory && !takeGp(ValType::I64)) return false;
    }

    for (std::size_t i = 0; i < info.paramTypes.size(); ++i) {
        Types::TypeRef pty = info.paramTypes[i];
        if (isInt128Type(pty)) {
            // System V delivers a 16-byte integer as a register pair, low word
            // first. Two i64 parameters, exactly like a two-eightbyte aggregate.
            if (!takeGp(ValType::I64)) return false;
            if (!takeGp(ValType::I64)) return false;
            continue;
        }
        if (isel.isAggregateType(pty)) {
            const InstructionSelector::AggregateAbi cls = isel.classifyAggregate(pty);
            if (cls.inMemory) {
                if (!takeGp(ValType::I64)) return false;  // hidden pointer
                continue;
            }
            for (const auto& eb : cls.eightbytes) {
                // An SSE eightbyte travels in an XMM register, sized as the
                // selector sizes it: f32 when it holds at most 4 bytes.
                if (eb.isSSE) {
                    if (!takeSse(eb.bytes <= 4 ? ValType::F32 : ValType::F64)) {
                        return false;
                    }
                } else if (!takeGp(ValType::I64)) {
                    return false;
                }
            }
            continue;
        }

        ValType vt = ValType::I32;
        bool isVoid = false;
        bool isSigned = true;
        if (!boundaryType(pty, vt, isVoid, isSigned, why)) return false;
        if (isVoid) {
            why = "a parameter has type void";
            return false;
        }
        // An f16 travels in an SSE register like any other float, but its wasm
        // type is i32 because the register holds a packed half, not an f32.
        const bool isHalf = pty != nullptr && pty->kind == Types::Kind::Float &&
                            pty->bitWidth == 16;
        if (vt == ValType::F32 || vt == ValType::F64 || isHalf) {
            if (!takeSse(vt)) return false;
        } else if (!takeGp(vt)) {
            return false;
        }
    }

    if (aggregateReturn) {
        if (returnCls.inMemory) {
            // The callee hands the hidden pointer straight back.
            out.signature.results.push_back(ValType::I64);
            out.returnsValue = true;
            out.returnType = ValType::I64;
            out.returnSigned = false;
            out.returnRegisterCount = 1;
            out.returnRegs.push_back(ArgSlot::integer(abi.intReturnReg));
            out.sretReturn = true;
        } else {
            unsigned intIdx = 0;
            unsigned sseIdx = 0;
            for (const auto& eb : returnCls.eightbytes) {
                if (eb.isSSE) {
                    if (sseIdx >= 2) {
                        why = "too many floating-point eightbytes in a returned "
                              "aggregate";
                        return false;
                    }
                    out.signature.results.push_back(eb.bytes <= 4 ? ValType::F32
                                                                  : ValType::F64);
                    out.returnRegs.push_back(ArgSlot::sse(kSseReturnRegs[sseIdx++]));
                } else {
                    if (intIdx >= 2) {
                        why = "too many integer eightbytes in a returned aggregate";
                        return false;
                    }
                    out.signature.results.push_back(ValType::I64);
                    out.returnRegs.push_back(ArgSlot::integer(kReturnRegs[intIdx++]));
                }
            }
            out.returnsValue = !returnCls.eightbytes.empty();
            out.returnType = ValType::I64;
            out.returnSigned = false;
            out.returnRegisterCount =
                static_cast<unsigned>(returnCls.eightbytes.size());
        }
        return true;
    }

    if (isInt128Type(info.returnType)) {
        // Returned in RAX:RDX, low word first.
        out.signature.results.push_back(ValType::I64);
        out.signature.results.push_back(ValType::I64);
        out.returnRegs.push_back(ArgSlot::integer(kReturnRegs[0]));
        out.returnRegs.push_back(ArgSlot::integer(kReturnRegs[1]));
        out.returnsValue = true;
        out.returnType = ValType::I64;
        out.returnSigned = false;
        out.returnRegisterCount = 2;
        return true;
    }

    ValType rt = ValType::I32;
    bool isVoid = false;
    bool retSigned = true;
    if (!boundaryType(info.returnType, rt, isVoid, retSigned, why)) return false;
    if (!isVoid) {
        out.signature.results.push_back(rt);
        out.returnsValue = true;
        out.returnType = rt;
        out.returnSigned = retSigned;
        out.returnRegisterCount = 1;
        const bool isHalf = info.returnType != nullptr &&
                            info.returnType->kind == Types::Kind::Float &&
                            info.returnType->bitWidth == 16;
        if (rt == ValType::F32 || rt == ValType::F64 || isHalf) {
            out.returnIsFloat = true;
            out.returnRegs.push_back(ArgSlot::sse(abi.xmmReturnReg));
        } else {
            out.returnRegs.push_back(ArgSlot::integer(abi.intReturnReg));
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// FunctionEmitter
// ---------------------------------------------------------------------------

// Translates one MFunction into a wasm function body.
class FunctionEmitter {
public:
    // `self` is this function's own descriptor, produced by describeCallee. Using
    // the same descriptor callers use is what guarantees the two agree about
    // where parameters arrive and results are left.
    FunctionEmitter(const MFunction& fn, const Sema::FunctionInfo& info, const AbiInfo& abi,
                    const DataLayout& data, std::uint32_t stackPointerGlobal,
                    const std::unordered_map<std::string, Callee>& callees,
                    const Callee& self)
        : fn_(fn), info_(info), abi_(abi), data_(data),
          stackPointerGlobal_(stackPointerGlobal), callees_(callees), self_(self) {}

    bool run(FuncType& signatureOut, FunctionBody& bodyOut, std::string& errorOut);

private:
    // --- local allocation --------------------------------------------------
    bool assignLocals(std::string& errorOut);
    std::uint32_t vregLocal(VReg v) const { return vregBase_ + v; }
    bool physLocal(PhysReg r, std::uint32_t& out) const;
    bool xmmLocal(XmmReg r, std::uint32_t& out) const;
    bool slotLocal(std::uint32_t slot, std::uint32_t& out) const;

    // --- emission ----------------------------------------------------------
    bool emitEntrySeed(std::string& errorOut);
    bool emitInst(const MInst& inst, std::string& errorOut);
    bool pushOperand(const MOperand& op, std::string& errorOut);
    bool storeToOperand(const MOperand& op, std::string& errorOut);
    void emitWidthAdjust(std::uint8_t width, bool isSigned);
    bool emitReturn(std::string& errorOut);

    // --- memory ------------------------------------------------------------
    // Reserves the frame and points `framePtrLocal_` at it; paired with
    // emitFrameEpilogue() at every return. No-ops when nothing needs memory.
    void emitFramePrologue();
    void emitFrameEpilogue();
    bool slotMemoryOffset(std::uint32_t slot, std::uint32_t& out) const;
    // Pushes an i32 linear-memory address from an i64 address operand, folding a
    // non-negative displacement into the access's static offset instead.
    bool pushAddress(const MOperand& base, std::int64_t displacement,
                     std::uint32_t& staticOffset, std::string& errorOut);

    // --- floating point ----------------------------------------------------
    // XMM-class locals hold raw bits. These convert at the boundary of each
    // floating-point instruction, using the instruction's width to decide
    // whether the bits are an f32 (in the low half) or an f64.
    bool pushFloat(const MOperand& op, std::uint8_t width, std::string& errorOut);
    bool storeFloat(const MOperand& op, std::uint8_t width, std::string& errorOut);
    // Reads/writes a whole XMM local as raw i64 bits, for bitcasts and moves.
    bool pushFloatBits(const MOperand& op, std::string& errorOut);
    // f16 memory access. On x86 the conversion is folded into the load/store
    // itself (vcvtph2ps / vcvtps2ph), so a width of 2 on any of the F* memory
    // opcodes means "packed half in memory, f32 in the register". These convert
    // the i64 on the stack in place.
    bool emitHalfToF32(std::string& errorOut);
    bool emitF32ToHalf(std::string& errorOut);

    // --- calls -------------------------------------------------------------
    bool emitCall(const std::string& symbol, std::string& errorOut);

    // --- control flow ------------------------------------------------------
    bool emitDispatchLoop(std::string& errorOut);
    // Relative depth of the dispatcher `loop` from inside the current block's
    // code, plus `extraScopes` for any scope opened since (e.g. an `if`).
    std::uint32_t dispatchDepth(std::uint32_t extraScopes) const {
        return (blockCount_ - 1 - currentBlock_) + extraScopes;
    }
    bool emitBranchToBlock(std::uint32_t target, std::uint32_t extraScopes,
                           std::string& errorOut);
    // Emits the pending Cmp as a wasm comparison, leaving an i32 0/1 on the
    // stack. Consumes the pending compare.
    bool emitPendingCompare(Cond cond, std::string& errorOut);

    bool fail(const std::string& message, std::string& errorOut) const {
        errorOut = "wasm: " + fn_.name() + ": " + message;
        return false;
    }

    const MFunction& fn_;
    const Sema::FunctionInfo& info_;
    const AbiInfo& abi_;
    const DataLayout& data_;
    std::uint32_t stackPointerGlobal_ = 0;
    const std::unordered_map<std::string, Callee>& callees_;
    const Callee& self_;
    // Function symbol -> its slot in the module's function table, for the
    // functions whose address is taken. Set by the module assembler.
    const std::unordered_map<std::string, std::uint32_t>* tableSlots_ = nullptr;
    // Number of indirect-call arguments -> the type index of the uniform thunk
    // signature for that arity.
    const std::unordered_map<std::uint32_t, std::uint32_t>* indirectTypes_ = nullptr;

public:
    void setIndirectTables(
        const std::unordered_map<std::string, std::uint32_t>* slots,
        const std::unordered_map<std::uint32_t, std::uint32_t>* types) {
        tableSlots_ = slots;
        indirectTypes_ = types;
    }

private:

    CodeBuilder code_;
    FuncType signature_;
    std::vector<ValType> locals_;  // declared locals, beyond the parameters

    std::uint32_t paramCount_ = 0;
    std::uint32_t vregBase_ = 0;
    std::unordered_map<std::uint8_t, std::uint32_t> physLocals_;
    std::unordered_map<std::uint8_t, std::uint32_t> xmmLocals_;
    // A frame slot lives in exactly one of these: a wasm local when its address
    // is never taken, otherwise a byte offset into the function's stack frame.
    std::unordered_map<std::uint32_t, std::uint32_t> slotLocals_;
    std::unordered_map<std::uint32_t, std::uint32_t> slotOffsets_;
    std::uint32_t frameSize_ = 0;
    std::uint32_t framePtrLocal_ = 0;
    bool hasFrame_ = false;
    // Bytes at the bottom of the frame reserved for outgoing stack arguments.
    std::uint32_t outgoingSize_ = 0;
    // Stack-argument index -> the local standing in for its incoming slot.
    std::unordered_map<std::uint32_t, std::uint32_t> incomingStackLocals_;

    bool returnsValue_ = false;
    ValType returnType_ = ValType::I32;

    // Control-flow state. `stateLocal_` holds the index of the MIR block to run
    // next; the dispatcher branches on it.
    std::uint32_t stateLocal_ = 0;
    std::uint32_t blockCount_ = 0;
    std::uint32_t currentBlock_ = 0;

    // x86 compares set EFLAGS, which a following Jcc/SetCC reads. wasm has no
    // flags, so a Cmp emits nothing and is instead folded into its consumer.
    bool hasPendingCmp_ = false;
    bool pendingCmpIsFloat_ = false;
    std::uint8_t pendingCmpWidth_ = 8;
    MOperand pendingLhs_;
    MOperand pendingRhs_;
};

// Only `Jmp` and `Ret` end a block unconditionally. A trailing `Jcc` does not:
// its not-taken edge falls through, which the optimizer relies on when it drops
// a `Jmp` whose target is the next block.
bool endsWithTerminator(const MBasicBlock& block) {
    if (block.insts.empty()) return false;
    const MOpcode op = block.insts.back().op;
    return op == MOpcode::Jmp || op == MOpcode::Ret;
}

// Machine-IR condition -> the wasm comparison for 64-bit operands. `Cmp a, b`
// sets flags as if computing `a - b`, so the operands map straight across in
// order.
bool condToWasmOp(Cond cond, Op& out) {
    switch (cond) {
        case Cond::EQ:  out = Op::I64Eq;   return true;
        case Cond::NE:  out = Op::I64Ne;   return true;
        case Cond::LT:  out = Op::I64LtS;  return true;
        case Cond::LE:  out = Op::I64LeS;  return true;
        case Cond::GT:  out = Op::I64GtS;  return true;
        case Cond::GE:  out = Op::I64GeS;  return true;
        case Cond::ULT: out = Op::I64LtU;  return true;
        case Cond::ULE: out = Op::I64LeU;  return true;
        case Cond::UGT: out = Op::I64GtU;  return true;
        case Cond::UGE: out = Op::I64GeU;  return true;
    }
    return false;
}

// Float comparison. ucomisd sets the *unsigned* flag bits, so the selector emits
// the unsigned condition codes for ordered float comparisons; both spellings map
// to the same wasm operator here.
bool floatCondToWasmOp(Cond cond, std::uint8_t width, Op& out) {
    const bool single = width == 4;
    switch (cond) {
        case Cond::EQ:  out = single ? Op::F32Eq : Op::F64Eq; return true;
        case Cond::NE:  out = single ? Op::F32Ne : Op::F64Ne; return true;
        case Cond::LT:
        case Cond::ULT: out = single ? Op::F32Lt : Op::F64Lt; return true;
        case Cond::LE:
        case Cond::ULE: out = single ? Op::F32Le : Op::F64Le; return true;
        case Cond::GT:
        case Cond::UGT: out = single ? Op::F32Gt : Op::F64Gt; return true;
        case Cond::GE:
        case Cond::UGE: out = single ? Op::F32Ge : Op::F64Ge; return true;
    }
    return false;
}

// Allocates one wasm local per storage location the Machine IR can name.
//
// Layout:  [0, params)  the wasm parameters
//          then         one per virtual register
//          then         one per physical register actually referenced
//          then         one per frame slot promoted out of memory
bool FunctionEmitter::assignLocals(std::string& errorOut) {
    // The signature is taken from this function's descriptor rather than being
    // recomputed, so it cannot drift from what callers expect.
    signature_ = self_.signature;
    paramCount_ = static_cast<std::uint32_t>(signature_.params.size());
    returnsValue_ = self_.returnsValue;
    returnType_ = self_.returnType;

    // One local per virtual register, typed by its register class.
    vregBase_ = paramCount_;
    for (VReg v = 0; v < fn_.numVRegs(); ++v) {
        // XMM-class registers hold raw bits, like the hardware: the instruction's
        // width decides whether they are read as f32 or f64. Typing them f64 would
        // silently round f32 arithmetic through double precision.
        locals_.push_back(ValType::I64);
    }

    // Scan for the physical registers and frame slots the body references. A
    // slot whose address is taken cannot be a local -- it needs real memory.
    std::set<std::uint8_t> usedPhys;
    std::set<std::uint8_t> usedXmm;
    std::set<std::uint32_t> usedSlots;
    std::set<std::uint32_t> addressTakenSlots;
    for (const auto& block : fn_.blocks()) {
        for (const auto& inst : block.insts) {
            if (inst.op == MOpcode::LeaSlot) {
                for (const auto& op : inst.operands) {
                    if (op.kind == OperandKind::FrameSlot) {
                        addressTakenSlots.insert(op.frameSlot);
                    }
                }
            }
            for (const auto& op : inst.operands) {
                if (op.kind == OperandKind::PhysReg) {
                    if (op.xmm != XmmReg::None) {
                        usedXmm.insert(xmmIndex(op.xmm));
                    } else if (op.phys != PhysReg::None) {
                        usedPhys.insert(regIndex(op.phys));
                    }
                } else if (op.kind == OperandKind::FrameSlot) {
                    usedSlots.insert(op.frameSlot);
                }
            }
            for (PhysReg clobber : inst.clobbers) {
                if (clobber != PhysReg::None) usedPhys.insert(regIndex(clobber));
            }
        }
    }

    // The ABI argument registers are always live at entry, even if the body never
    // reads a given one, because the entry seeding writes them all.
    for (const ArgSlot& s : self_.paramRegs) {
        if (s.isXmm) {
            usedXmm.insert(xmmIndex(s.xmm));
        } else {
            usedPhys.insert(regIndex(s.gp));
        }
    }
    // Every register the result is delivered in.
    for (const ArgSlot& s : self_.returnRegs) {
        if (s.isXmm) {
            usedXmm.insert(xmmIndex(s.xmm));
        } else {
            usedPhys.insert(regIndex(s.gp));
        }
    }
    if (returnsValue_ && !self_.returnIsFloat) {
        usedPhys.insert(regIndex(abi_.intReturnReg));
    }

    // A call reads its arguments back out of the argument-register locals and
    // writes the result into the return-register local, so reserve the whole set
    // up front rather than relying on the body to have mentioned each one.
    bool makesCalls = false;
    for (const auto& block : fn_.blocks()) {
        for (const auto& inst : block.insts) {
            if (inst.op == MOpcode::Call || inst.op == MOpcode::CallImport ||
                inst.op == MOpcode::CallIndirect) {
                makesCalls = true;
                break;
            }
        }
        if (makesCalls) break;
    }
    if (makesCalls) {
        for (PhysReg r : abi_.intArgRegs) usedPhys.insert(regIndex(r));
        usedPhys.insert(regIndex(abi_.intReturnReg));
        for (XmmReg r : abi_.xmmArgRegs) usedXmm.insert(xmmIndex(r));
        usedXmm.insert(xmmIndex(abi_.xmmReturnReg));
        usedXmm.insert(xmmIndex(XmmReg::XMM1));  // second SSE return eightbyte
    }

    for (std::uint8_t r : usedPhys) {
        physLocals_[r] = paramCount_ + static_cast<std::uint32_t>(locals_.size());
        locals_.push_back(ValType::I64);
    }
    for (std::uint8_t r : usedXmm) {
        xmmLocals_[r] = paramCount_ + static_cast<std::uint32_t>(locals_.size());
        locals_.push_back(ValType::I64);  // raw bits; see above
    }
    // Split the slots: an address-taken slot must live in linear memory, because
    // a write through the pointer has to be visible to every other access. The
    // rest become locals, which is both smaller and faster.
    //
    // An `isIncoming` slot is special: it names a stack argument in what x86 would
    // call the caller's frame. There is no such frame here, so it becomes a local
    // seeded from the corresponding wasm parameter. Its rbpOffset encodes which
    // stack argument it is, which is how it pairs up with the descriptor.
    const auto& slots = fn_.frameSlots();
    for (std::uint32_t slot : usedSlots) {
        if (slot < slots.size() && slots[slot].isIncoming) {
            const std::int64_t base = 16 + static_cast<std::int64_t>(abi_.shadowSpace);
            const std::int64_t delta = slots[slot].rbpOffset - base;
            if (delta < 0 || (delta % 8) != 0) {
                return fail("incoming argument slot at an unexpected frame offset",
                            errorOut);
            }
            const std::uint32_t stackIndex = static_cast<std::uint32_t>(delta / 8);
            std::uint32_t local = paramCount_ + static_cast<std::uint32_t>(locals_.size());
            locals_.push_back(ValType::I64);
            slotLocals_[slot] = local;
            incomingStackLocals_[stackIndex] = local;
            continue;
        }
        if (addressTakenSlots.count(slot)) continue;  // laid out below
        slotLocals_[slot] = paramCount_ + static_cast<std::uint32_t>(locals_.size());
        locals_.push_back(ValType::I64);
    }

    // Outgoing arguments for the largest call site occupy the bottom of the frame,
    // at exactly the offsets the selector's StoreOutgoing uses.
    outgoingSize_ = static_cast<std::uint32_t>(fn_.maxOutgoingArgBytes());
    if (outgoingSize_ > 0) frameSize_ = alignUp(outgoingSize_, kStackAlign);

    // Lay the address-taken slots out in the frame, each at its natural
    // alignment. Iteration is over the ordered set, so the layout is
    // deterministic.
    for (std::uint32_t slot : addressTakenSlots) {
        if (!usedSlots.count(slot)) continue;
        unsigned size = 8;
        unsigned align = 8;
        if (slot < slots.size()) {
            size = slots[slot].size ? slots[slot].size : 8;
            align = slots[slot].align ? slots[slot].align : 8;
        }
        frameSize_ = alignUp(frameSize_, align);
        slotOffsets_[slot] = frameSize_;
        frameSize_ += size;
    }
    if (frameSize_ > 0) {
        frameSize_ = alignUp(frameSize_, kStackAlign);
        hasFrame_ = true;
        framePtrLocal_ = paramCount_ + static_cast<std::uint32_t>(locals_.size());
        locals_.push_back(ValType::I32);  // a linear-memory address
    }

    // The dispatcher's state variable, last so it does not disturb the indices
    // above. i32 because br_table takes an i32.
    blockCount_ = static_cast<std::uint32_t>(fn_.blocks().size());
    stateLocal_ = paramCount_ + static_cast<std::uint32_t>(locals_.size());
    locals_.push_back(ValType::I32);
    return true;
}

bool FunctionEmitter::physLocal(PhysReg r, std::uint32_t& out) const {
    auto it = physLocals_.find(regIndex(r));
    if (it == physLocals_.end()) return false;
    out = it->second;
    return true;
}

bool FunctionEmitter::xmmLocal(XmmReg r, std::uint32_t& out) const {
    auto it = xmmLocals_.find(xmmIndex(r));
    if (it == xmmLocals_.end()) return false;
    out = it->second;
    return true;
}

bool FunctionEmitter::slotLocal(std::uint32_t slot, std::uint32_t& out) const {
    auto it = slotLocals_.find(slot);
    if (it == slotLocals_.end()) return false;
    out = it->second;
    return true;
}

bool FunctionEmitter::slotMemoryOffset(std::uint32_t slot, std::uint32_t& out) const {
    auto it = slotOffsets_.find(slot);
    if (it == slotOffsets_.end()) return false;
    out = it->second;
    return true;
}

// Claims `frameSize_` bytes of shadow stack. The stack grows down, so the frame
// base is the decremented pointer.
void FunctionEmitter::emitFramePrologue() {
    if (!hasFrame_) return;
    code_.globalGet(stackPointerGlobal_);
    code_.i32Const(static_cast<std::int32_t>(frameSize_));
    code_.op(Op::I32Sub);
    code_.localTee(framePtrLocal_);
    code_.globalSet(stackPointerGlobal_);
}

// Releases the frame. Emitted before every return, so a caller sees the stack
// pointer it had.
void FunctionEmitter::emitFrameEpilogue() {
    if (!hasFrame_) return;
    code_.localGet(framePtrLocal_);
    code_.i32Const(static_cast<std::int32_t>(frameSize_));
    code_.op(Op::I32Add);
    code_.globalSet(stackPointerGlobal_);
}

// Leaves an i32 linear-memory address on the stack for `base + displacement`.
//
// wasm memory accesses carry a static unsigned offset, so a non-negative
// displacement is folded into it (`staticOffset`) and costs nothing. A negative
// displacement has no such encoding and is added explicitly.
bool FunctionEmitter::pushAddress(const MOperand& base, std::int64_t displacement,
                                 std::uint32_t& staticOffset, std::string& errorOut) {
    if (!pushOperand(base, errorOut)) return false;
    code_.op(Op::I32WrapI64);  // wasm32: addresses are 32-bit
    if (displacement >= 0) {
        staticOffset = static_cast<std::uint32_t>(displacement);
    } else {
        staticOffset = 0;
        code_.i32Const(static_cast<std::int32_t>(displacement));
        code_.op(Op::I32Add);
    }
    return true;
}

// Copies the wasm parameters into the locals standing in for the ABI argument
// registers, widening to the i64 the body computes in. After this the selector's
// `Store slot, usePhys(RDI)` parameter prologue works unmodified.
// Copies each wasm parameter into the local standing in for the argument register
// the selector expects it in. One source parameter can span several of these (an
// aggregate contributes one per eightbyte), which is why this iterates the
// descriptor's flat register list rather than the source parameter list.
bool FunctionEmitter::emitEntrySeed(std::string& errorOut) {
    // Signedness per source parameter, so an i32 widens correctly. Aggregate
    // pieces are raw eightbytes and already i64, so they need no extension.
    std::vector<bool> paramSigned(signature_.params.size(), false);
    {
        std::size_t slot = 0;
        if (self_.sretReturn && slot < paramSigned.size()) ++slot;  // hidden pointer
        for (Types::TypeRef pty : info_.paramTypes) {
            ValType vt = ValType::I32;
            bool isVoid = false;
            bool isSigned = true;
            std::string why;
            if (!boundaryType(pty, vt, isVoid, isSigned, why)) {
                // An aggregate: its pieces are i64 eightbytes, no extension.
                ++slot;
                continue;
            }
            if (slot < paramSigned.size()) paramSigned[slot] = isSigned;
            ++slot;
        }
    }

    for (std::size_t i = 0; i < self_.paramRegs.size(); ++i) {
        const ArgSlot& slot = self_.paramRegs[i];
        std::uint32_t dest = 0;
        if (slot.isStack) {
            // A stack argument. The body reads it through an incoming frame slot,
            // which is a local here, so seed that local from the parameter.
            auto found = incomingStackLocals_.find(slot.stackIndex);
            if (found == incomingStackLocals_.end()) {
                continue;  // the body never reads this argument
            }
            code_.localGet(static_cast<std::uint32_t>(i));
            const ValType pt = signature_.params[i];
            if (pt == ValType::F32) {
                code_.op(Op::I32ReinterpretF32);
                code_.op(Op::I64ExtendI32U);
            } else if (pt == ValType::F64) {
                code_.op(Op::I64ReinterpretF64);
            } else if (pt == ValType::I32) {
                code_.op(paramSigned[i] ? Op::I64ExtendI32S : Op::I64ExtendI32U);
            }
            code_.localSet(found->second);
            continue;
        }
        if (slot.isXmm) {
            if (!xmmLocal(slot.xmm, dest)) {
                return fail("internal: no local for SSE argument register", errorOut);
            }
            // Store the raw bits, matching how the body will read them back. An
            // i32 here is a packed f16, already raw bits: just widen it.
            code_.localGet(static_cast<std::uint32_t>(i));
            if (signature_.params[i] == ValType::F32) {
                code_.op(Op::I32ReinterpretF32);
                code_.op(Op::I64ExtendI32U);
            } else if (signature_.params[i] == ValType::I32) {
                code_.op(Op::I64ExtendI32U);
            } else {
                code_.op(Op::I64ReinterpretF64);
            }
            code_.localSet(dest);
            continue;
        }
        if (!physLocal(slot.gp, dest)) {
            return fail("internal: no local for argument register", errorOut);
        }
        code_.localGet(static_cast<std::uint32_t>(i));
        if (signature_.params[i] == ValType::I32) {
            code_.op(paramSigned[i] ? Op::I64ExtendI32S : Op::I64ExtendI32U);
        }
        code_.localSet(dest);
    }
    return true;
}

// Pushes the value of a read operand onto the wasm operand stack.
bool FunctionEmitter::pushOperand(const MOperand& op, std::string& errorOut) {
    switch (op.kind) {
        case OperandKind::VirtReg:
            code_.localGet(vregLocal(op.vreg));
            return true;
        case OperandKind::Imm:
            code_.i64Const(op.imm);
            return true;
        case OperandKind::PhysReg: {
            std::uint32_t local = 0;
            if (op.xmm != XmmReg::None) {
                if (!xmmLocal(op.xmm, local)) {
                    return fail("internal: no local for xmm register", errorOut);
                }
            } else if (!physLocal(op.phys, local)) {
                return fail("internal: no local for physical register", errorOut);
            }
            code_.localGet(local);
            return true;
        }
        case OperandKind::FrameSlot: {
            std::uint32_t local = 0;
            if (!slotLocal(op.frameSlot, local)) {
                return fail("internal: frame slot " + std::to_string(op.frameSlot) +
                                " lives in memory and must be reached through an "
                                "explicit Load/Store, not as a value operand",
                            errorOut);
            }
            code_.localGet(local);
            return true;
        }
        default:
            return fail("unsupported operand kind in a value position", errorOut);
    }
}

// Pops the top of the operand stack into a written operand.
bool FunctionEmitter::storeToOperand(const MOperand& op, std::string& errorOut) {
    switch (op.kind) {
        case OperandKind::VirtReg:
            code_.localSet(vregLocal(op.vreg));
            return true;
        case OperandKind::PhysReg: {
            std::uint32_t local = 0;
            if (op.xmm != XmmReg::None) {
                if (!xmmLocal(op.xmm, local)) {
                    return fail("internal: no local for xmm register", errorOut);
                }
            } else if (!physLocal(op.phys, local)) {
                return fail("internal: no local for physical register", errorOut);
            }
            code_.localSet(local);
            return true;
        }
        case OperandKind::FrameSlot: {
            std::uint32_t local = 0;
            if (!slotLocal(op.frameSlot, local)) {
                return fail("internal: frame slot " + std::to_string(op.frameSlot) +
                                " lives in memory and must be reached through an "
                                "explicit Load/Store, not as a value operand",
                            errorOut);
            }
            code_.localSet(local);
            return true;
        }
        default:
            return fail("unsupported operand kind in a destination position", errorOut);
    }
}

// Expands an IEEE-754 binary16 bit pattern to binary32, so an f16 constant can be
// folded here instead of calling the runtime helper at startup. Mirrors
// __ins_f16_to_f32 in the core runtime.
std::uint32_t halfBitsToF32Bits(std::uint32_t h) {
    const std::uint32_t sign = (h & 0x8000u) << 16;
    const std::uint32_t exp = (h >> 10) & 0x1Fu;
    const std::uint32_t mant = h & 0x3FFu;
    if (exp == 0) {
        if (mant == 0) return sign;  // signed zero
        std::uint32_t e = 0;
        std::uint32_t m = mant;
        while ((m & 0x400u) == 0) {
            m <<= 1;
            ++e;
        }
        m &= 0x3FFu;
        return sign | ((127u - 14u - e) << 23) | (m << 13);
    }
    if (exp == 31) return sign | 0x7F800000u | (mant << 13);
    return sign | ((exp + 112u) << 23) | (mant << 13);
}

// Reads an XMM-class operand's raw bits, without interpreting them. Used for
// bitcasts (FMovToGpr/FMovFromGpr) and register-to-register moves, which on x86
// copy bits and do not care about the interpretation.
bool FunctionEmitter::pushFloatBits(const MOperand& op, std::string& errorOut) {
    return pushOperand(op, errorOut);
}

// Reinterprets an XMM-class operand's bits as a floating-point value, leaving an
// f32 or f64 on the stack. An f32 lives in the low 32 bits, matching movss.
bool FunctionEmitter::pushFloat(const MOperand& op, std::uint8_t width,
                                std::string& errorOut) {
    if (!pushOperand(op, errorOut)) return false;
    if (width == 4) {
        code_.op(Op::I32WrapI64);
        code_.op(Op::F32ReinterpretI32);
    } else if (width == 8) {
        code_.op(Op::F64ReinterpretI64);
    } else {
        return fail("unsupported floating-point width " + std::to_string(width),
                    errorOut);
    }
    return true;
}

// Stores the f32/f64 on the stack back into an XMM-class operand as raw bits.
bool FunctionEmitter::storeFloat(const MOperand& op, std::uint8_t width,
                                 std::string& errorOut) {
    if (width == 4) {
        code_.op(Op::I32ReinterpretF32);
        code_.op(Op::I64ExtendI32U);  // movss zeroes the upper bits
    } else if (width == 8) {
        code_.op(Op::I64ReinterpretF64);
    } else {
        return fail("unsupported floating-point width " + std::to_string(width),
                    errorOut);
    }
    return storeToOperand(op, errorOut);
}

bool FunctionEmitter::emitHalfToF32(std::string& errorOut) {
    auto helper = callees_.find("__ins_f16_to_f32");
    if (helper == callees_.end()) {
        return fail("internal: the f16 expansion helper was not registered in this "
                    "module",
                    errorOut);
    }
    code_.op(Op::I32WrapI64);
    code_.call(helper->second.funcIndex);
    code_.op(Op::I64ExtendI32U);
    return true;
}

bool FunctionEmitter::emitF32ToHalf(std::string& errorOut) {
    auto helper = callees_.find("__ins_f32_to_f16");
    if (helper == callees_.end()) {
        return fail("internal: the f16 packing helper was not registered in this "
                    "module",
                    errorOut);
    }
    code_.op(Op::I32WrapI64);
    code_.call(helper->second.funcIndex);
    code_.op(Op::I64ExtendI32U);
    return true;
}

// Narrows the i64 on top of the stack to `width` bytes with the requested sign
// extension, matching the movsx/movzx the x86 path would emit.
void FunctionEmitter::emitWidthAdjust(std::uint8_t width, bool isSigned) {
    switch (width) {
        case 1:
            if (isSigned) {
                code_.op(Op::I64Extend8S);
            } else {
                code_.i64Const(0xFF);
                code_.op(Op::I64And);
            }
            break;
        case 2:
            if (isSigned) {
                code_.op(Op::I64Extend16S);
            } else {
                code_.i64Const(0xFFFF);
                code_.op(Op::I64And);
            }
            break;
        case 4:
            if (isSigned) {
                code_.op(Op::I64Extend32S);
            } else {
                code_.i64Const(0xFFFFFFFFLL);
                code_.op(Op::I64And);
            }
            break;
        default:
            break;  // 8 bytes: already the full value
    }
}

// Reads the ABI return register and returns it, narrowing to the declared wasm
// result type.
bool FunctionEmitter::emitReturn(std::string& errorOut) {
    emitFrameEpilogue();
    // Push one value per declared result, reading it out of the register the
    // selector left it in and converting to the result's wasm type.
    for (std::size_t i = 0; i < self_.returnRegs.size(); ++i) {
        const ArgSlot& slot = self_.returnRegs[i];
        const ValType rt = signature_.results[i];
        std::uint32_t local = 0;
        if (slot.isXmm) {
            if (!xmmLocal(slot.xmm, local)) {
                return fail("internal: no local for the SSE return register", errorOut);
            }
            code_.localGet(local);
            if (rt == ValType::F32) {
                code_.op(Op::I32WrapI64);
                code_.op(Op::F32ReinterpretI32);
            } else if (rt == ValType::I32) {
                code_.op(Op::I32WrapI64);  // packed f16: raw bits
            } else {
                code_.op(Op::F64ReinterpretI64);
            }
            continue;
        }
        if (!physLocal(slot.gp, local)) {
            return fail("internal: no local for the return register", errorOut);
        }
        code_.localGet(local);
        if (rt == ValType::I32) code_.op(Op::I32WrapI64);
    }
    code_.op(Op::Return);
    return true;
}

// Emits a direct call.
//
// The selector has already moved each argument into its ABI argument register,
// which on wasm means into the local standing in for that register. This reads
// them back in parameter order onto the operand stack, narrowing to the callee's
// declared parameter type, calls, and files the result back into the local for
// the return register.
bool FunctionEmitter::emitCall(const std::string& symbol, std::string& errorOut) {
    auto it = callees_.find(symbol);
    if (it == callees_.end()) {
        return fail("call to '" + symbol +
                        "' which has no definition or import in this module",
                    errorOut);
    }
    const Callee& callee = it->second;

    for (std::size_t i = 0; i < callee.paramRegs.size(); ++i) {
        const ArgSlot& slot = callee.paramRegs[i];
        const ValType pt = callee.signature.params[i];
        std::uint32_t local = 0;
        if (slot.isStack) {
            // The selector wrote this argument into the outgoing area with
            // StoreOutgoing; read it back from the same offset.
            if (!hasFrame_) {
                return fail("internal: a call passes arguments on the stack but no "
                            "outgoing area was reserved",
                            errorOut);
            }
            code_.localGet(framePtrLocal_);
            code_.load(Op::I64Load, /*alignLog2=*/0, slot.stackOffset);
            if (pt == ValType::F32) {
                code_.op(Op::I32WrapI64);
                code_.op(Op::F32ReinterpretI32);
            } else if (pt == ValType::F64) {
                code_.op(Op::F64ReinterpretI64);
            } else if (pt == ValType::I32) {
                code_.op(Op::I32WrapI64);
            }
            continue;
        }
        if (slot.isXmm) {
            if (!xmmLocal(slot.xmm, local)) {
                return fail("internal: no local for an SSE argument register in a "
                            "call to '" + symbol + "'",
                            errorOut);
            }
            // The local holds raw bits; reinterpret to the declared type.
            code_.localGet(local);
            if (pt == ValType::F32) {
                code_.op(Op::I32WrapI64);
                code_.op(Op::F32ReinterpretI32);
            } else if (pt == ValType::I32) {
                code_.op(Op::I32WrapI64);  // packed f16: raw bits
            } else {
                code_.op(Op::F64ReinterpretI64);
            }
            continue;
        }
        if (!physLocal(slot.gp, local)) {
            return fail("internal: no local for argument register in call to '" +
                            symbol + "'",
                        errorOut);
        }
        code_.localGet(local);
        if (pt == ValType::I32) {
            code_.op(Op::I32WrapI64);  // the body computes in i64
        }
    }

    code_.call(callee.funcIndex);

    if (!callee.returnsValue) return true;

    // Results arrive in signature order, so the last is on top of the stack:
    // file them away back to front.
    for (std::size_t i = callee.returnRegs.size(); i-- > 0;) {
        const ArgSlot& slot = callee.returnRegs[i];
        const ValType rt = callee.signature.results[i];
        std::uint32_t local = 0;
        const bool have = slot.isXmm ? xmmLocal(slot.xmm, local)
                                     : physLocal(slot.gp, local);
        if (!have) {
            // The caller discarded this result, so no local was reserved for it.
            code_.op(Op::Drop);
            continue;
        }
        if (slot.isXmm) {
            if (rt == ValType::F32) {
                code_.op(Op::I32ReinterpretF32);
                code_.op(Op::I64ExtendI32U);
            } else if (rt == ValType::I32) {
                code_.op(Op::I64ExtendI32U);  // packed f16: raw bits
            } else {
                code_.op(Op::I64ReinterpretF64);
            }
        } else if (rt == ValType::I32) {
            code_.op(callee.returnSigned ? Op::I64ExtendI32S : Op::I64ExtendI32U);
        }
        code_.localSet(local);
    }
    return true;
}

// Records the target block in the dispatcher's state variable and jumps back to
// the dispatcher.
bool FunctionEmitter::emitBranchToBlock(std::uint32_t target, std::uint32_t extraScopes,
                                       std::string& errorOut) {
    if (target >= blockCount_) {
        return fail("branch to out-of-range block " + std::to_string(target), errorOut);
    }
    code_.i32Const(static_cast<std::int32_t>(target));
    code_.localSet(stateLocal_);
    code_.br(dispatchDepth(extraScopes));
    return true;
}

bool FunctionEmitter::emitPendingCompare(Cond cond, std::string& errorOut) {
    if (!hasPendingCmp_) {
        return fail("a conditional branch or SetCC appeared without a preceding Cmp; "
                    "the flag-producing compare must immediately precede its consumer",
                    errorOut);
    }
    Op wasmOp = Op::I64Eq;
    if (pendingCmpIsFloat_) {
        if (!floatCondToWasmOp(cond, pendingCmpWidth_, wasmOp)) {
            return fail("unsupported floating-point condition code", errorOut);
        }
        if (!pushFloat(pendingLhs_, pendingCmpWidth_, errorOut)) return false;
        if (!pushFloat(pendingRhs_, pendingCmpWidth_, errorOut)) return false;
    } else {
        if (!condToWasmOp(cond, wasmOp)) {
            return fail("unsupported condition code", errorOut);
        }
        if (!pushOperand(pendingLhs_, errorOut)) return false;
        if (!pushOperand(pendingRhs_, errorOut)) return false;
    }
    code_.op(wasmOp);
    hasPendingCmp_ = false;
    pendingCmpIsFloat_ = false;
    return true;
}

// Emits every basic block inside a dispatcher: a `loop` wrapping one `block`
// scope per MIR block, with a `br_table` selecting which to enter.
//
//     loop                        ;; re-entered on every inter-block branch
//       block ... block           ;; one per MIR block, L_{n-1} outermost
//         local.get $state
//         br_table 0 .. n-1
//       end                       ;; <- br 0 lands here
//       <block 0>
//       end                       ;; <- br 1 lands here
//       <block 1>
//       ...
//     end
//     unreachable
//
// Branching to block k is `$state = k; br <depth of loop>`, so any CFG works --
// including irreducible ones, which a structured reconstruction would have to
// special-case. The cost is that loops are not expressed as wasm `loop`s, so an
// engine cannot recognise them; see the note in wasm_emit.hpp.
bool FunctionEmitter::emitDispatchLoop(std::string& errorOut) {
    const std::uint32_t n = blockCount_;
    if (n == 0) return fail("function has no basic blocks", errorOut);

    // Block 0 is the entry. wasm zero-initializes locals, but be explicit.
    code_.i32Const(0);
    code_.localSet(stateLocal_);

    code_.loop();
    for (std::uint32_t i = 0; i < n; ++i) {
        code_.block();
    }
    code_.localGet(stateLocal_);
    std::vector<std::uint32_t> targets;
    targets.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) targets.push_back(i);
    // The default is unreachable: `$state` is only ever set to a valid index.
    code_.brTable(targets, 0);

    for (std::uint32_t i = 0; i < n; ++i) {
        code_.op(Op::End);  // closes L_i; control lands on block i's code
        currentBlock_ = i;
        hasPendingCmp_ = false;
        const MBasicBlock& block = fn_.blocks()[i];
        for (const auto& inst : block.insts) {
            if (!emitInst(inst, errorOut)) return false;
        }
        // A block with no terminator falls through to the next one. This is
        // normal after optimization, which drops a `Jmp`/`Jcc` whose target is
        // already the following block.
        if (!endsWithTerminator(block)) {
            if (i + 1 < n) {
                if (!emitBranchToBlock(i + 1, 0, errorOut)) return false;
            } else {
                code_.op(Op::Unreachable);
            }
        }
    }

    code_.op(Op::End);          // closes the loop
    code_.op(Op::Unreachable);  // control never falls out of the dispatcher
    return true;
}

bool FunctionEmitter::emitInst(const MInst& inst, std::string& errorOut) {
    const auto& ops = inst.operands;

    // Binary in-place forms: usedef0 <op>= use1.
    auto binary = [&](Op wasmOp) -> bool {
        if (ops.size() < 2) return fail("malformed binary instruction", errorOut);
        if (!pushOperand(ops[0], errorOut)) return false;
        if (!pushOperand(ops[1], errorOut)) return false;
        code_.op(wasmOp);
        return storeToOperand(ops[0], errorOut);
    };

    switch (inst.op) {
        case MOpcode::MovRR:
            if (ops.size() < 2) return fail("malformed MovRR", errorOut);
            if (!pushOperand(ops[1], errorOut)) return false;
            return storeToOperand(ops[0], errorOut);

        case MOpcode::MovRI:
            if (ops.size() < 2 || ops[1].kind != OperandKind::Imm) {
                return fail("malformed MovRI", errorOut);
            }
            code_.i64Const(ops[1].imm);
            return storeToOperand(ops[0], errorOut);

        case MOpcode::Add: return binary(Op::I64Add);
        case MOpcode::Sub: return binary(Op::I64Sub);
        case MOpcode::IMul: return binary(Op::I64Mul);
        case MOpcode::And: return binary(Op::I64And);
        case MOpcode::Or: return binary(Op::I64Or);
        case MOpcode::Xor: return binary(Op::I64Xor);
        case MOpcode::Shl: return binary(Op::I64Shl);
        case MOpcode::Shr:
            // x86 masks the shift count to 6 bits for 64-bit operands and wasm
            // takes it modulo 64, so no explicit masking is needed.
            return binary(inst.isSigned ? Op::I64ShrS : Op::I64ShrU);

        case MOpcode::Neg:
            if (ops.empty()) return fail("malformed Neg", errorOut);
            code_.i64Const(0);
            if (!pushOperand(ops[0], errorOut)) return false;
            code_.op(Op::I64Sub);
            return storeToOperand(ops[0], errorOut);

        case MOpcode::Not:
            if (ops.empty()) return fail("malformed Not", errorOut);
            if (!pushOperand(ops[0], errorOut)) return false;
            code_.i64Const(-1);
            code_.op(Op::I64Xor);
            return storeToOperand(ops[0], errorOut);

        case MOpcode::Div:
        case MOpcode::Mod: {
            // def0 = use1 / use2. Both x86 idiv and wasm i64.div_s trap on a
            // zero divisor and on the INT64_MIN / -1 overflow, so the trapping
            // behaviour carries over unchanged.
            if (ops.size() < 3) return fail("malformed Div/Mod", errorOut);
            if (!pushOperand(ops[1], errorOut)) return false;
            if (!pushOperand(ops[2], errorOut)) return false;
            if (inst.op == MOpcode::Div) {
                code_.op(inst.isSigned ? Op::I64DivS : Op::I64DivU);
            } else {
                code_.op(inst.isSigned ? Op::I64RemS : Op::I64RemU);
            }
            return storeToOperand(ops[0], errorOut);
        }

        case MOpcode::UMulHi: {
            // def0 = (use1 * use2) >> 64, unsigned. Delegated to a synthesized
            // helper, since wasm has no widening multiply.
            if (ops.size() < 3) return fail("malformed UMulHi", errorOut);
            auto helper = callees_.find(InstructionSelector::kWasmUMulHiSymbol);
            if (helper == callees_.end()) {
                return fail("internal: the 64x64 high-multiply helper was not "
                            "registered in this module",
                            errorOut);
            }
            if (!pushOperand(ops[1], errorOut)) return false;
            if (!pushOperand(ops[2], errorOut)) return false;
            code_.call(helper->second.funcIndex);
            return storeToOperand(ops[0], errorOut);
        }

        case MOpcode::Ext:
            if (ops.empty()) return fail("malformed Ext", errorOut);
            if (!pushOperand(ops[0], errorOut)) return false;
            emitWidthAdjust(inst.width, inst.isSigned);
            return storeToOperand(ops[0], errorOut);

        case MOpcode::Load: {
            // def0 = [frameSlot1], sign/zero extended to 64 bits.
            if (ops.size() < 2 || ops[1].kind != OperandKind::FrameSlot) {
                return fail("malformed Load", errorOut);
            }
            const std::uint32_t slot = ops[1].frameSlot;
            std::uint32_t offset = 0;
            if (slotMemoryOffset(slot, offset)) {
                Op loadOp = Op::I64Load;
                if (!loadOpFor(inst.width, inst.isSigned, loadOp)) {
                    return fail("unsupported load width", errorOut);
                }
                code_.localGet(framePtrLocal_);
                code_.load(loadOp, /*alignLog2=*/0, offset);
            } else {
                // A slot held in a local keeps the whole 64-bit value, so only
                // the width's extension has to be reapplied.
                if (!pushOperand(ops[1], errorOut)) return false;
                emitWidthAdjust(inst.width, inst.isSigned);
            }
            return storeToOperand(ops[0], errorOut);
        }

        case MOpcode::Store: {
            // [frameSlot0] = low `width` bytes of use1.
            if (ops.size() < 2 || ops[0].kind != OperandKind::FrameSlot) {
                return fail("malformed Store", errorOut);
            }
            const std::uint32_t slot = ops[0].frameSlot;
            std::uint32_t offset = 0;
            if (slotMemoryOffset(slot, offset)) {
                Op storeOp = Op::I64Store;
                if (!storeOpFor(inst.width, storeOp)) {
                    return fail("unsupported store width", errorOut);
                }
                code_.localGet(framePtrLocal_);
                if (!pushOperand(ops[1], errorOut)) return false;
                code_.store(storeOp, /*alignLog2=*/0, offset);
                return true;
            }
            // Local-resident: keep the full value; the width is applied on load.
            if (!pushOperand(ops[1], errorOut)) return false;
            return storeToOperand(ops[0], errorOut);
        }

        case MOpcode::LoadInd: {
            // def0 = [base1 + imm2], sign/zero extended to 64 bits.
            if (ops.size() < 3) return fail("malformed LoadInd", errorOut);
            Op loadOp = Op::I64Load;
            if (!loadOpFor(inst.width, inst.isSigned, loadOp)) {
                return fail("unsupported load width", errorOut);
            }
            std::uint32_t offset = 0;
            if (!pushAddress(ops[1], ops[2].imm, offset, errorOut)) return false;
            code_.load(loadOp, /*alignLog2=*/0, offset);
            return storeToOperand(ops[0], errorOut);
        }

        case MOpcode::StoreInd: {
            // [base0 + imm1] = low `width` bytes of use2.
            if (ops.size() < 3) return fail("malformed StoreInd", errorOut);
            Op storeOp = Op::I64Store;
            if (!storeOpFor(inst.width, storeOp)) {
                return fail("unsupported store width", errorOut);
            }
            std::uint32_t offset = 0;
            if (!pushAddress(ops[0], ops[1].imm, offset, errorOut)) return false;
            if (!pushOperand(ops[2], errorOut)) return false;
            code_.store(storeOp, /*alignLog2=*/0, offset);
            return true;
        }

        case MOpcode::LeaSlot: {
            // def0 = &[frameSlot1]. Only meaningful for a slot in memory; a slot
            // held in a local has no address, and assignLocals guarantees that
            // any slot reached by a LeaSlot was placed in the frame.
            if (ops.size() < 2 || ops[1].kind != OperandKind::FrameSlot) {
                return fail("malformed LeaSlot", errorOut);
            }
            std::uint32_t offset = 0;
            if (!slotMemoryOffset(ops[1].frameSlot, offset)) {
                return fail("internal: address taken of a slot that was not "
                            "placed in the frame",
                            errorOut);
            }
            code_.localGet(framePtrLocal_);
            if (offset != 0) {
                code_.i32Const(static_cast<std::int32_t>(offset));
                code_.op(Op::I32Add);
            }
            code_.op(Op::I64ExtendI32U);  // back into the 64-bit address model
            return storeToOperand(ops[0], errorOut);
        }

        case MOpcode::Lea: {
            // def0 = &symbol1, for a string literal, module global, or function.
            if (ops.size() < 2 || ops[1].kind != OperandKind::Symbol) {
                return fail("malformed Lea", errorOut);
            }
            // A function has no address on wasm: code does not live in linear
            // memory. `&fn` therefore yields its slot in the function table, which
            // is what call_indirect consumes.
            if (tableSlots_) {
                auto slot = tableSlots_->find(ops[1].symbol);
                if (slot != tableSlots_->end()) {
                    code_.i64Const(static_cast<std::int64_t>(slot->second));
                    return storeToOperand(ops[0], errorOut);
                }
            }
            std::uint32_t address = 0;
            if (!data_.addressOf(ops[1].symbol, address)) {
                return fail("no address assigned to data symbol '" + ops[1].symbol + "'",
                            errorOut);
            }
            code_.i64Const(static_cast<std::int64_t>(address));
            return storeToOperand(ops[0], errorOut);
        }

        case MOpcode::CallIndirect: {
            // operands: slot(target), imm(hasResult), imm(isFloat) per argument.
            // Every table entry is a thunk with a uniform all-i64 signature, so
            // the type named here depends only on the argument count.
            if (ops.size() < 2 || ops[0].kind != OperandKind::FrameSlot) {
                return fail("malformed CallIndirect", errorOut);
            }
            const bool hasResult = ops[1].imm != 0;
            const std::uint32_t argCount = static_cast<std::uint32_t>(ops.size() - 2);

            // Read each argument back out of the register the selector put it in,
            // replaying the same two-cursor assignment.
            std::size_t gpCursor = 0;
            std::size_t xmmCursor = 0;
            for (std::uint32_t i = 0; i < argCount; ++i) {
                const bool isFloat = ops[2 + i].imm != 0;
                std::uint32_t local = 0;
                if (isFloat) {
                    const std::size_t idx =
                        abi_.sharedArgRegIndex ? gpCursor : xmmCursor;
                    if (idx >= abi_.xmmArgRegs.size()) {
                        return fail("indirect calls cannot pass floating-point "
                                    "arguments on the stack yet",
                                    errorOut);
                    }
                    if (!xmmLocal(abi_.xmmArgRegs[idx], local)) {
                        return fail("internal: no local for an SSE argument register "
                                    "in an indirect call",
                                    errorOut);
                    }
                    if (abi_.sharedArgRegIndex) ++gpCursor;
                    ++xmmCursor;
                } else {
                    if (gpCursor >= abi_.intArgRegs.size()) {
                        return fail("indirect calls cannot pass arguments on the "
                                    "stack yet",
                                    errorOut);
                    }
                    if (!physLocal(abi_.intArgRegs[gpCursor], local)) {
                        return fail("internal: no local for an argument register in "
                                    "an indirect call",
                                    errorOut);
                    }
                    ++gpCursor;
                    if (abi_.sharedArgRegIndex) ++xmmCursor;
                }
                // Thunks take raw i64s, so no conversion is needed either way.
                code_.localGet(local);
            }

            // The table index is in the target slot, as an i64 in the address
            // model; call_indirect wants an i32.
            if (!pushOperand(ops[0], errorOut)) return false;
            code_.op(Op::I32WrapI64);

            if (!indirectTypes_) {
                return fail("internal: no indirect-call signatures were registered",
                            errorOut);
            }
            const std::uint32_t key = argCount * 2 + (hasResult ? 1 : 0);
            auto typeIndex = indirectTypes_->find(key);
            if (typeIndex == indirectTypes_->end()) {
                return fail("internal: no thunk signature for this indirect call",
                            errorOut);
            }
            code_.callIndirect(typeIndex->second, /*tableIndex=*/0);

            if (!hasResult) return true;
            std::uint32_t retLocal = 0;
            if (!physLocal(abi_.intReturnReg, retLocal)) {
                code_.op(Op::Drop);
                return true;
            }
            code_.localSet(retLocal);
            return true;
        }

        case MOpcode::LeaDisp: {
            // def0 = base1 + imm2, in the 64-bit address model.
            if (ops.size() < 3) return fail("malformed LeaDisp", errorOut);
            if (!pushOperand(ops[1], errorOut)) return false;
            if (ops[2].imm != 0) {
                code_.i64Const(ops[2].imm);
                code_.op(Op::I64Add);
            }
            return storeToOperand(ops[0], errorOut);
        }

        case MOpcode::LeaIndex: {
            // def0 = base1 + index2 * scale + imm3. x86 folds this into one SIB
            // lea; wasm needs the multiply and adds spelled out.
            if (ops.size() < 4) return fail("malformed LeaIndex", errorOut);
            if (!pushOperand(ops[1], errorOut)) return false;
            if (!pushOperand(ops[2], errorOut)) return false;
            if (inst.scale > 1) {
                code_.i64Const(inst.scale);
                code_.op(Op::I64Mul);
            }
            code_.op(Op::I64Add);
            if (ops[3].imm != 0) {
                code_.i64Const(ops[3].imm);
                code_.op(Op::I64Add);
            }
            return storeToOperand(ops[0], errorOut);
        }

        case MOpcode::Cmp:
            // Produces EFLAGS, which has no wasm equivalent. Hold the operands
            // and let the consuming Jcc/SetCC emit the comparison. An
            // unconsumed compare is dead and simply never emitted -- the
            // optimizer leaves one behind whenever it drops a `Jcc -> next`.
            if (ops.size() < 2) return fail("malformed Cmp", errorOut);
            pendingLhs_ = ops[0];
            pendingRhs_ = ops[1];
            hasPendingCmp_ = true;
            return true;

        case MOpcode::SetCC: {
            // def0 = (condition holds) ? 1 : 0, zero-extended to 64 bits.
            if (ops.empty()) return fail("malformed SetCC", errorOut);
            if (!emitPendingCompare(inst.cond, errorOut)) return false;
            code_.op(Op::I64ExtendI32U);
            return storeToOperand(ops[0], errorOut);
        }

        case MOpcode::Jmp:
            if (ops.empty() || ops[0].kind != OperandKind::Label) {
                return fail("malformed Jmp", errorOut);
            }
            return emitBranchToBlock(ops[0].label, 0, errorOut);

        case MOpcode::Jcc: {
            // Branch when the condition holds; otherwise fall through to the
            // next instruction. The `if` scope is one level deeper, so the
            // branch depth is adjusted for it.
            if (ops.empty() || ops[0].kind != OperandKind::Label) {
                return fail("malformed Jcc", errorOut);
            }
            if (!emitPendingCompare(inst.cond, errorOut)) return false;
            code_.ifThen();
            if (!emitBranchToBlock(ops[0].label, /*extraScopes=*/1, errorOut)) {
                return false;
            }
            code_.op(Op::End);
            return true;
        }

        case MOpcode::Call:
        case MOpcode::CallImport:
            // CallImport names a symbol plus the library to import it from; on
            // wasm both resolve to the same `call`, because a wasm import is
            // just a function occupying a low index.
            if (ops.empty() || ops[0].kind != OperandKind::Symbol) {
                return fail("malformed call", errorOut);
            }
            return emitCall(ops[0].symbol, errorOut);

        // --- floating point ------------------------------------------------
        // Every operand is an XMM-class location holding raw bits, so each
        // instruction reinterprets on the way in and back on the way out. The
        // engine folds the reinterprets away; they are bitcasts.
        case MOpcode::FMovRR:
            // A register move copies bits; no interpretation needed.
            if (ops.size() < 2) return fail("malformed FMovRR", errorOut);
            if (!pushFloatBits(ops[1], errorOut)) return false;
            return storeToOperand(ops[0], errorOut);

        case MOpcode::FConst: {
            // The immediate is the raw bit pattern for the width.
            if (ops.size() < 2 || ops[1].kind != OperandKind::Imm) {
                return fail("malformed FConst", errorOut);
            }
            std::uint64_t bits = static_cast<std::uint64_t>(ops[1].imm);
            if (inst.width == 2) {
                // The immediate is a 16-bit half; registers hold the f32 compute
                // form, so expand it here rather than at run time.
                bits = halfBitsToF32Bits(static_cast<std::uint32_t>(bits & 0xFFFFu));
            } else if (inst.width == 4) {
                bits &= 0xFFFFFFFFull;
            }
            code_.i64Const(static_cast<std::int64_t>(bits));
            return storeToOperand(ops[0], errorOut);
        }

        case MOpcode::FAdd:
        case MOpcode::FSub:
        case MOpcode::FMul:
        case MOpcode::FDiv: {
            if (ops.size() < 2) return fail("malformed float arithmetic", errorOut);
            if (!pushFloat(ops[0], inst.width, errorOut)) return false;
            if (!pushFloat(ops[1], inst.width, errorOut)) return false;
            const bool single = inst.width == 4;
            switch (inst.op) {
                case MOpcode::FAdd: code_.op(single ? Op::F32Add : Op::F64Add); break;
                case MOpcode::FSub: code_.op(single ? Op::F32Sub : Op::F64Sub); break;
                case MOpcode::FMul: code_.op(single ? Op::F32Mul : Op::F64Mul); break;
                default: code_.op(single ? Op::F32Div : Op::F64Div); break;
            }
            return storeFloat(ops[0], inst.width, errorOut);
        }

        case MOpcode::FNeg:
            if (ops.empty()) return fail("malformed FNeg", errorOut);
            if (!pushFloat(ops[0], inst.width, errorOut)) return false;
            code_.op(inst.width == 4 ? Op::F32Neg : Op::F64Neg);
            return storeFloat(ops[0], inst.width, errorOut);

        case MOpcode::FCmp:
            // Sets flags for a following SetCC/Jcc, exactly like Cmp. Recorded
            // rather than emitted; the consumer picks the comparison.
            if (ops.size() < 2) return fail("malformed FCmp", errorOut);
            pendingLhs_ = ops[0];
            pendingRhs_ = ops[1];
            hasPendingCmp_ = true;
            pendingCmpIsFloat_ = true;
            pendingCmpWidth_ = inst.width;
            return true;

        case MOpcode::PXorRR:
            // Used to zero a register and to flip a sign bit. Only the low 64
            // bits of the 128-bit operation are ever observed here.
            if (ops.size() < 2) return fail("malformed PXorRR", errorOut);
            if (!pushFloatBits(ops[0], errorOut)) return false;
            if (!pushFloatBits(ops[1], errorOut)) return false;
            code_.op(Op::I64Xor);
            return storeToOperand(ops[0], errorOut);

        case MOpcode::FMovToGpr:
        case MOpcode::FMovFromGpr:
            // Bitcasts between the register files. Both sides are i64 locals
            // holding bits, so this is a plain copy.
            if (ops.size() < 2) return fail("malformed float bitcast", errorOut);
            if (!pushOperand(ops[1], errorOut)) return false;
            return storeToOperand(ops[0], errorOut);

        case MOpcode::CvtI2F:
            // def0(xmm) = (float)use1(gpr)
            if (ops.size() < 2) return fail("malformed CvtI2F", errorOut);
            if (!pushOperand(ops[1], errorOut)) return false;
            if (inst.width == 4) {
                code_.op(inst.isSigned ? Op::F32ConvertI64S : Op::F32ConvertI64U);
            } else {
                code_.op(inst.isSigned ? Op::F64ConvertI64S : Op::F64ConvertI64U);
            }
            return storeFloat(ops[0], inst.width, errorOut);

        case MOpcode::CvtF2I:
            // def0(gpr) = (i64)use1(xmm), truncating toward zero.
            //
            // Saturating truncation is used deliberately: wasm's plain
            // i64.trunc_f64_s traps on NaN or out-of-range, whereas x86
            // cvttsd2si produces the "integer indefinite" value. Saturating
            // never traps, which is the closer match to the native behaviour.
            if (ops.size() < 2) return fail("malformed CvtF2I", errorOut);
            if (!pushFloat(ops[1], inst.width, errorOut)) return false;
            if (inst.width == 4) {
                code_.opFC(inst.isSigned ? OpFC::I64TruncSatF32S
                                         : OpFC::I64TruncSatF32U);
            } else {
                code_.opFC(inst.isSigned ? OpFC::I64TruncSatF64S
                                         : OpFC::I64TruncSatF64U);
            }
            return storeToOperand(ops[0], errorOut);

        case MOpcode::CvtF2F: {
            // width names the DESTINATION precision.
            if (ops.size() < 2) return fail("malformed CvtF2F", errorOut);
            const std::uint8_t sourceWidth = inst.width == 4 ? 8 : 4;
            if (!pushFloat(ops[1], sourceWidth, errorOut)) return false;
            code_.op(inst.width == 4 ? Op::F32DemoteF64 : Op::F64PromoteF32);
            return storeFloat(ops[0], inst.width, errorOut);
        }

        case MOpcode::FLoad: {
            // def0(xmm) = [frameSlot1]
            if (ops.size() < 2 || ops[1].kind != OperandKind::FrameSlot) {
                return fail("malformed FLoad", errorOut);
            }
            std::uint32_t offset = 0;
            Op loadOp = Op::I64Load;
            if (inst.width == 2) loadOp = Op::I64Load16U;
            else if (inst.width == 4) loadOp = Op::I64Load32U;
            if (slotMemoryOffset(ops[1].frameSlot, offset)) {
                code_.localGet(framePtrLocal_);
                code_.load(loadOp, /*alignLog2=*/0, offset);
            } else if (!pushOperand(ops[1], errorOut)) {
                return false;
            }
            if (inst.width == 2 && !emitHalfToF32(errorOut)) return false;
            return storeToOperand(ops[0], errorOut);
        }

        case MOpcode::FStore: {
            // [frameSlot0] = use1(xmm)
            if (ops.size() < 2 || ops[0].kind != OperandKind::FrameSlot) {
                return fail("malformed FStore", errorOut);
            }
            std::uint32_t offset = 0;
            Op storeOp = Op::I64Store;
            if (inst.width == 2) storeOp = Op::I64Store16;
            else if (inst.width == 4) storeOp = Op::I64Store32;
            const bool inMemory = slotMemoryOffset(ops[0].frameSlot, offset);
            if (inMemory) code_.localGet(framePtrLocal_);
            if (!pushFloatBits(ops[1], errorOut)) return false;
            if (inst.width == 2 && !emitF32ToHalf(errorOut)) return false;
            if (inMemory) {
                code_.store(storeOp, /*alignLog2=*/0, offset);
                return true;
            }
            return storeToOperand(ops[0], errorOut);
        }

        case MOpcode::FLoadInd: {
            // def0(xmm) = [base1 + imm2]
            if (ops.size() < 3) return fail("malformed FLoadInd", errorOut);
            std::uint32_t offset = 0;
            Op loadOp = Op::I64Load;
            if (inst.width == 2) loadOp = Op::I64Load16U;
            else if (inst.width == 4) loadOp = Op::I64Load32U;
            if (!pushAddress(ops[1], ops[2].imm, offset, errorOut)) return false;
            code_.load(loadOp, /*alignLog2=*/0, offset);
            if (inst.width == 2 && !emitHalfToF32(errorOut)) return false;
            return storeToOperand(ops[0], errorOut);
        }

        case MOpcode::FStoreInd: {
            // [base0 + imm1] = use2(xmm)
            if (ops.size() < 3) return fail("malformed FStoreInd", errorOut);
            std::uint32_t offset = 0;
            Op storeOp = Op::I64Store;
            if (inst.width == 2) storeOp = Op::I64Store16;
            else if (inst.width == 4) storeOp = Op::I64Store32;
            if (!pushAddress(ops[0], ops[1].imm, offset, errorOut)) return false;
            if (!pushFloatBits(ops[2], errorOut)) return false;
            if (inst.width == 2 && !emitF32ToHalf(errorOut)) return false;
            code_.store(storeOp, /*alignLog2=*/0, offset);
            return true;
        }

        case MOpcode::CvtF16ToF32:
        case MOpcode::CvtF32ToF16: {
            // wasm has no f16, so these go through the software conversion
            // helpers in the core runtime. Both take and return raw bit patterns
            // as u32, which is an i32 at the wasm boundary.
            if (ops.size() < 2) return fail("malformed f16 conversion", errorOut);
            const char* symbol = inst.op == MOpcode::CvtF16ToF32
                                     ? "__ins_f16_to_f32"
                                     : "__ins_f32_to_f16";
            auto helper = callees_.find(symbol);
            if (helper == callees_.end()) {
                return fail(std::string("internal: the f16 conversion helper '") +
                                symbol + "' was not registered in this module",
                            errorOut);
            }
            if (!pushFloatBits(ops[1], errorOut)) return false;
            if (inst.op == MOpcode::CvtF16ToF32) {
                // Only the low 16 bits are the half; discard anything above.
                code_.i64Const(0xFFFF);
                code_.op(Op::I64And);
            }
            code_.op(Op::I32WrapI64);
            code_.call(helper->second.funcIndex);
            code_.op(Op::I64ExtendI32U);
            return storeToOperand(ops[0], errorOut);
        }

        case MOpcode::StoreXmmLo16: {
            // [frameSlot0] = low 16 bits of use1(xmm), i.e. packed f16 storage.
            if (ops.size() < 2 || ops[0].kind != OperandKind::FrameSlot) {
                return fail("malformed StoreXmmLo16", errorOut);
            }
            std::uint32_t offset = 0;
            if (slotMemoryOffset(ops[0].frameSlot, offset)) {
                code_.localGet(framePtrLocal_);
                if (!pushFloatBits(ops[1], errorOut)) return false;
                code_.store(Op::I64Store16, /*alignLog2=*/0, offset);
                return true;
            }
            if (!pushFloatBits(ops[1], errorOut)) return false;
            code_.i64Const(0xFFFF);
            code_.op(Op::I64And);
            return storeToOperand(ops[0], errorOut);
        }

        case MOpcode::StoreOutgoing:
        case MOpcode::FStoreOutgoing: {
            // [frame base + imm0] = use1, an argument being staged for a call that
            // has more of them than there are argument registers.
            if (ops.size() < 2 || ops[0].kind != OperandKind::Imm) {
                return fail("malformed outgoing-argument store", errorOut);
            }
            if (!hasFrame_) {
                return fail("internal: an outgoing argument was stored but no "
                            "outgoing area was reserved",
                            errorOut);
            }
            code_.localGet(framePtrLocal_);
            if (inst.op == MOpcode::FStoreOutgoing) {
                if (!pushFloatBits(ops[1], errorOut)) return false;
                if (inst.width == 2 && !emitF32ToHalf(errorOut)) return false;
            } else if (!pushOperand(ops[1], errorOut)) {
                return false;
            }
            // The slot is a full eightbyte whatever the value's width.
            code_.store(Op::I64Store, /*alignLog2=*/0,
                        static_cast<std::uint32_t>(ops[0].imm));
            return true;
        }

        case MOpcode::AsmBlock:
            // A raw `asm( ... )` block is x86-64 machine code. wasm has no
            // instruction stream to splice it into, so this can never be
            // translated -- say that rather than reporting an opaque opcode.
            return fail(
                "inline asm blocks are x86-64 machine code and cannot be "
                "translated to WebAssembly; put the block behind "
                "`#if @targetIs(\"x86_64\")` and provide a wasm path alongside it",
                errorOut);

        case MOpcode::AsmFixed: {
            // A recognized fixed inline-asm template, selected by an immediate.
            // Only the two with a direct wasm equivalent are accepted; the rest
            // are x86 instructions with no counterpart.
            if (ops.empty() || ops[0].kind != OperandKind::Imm) {
                return fail("malformed AsmFixed", errorOut);
            }
            switch (ops[0].imm) {
                case 0:  // nop
                    code_.op(Op::Nop);
                    return true;
                case 4:  // ud2 -> an unconditional trap
                    code_.op(Op::Unreachable);
                    return true;
                default:
                    return fail("inline assembly is not supported on wasm", errorOut);
            }
        }

        case MOpcode::Ret:
            return emitReturn(errorOut);

        default:
            if (const char* why = unsupportedReason(inst.op)) {
                return fail(why, errorOut);
            }
            return fail(std::string("cannot translate Machine-IR opcode '") +
                            opcodeName(inst.op) + "' to WebAssembly",
                        errorOut);
    }
}

bool FunctionEmitter::run(FuncType& signatureOut, FunctionBody& bodyOut,
                          std::string& errorOut) {
    if (fn_.naked) {
        return fail("[naked(on)] cannot be honoured on wasm: a function body is a "
                    "structured instruction sequence, not raw bytes, so there is no "
                    "prologue to suppress and no way to supply one by hand",
                    errorOut);
    }
    if (!fn_.customSection.empty()) {
        return fail("[section(\"" + fn_.customSection +
                        "\")] cannot be honoured on wasm: code lives in the code "
                        "section and is addressed by function index, so there are no "
                        "named code sections to place a function in",
                    errorOut);
    }
    // Directives that other backends implement but wasm cannot, checked here so
    // they fail loudly instead of being quietly dropped.
    if (info_.decl) {
        for (const auto& attr : info_.decl->attributes) {
            const bool on = attr.value.empty() || attr.value == "on";
            if (attr.name == "interrupt" && on) {
                return fail("[interrupt(on)] cannot be honoured on wasm: there are no "
                            "interrupts and no interrupt calling convention",
                            errorOut);
            }
            if (attr.name == "conv" && !attr.value.empty() && attr.value != "cdecl") {
                return fail("calling convention '" + attr.value +
                                "' cannot be honoured on wasm: it has exactly one "
                                "calling convention",
                            errorOut);
            }
        }
    }
    if (!assignLocals(errorOut)) return false;
    emitFramePrologue();
    if (!emitEntrySeed(errorOut)) return false;

    // A function with no inter-block branches needs no dispatcher; emitting it
    // flat avoids the state variable and the surrounding scopes entirely.
    bool hasBranches = fn_.blocks().size() > 1;
    if (!hasBranches) {
        for (const auto& block : fn_.blocks()) {
            for (const auto& inst : block.insts) {
                if (inst.op == MOpcode::Jmp || inst.op == MOpcode::Jcc) {
                    hasBranches = true;  // a self-loop in a single block
                    break;
                }
            }
        }
    }

    if (hasBranches) {
        if (!emitDispatchLoop(errorOut)) return false;
    } else {
        currentBlock_ = 0;
        for (const auto& block : fn_.blocks()) {
            for (const auto& inst : block.insts) {
                if (!emitInst(inst, errorOut)) return false;
            }
        }
        // The selector appends a trailing Ret to every non-naked function, so
        // falling off the end is impossible. `unreachable` says exactly that: it
        // satisfies any result signature without having to synthesize a zero of
        // the right type, and if the assumption were ever wrong it traps rather
        // than returning a fabricated value.
        code_.op(Op::Unreachable);
    }

    signatureOut = signature_;
    bodyOut.locals = locals_;
    bodyOut.code = code_.take();
    return true;
}

// ---------------------------------------------------------------------------
// Module assembly
// ---------------------------------------------------------------------------

// A WASI command exports `_start` with no result, but Insty's `main` returns an
// exit status. Bridge them with a synthesized shim that calls `main` and hands
// the result to proc_exit.
FunctionBody buildStartShim(std::uint32_t mainIndex, bool mainReturnsI32,
                            std::uint32_t procExitIndex) {
    CodeBuilder b;
    b.call(mainIndex);
    if (!mainReturnsI32) {
        b.i32Const(0);
    }
    b.call(procExitIndex);
    // proc_exit does not return; nothing further can execute.
    b.op(Op::Unreachable);

    FunctionBody body;
    body.code = b.take();
    return body;
}

// Places every data symbol the program references and sizes linear memory.
//
// Order matters: initialized bytes (string literals, then globals with a foldable
// initializer) come first as one contiguous data segment, and zero-initialized
// globals are appended afterwards as pure address space. wasm zeroes memory on
// instantiation, so that tail costs nothing in the module -- it is .bss without
// needing a .bss.
void layoutData(const Sema::SemaResult& sema, const AST::ProgramRoot* program,
                const std::vector<std::unique_ptr<MFunction>>& functions,
                DataLayout& layout) {
    std::uint32_t cursor = kDataBase;

    auto placeInitialized = [&](const std::string& symbol, const std::uint8_t* bytes,
                                std::uint64_t size, unsigned align) {
        cursor = alignUp(cursor, align ? align : 1);
        // Pad the blob so its start matches the aligned address.
        while (layout.initialData.size() < cursor - kDataBase) {
            layout.initialData.push_back(0);
        }
        layout.symbolAddress[symbol] = cursor;
        for (std::uint64_t i = 0; i < size; ++i) {
            layout.initialData.push_back(bytes[i]);
        }
        cursor += static_cast<std::uint32_t>(size);
    };

    // String and raw blob constants interned by the selector. Two functions using
    // the same literal get separate symbols, so dedupe by content as well --
    // otherwise every use of "hello" costs another copy. Matches the x86 path,
    // which interns .rodata by content.
    std::unordered_map<std::string, std::uint32_t> contentAddress;
    for (const auto& fn : functions) {
        for (const auto& sc : fn->stringConstants()) {
            if (layout.symbolAddress.count(sc.symbol)) continue;
            std::string content = sc.bytes;
            if (sc.appendNul) content.push_back('\0');
            const unsigned align = sc.align ? sc.align : 1;
            // Alignment is part of the key: identical bytes needed at different
            // alignments cannot share one placement.
            const std::string key = std::to_string(align) + ":" + content;
            auto existing = contentAddress.find(key);
            if (existing != contentAddress.end()) {
                layout.symbolAddress[sc.symbol] = existing->second;
                continue;
            }
            placeInitialized(sc.symbol,
                             reinterpret_cast<const std::uint8_t*>(content.data()),
                             content.size(), align);
            contentAddress[key] = layout.symbolAddress[sc.symbol];
        }
    }

    // Module-level globals. SemaResult drops the initializer, so recover it from
    // the AST by name, exactly as the x86 path does.
    std::unordered_map<std::string, const AST::VariableDeclarationExpr*> initByName;
    if (program) {
        for (const auto& node : program->body) {
            if (!node || node->nodeType() != AST::NodeType::VariableDeclaration) continue;
            const auto* vd = static_cast<const AST::VariableDeclarationExpr*>(node.get());
            initByName[vd->identifier] = vd;
        }
    }

    std::vector<const Sema::GlobalInfo*> zeroInit;
    for (const auto& g : sema.globals) {
        if (!g.type || g.type->isError()) continue;
        SizeAlign sa = scalarSizeAlign(g.type);
        if (sa.size == 0) sa.size = 1;

        __int128 value = 0;
        bool haveConst = false;
        const bool scalarInt = g.type->kind == Types::Kind::Int ||
                               g.type->kind == Types::Kind::Bool ||
                               g.type->kind == Types::Kind::Pointer;
        auto it = initByName.find(g.name);
        if (scalarInt && it != initByName.end() && it->second->initialValue) {
            haveConst = evalConstInt(it->second->initialValue.get(), value);
        }

        if (!haveConst) {
            zeroInit.push_back(&g);
            continue;
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(sa.size), 0);
        unsigned __int128 raw = static_cast<unsigned __int128>(value);
        for (std::uint64_t b = 0; b < sa.size && b < 16; ++b) {
            bytes[static_cast<std::size_t>(b)] =
                static_cast<std::uint8_t>((raw >> (8 * b)) & 0xFF);
        }
        placeInitialized(g.name, bytes.data(), sa.size, sa.align);
    }

    // Zero-initialized globals: address space only, no bytes in the module.
    for (const Sema::GlobalInfo* g : zeroInit) {
        SizeAlign sa = scalarSizeAlign(g->type);
        if (sa.size == 0) sa.size = 1;
        cursor = alignUp(cursor, sa.align ? sa.align : 1);
        layout.symbolAddress[g->name] = cursor;
        cursor += static_cast<std::uint32_t>(sa.size);
    }

    layout.dataEnd = cursor;
    layout.stackTop = alignUp(layout.dataEnd, kStackAlign) + kStackSize;
    // The heap starts above the stack and grows upward into fresh pages.
    layout.heapBase = alignUp(layout.stackTop, kHeapAlign);
    layout.memoryPages = (layout.heapBase + kPageSize - 1) / kPageSize;
    if (layout.memoryPages == 0) layout.memoryPages = 1;
}

// ---------------------------------------------------------------------------
// Runtime allocator
// ---------------------------------------------------------------------------

// Builds __wasm_alloc(size: i64, align: i64) -> i64.
//
// A bump allocator over linear memory. wasm gives no OS heap, and the native
// lowerings reach for mmap or HeapAlloc, so the wasm target needs its own. This
// cannot be written in Insty because it needs memory.grow and a mutable global,
// neither of which the Machine IR can express, so it is emitted directly.
//
// __wasm_free rolls the cursor back when the block being freed is the most
// recent allocation, which makes an allocate/free loop run in constant memory
// instead of exhausting it. Anything else leaks until the module is discarded.
FunctionBody buildWasmAlloc(std::uint32_t heapCursorGlobal) {
    // locals: 0 = size (i64 param), 1 = align (i64 param),
    //         2 = block start (i32), 3 = new cursor (i32), 4 = scratch (i32)
    CodeBuilder b;

    // A zero-sized request still needs a distinct address.
    b.localGet(0);
    b.i64Const(0);
    b.op(Op::I64Eq);
    b.ifThen();
    b.i64Const(1);
    b.localSet(0);
    b.op(Op::End);

    // Clamp the alignment to at least the guaranteed minimum, so the mask below
    // is always a valid power of two.
    b.localGet(1);
    b.i64Const(kHeapAlign);
    b.op(Op::I64LtU);
    b.ifThen();
    b.i64Const(kHeapAlign);
    b.localSet(1);
    b.op(Op::End);

    // block = (cursor + align - 1) & ~(align - 1)
    b.globalGet(heapCursorGlobal);
    b.localGet(1);
    b.op(Op::I32WrapI64);
    b.op(Op::I32Add);
    b.i32Const(1);
    b.op(Op::I32Sub);
    b.localGet(1);
    b.op(Op::I32WrapI64);
    b.i32Const(1);
    b.op(Op::I32Sub);
    b.i32Const(-1);
    b.op(Op::I32Xor);  // ~(align - 1)
    b.op(Op::I32And);
    b.localSet(2);

    // newCursor = block + size
    b.localGet(2);
    b.localGet(0);
    b.op(Op::I32WrapI64);
    b.op(Op::I32Add);
    b.localSet(3);

    // Grow memory while it does not reach newCursor.
    b.localGet(3);
    b.memorySize();
    b.i32Const(16);  // log2(64 KiB): pages -> bytes
    b.op(Op::I32Shl);
    b.op(Op::I32GtU);
    b.ifThen();
    {
        // pages = ceil((newCursor - size_in_bytes) / 64KiB)
        b.localGet(3);
        b.memorySize();
        b.i32Const(16);
        b.op(Op::I32Shl);
        b.op(Op::I32Sub);
        b.i32Const(static_cast<std::int32_t>(kPageSize) - 1);
        b.op(Op::I32Add);
        b.i32Const(16);
        b.op(Op::I32ShrU);
        b.memoryGrow();
        b.localSet(4);
        // memory.grow answers -1 when it cannot satisfy the request.
        b.localGet(4);
        b.i32Const(-1);
        b.op(Op::I32Eq);
        b.ifThen();
        b.i64Const(0);  // out of memory: a null pointer
        b.op(Op::Return);
        b.op(Op::End);
    }
    b.op(Op::End);

    b.localGet(3);
    b.globalSet(heapCursorGlobal);

    b.localGet(2);
    b.op(Op::I64ExtendI32U);  // back into the 64-bit address model

    FunctionBody body;
    body.locals = {ValType::I32, ValType::I32, ValType::I32};
    body.code = b.take();
    return body;
}

// Builds the table thunk for one address-taken function.
//
// wasm's call_indirect checks the callee's type at run time, so every entry in
// the table has to share one signature or an indirect call could not name a type
// that fits them all. Each entry is therefore a small wrapper taking and
// returning raw i64s, which converts to the real signature and calls it. That
// also means `&fn` needs no knowledge of the target's parameter types.
FunctionBody buildIndirectThunk(const Callee& target, std::uint32_t targetIndex) {
    CodeBuilder b;
    for (std::size_t i = 0; i < target.signature.params.size(); ++i) {
        b.localGet(static_cast<std::uint32_t>(i));
        switch (target.signature.params[i]) {
            case ValType::I32:
                b.op(Op::I32WrapI64);
                break;
            case ValType::F32:
                b.op(Op::I32WrapI64);
                b.op(Op::F32ReinterpretI32);
                break;
            case ValType::F64:
                b.op(Op::F64ReinterpretI64);
                break;
            default:
                break;  // already i64
        }
    }
    b.call(targetIndex);
    if (!target.signature.results.empty()) {
        switch (target.signature.results[0]) {
            case ValType::I32:
                // Zero-extend, not sign-extend. An indirect call sees the whole
                // 64-bit result, and on x86 a 32-bit write to EAX clears the upper
                // half of RAX -- so this is what the native target leaves behind.
                b.op(Op::I64ExtendI32U);
                break;
            case ValType::F32:
                b.op(Op::I32ReinterpretF32);
                b.op(Op::I64ExtendI32U);
                break;
            case ValType::F64:
                b.op(Op::I64ReinterpretF64);
                break;
            default:
                break;
        }
    }
    FunctionBody body;
    body.code = b.take();
    return body;
}

// Builds __wasm_umulhi(a: i64, b: i64) -> i64, the high 64 bits of an unsigned
// 64x64 product.
//
// x86 gets this from `mul`, which writes a 128-bit result across RDX:RAX. wasm
// has no widening multiply, so the product is assembled from four 32x32 partial
// products. Branch-free.
//
//   hi = ahi*bhi + (alo*bhi >> 32) + (ahi*blo >> 32)
//        + (((alo*blo >> 32) + (alo*bhi & M) + (ahi*blo & M)) >> 32)
//
// The last term is the carry out of the low 64 bits, which is why the middle
// partial products are split rather than just added.
FunctionBody buildWasmUMulHi() {
    // locals: 0 = a, 1 = b (params); 2 = alo, 3 = ahi, 4 = blo, 5 = bhi,
    //         6 = alo*blo, 7 = alo*bhi, 8 = ahi*blo
    constexpr std::int64_t kMask = 0xFFFFFFFFLL;
    CodeBuilder b;
    auto split = [&](std::uint32_t src, std::uint32_t lo, std::uint32_t hi) {
        b.localGet(src);
        b.i64Const(kMask);
        b.op(Op::I64And);
        b.localSet(lo);
        b.localGet(src);
        b.i64Const(32);
        b.op(Op::I64ShrU);
        b.localSet(hi);
    };
    split(0, 2, 3);
    split(1, 4, 5);

    auto product = [&](std::uint32_t x, std::uint32_t y, std::uint32_t dest) {
        b.localGet(x);
        b.localGet(y);
        b.op(Op::I64Mul);
        b.localSet(dest);
    };
    product(2, 4, 6);  // alo*blo
    product(2, 5, 7);  // alo*bhi
    product(3, 4, 8);  // ahi*blo

    // ahi*bhi
    b.localGet(3);
    b.localGet(5);
    b.op(Op::I64Mul);

    // + (alo*bhi >> 32)
    b.localGet(7);
    b.i64Const(32);
    b.op(Op::I64ShrU);
    b.op(Op::I64Add);

    // + (ahi*blo >> 32)
    b.localGet(8);
    b.i64Const(32);
    b.op(Op::I64ShrU);
    b.op(Op::I64Add);

    // + carry out of the low half
    b.localGet(6);
    b.i64Const(32);
    b.op(Op::I64ShrU);
    b.localGet(7);
    b.i64Const(kMask);
    b.op(Op::I64And);
    b.op(Op::I64Add);
    b.localGet(8);
    b.i64Const(kMask);
    b.op(Op::I64And);
    b.op(Op::I64Add);
    b.i64Const(32);
    b.op(Op::I64ShrU);
    b.op(Op::I64Add);

    FunctionBody body;
    body.locals = {ValType::I64, ValType::I64, ValType::I64, ValType::I64,
                   ValType::I64, ValType::I64, ValType::I64};
    body.code = b.take();
    return body;
}

// Builds __wasm_free(ptr: i64, size: i64, align: i64).
//
// Only the last allocation can be reclaimed; see buildWasmAlloc.
FunctionBody buildWasmFree(std::uint32_t heapCursorGlobal) {
    CodeBuilder b;
    // if (ptr + size == cursor) cursor = ptr
    b.localGet(0);
    b.op(Op::I32WrapI64);
    b.localGet(1);
    b.op(Op::I32WrapI64);
    b.op(Op::I32Add);
    b.globalGet(heapCursorGlobal);
    b.op(Op::I32Eq);
    b.ifThen();
    b.localGet(0);
    b.op(Op::I32WrapI64);
    b.globalSet(heapCursorGlobal);
    b.op(Op::End);

    FunctionBody body;
    body.code = b.take();
    return body;
}

}  // namespace

bool emitWasmModule(const Sema::SemaResult& sema, const EmitOptions& options,
                    const std::string& outPath, std::string& errorOut) {
    // wasm is not an x86 ABI, but the selector needs one to decide how
    // parameters and returns are placed. System V's register assignment is used
    // purely as a naming convention: FunctionEmitter maps those registers onto
    // wasm locals and seeds them from the real wasm parameters.
    const Abi abi = Abi::SystemV;
    const AbiInfo abiInfo = makeAbi(abi);

    const bool freestanding =
        options.target != nullptr && options.target->freestandingExecutable;

    // Collect the functions to emit before touching the module builder, so the
    // imports can be declared first (imports occupy the low function indices,
    // and the builder rejects a late import).
    struct Pending {
        const Sema::FunctionInfo* info;
        std::string symbol;
        std::string exportName;
    };
    std::vector<Pending> pending;
    // Synthetic FunctionInfos for generic instantiations. Held by pointer so the
    // addresses in `pending` stay valid as the vector grows.
    std::vector<std::unique_ptr<Sema::FunctionInfo>> synthetic;
    std::set<std::string> definedSymbols;
    std::set<std::string> exportNames;
    const Sema::FunctionInfo* mainInfo = nullptr;

    auto symbolOf = [](const Sema::FunctionInfo& info) {
        return info.mangledName.empty() ? info.name : info.mangledName;
    };

    // Export policy: the module's own functions, plus `memory` and `_start`.
    //
    // Insty's `export` keyword means "visible to importing modules", which is not
    // the same question -- every function in libs/wasi is `export`, and exporting
    // all of them would pin the entire standard library into every module. The
    // useful line is the one between the program being compiled and the libraries
    // it pulled in, and mangling draws it: a function defined in module M carries
    // the symbol mangleFunction(M, name).
    const std::string& rootModule = sema.moduleName;
    auto definedInRootModule = [&](const Sema::FunctionInfo& info) {
        if (info.name.empty()) return false;
        if (symbolOf(info) == Sema::mangleFunction(rootModule, info.name)) return true;
        // `[name(sym)]` pins a link symbol, which exists precisely to make the
        // function externally reachable, so honour it as an export request. The
        // mangling test above cannot see these, because the pinned symbol
        // deliberately does not follow the module's naming.
        if (info.decl) {
            for (const auto& attr : info.decl->attributes) {
                if (attr.name == "name" && !attr.value.empty()) return true;
            }
        }
        return false;
    };

    for (const auto& info : sema.functions) {
        if (!info.decl || !info.decl->hasBody || info.isExternal) continue;
        const std::string symbol = symbolOf(info);
        if (!definedSymbols.insert(symbol).second) continue;
        std::string name;
        if (definedInRootModule(info)) {
            name = info.name;
            if (!exportNames.insert(name).second) {
                name.clear();  // duplicated: emit it, do not export it
            }
        }
        if (info.name == "main") mainInfo = &info;
        pending.push_back(Pending{&info, symbol, name});
    }

    // Generic instantiations are checked by sema but never added to
    // sema.functions: they reuse the template body with concrete types under a
    // concrete symbol. Rebuild a FunctionInfo for each, as the x86 path does.
    for (const auto& inst : sema.genericInstantiations) {
        if (!inst.templateDecl || !inst.templateDecl->hasBody) continue;
        if (definedSymbols.count(inst.mangledName)) continue;
        auto info = std::make_unique<Sema::FunctionInfo>();
        info->name = inst.mangledName;
        info->mangledName = inst.mangledName;
        info->paramTypes = inst.paramTypes;
        info->paramNames = inst.paramNames;
        info->returnType = inst.returnType;
        // This instantiation's own copy of the body: its nodes carry the types
        // sema recorded for this instantiation.
        info->decl = inst.decl ? inst.decl.get() : inst.templateDecl;
        info->isGenericInstance = true;
        for (const auto& attr : inst.templateDecl->attributes) {
            if (attr.name == "unsafe" && (attr.value.empty() || attr.value == "on")) {
                info->isUnsafe = true;
                break;
            }
        }
        definedSymbols.insert(inst.mangledName);
        pending.push_back(Pending{info.get(), inst.mangledName, std::string()});
        synthetic.push_back(std::move(info));
    }

    if (pending.empty()) {
        errorOut = "wasm: module defines no functions with bodies";
        return false;
    }

    // --- pass 1: instruction selection -----------------------------------
    // Every function is selected before any is translated, because translation
    // needs each callee's index and each data symbol's address, and both depend
    // on the full set of functions and string constants.
    InstructionSelector isel(sema, abi, /*instantOsSyscalls=*/false,
                             options.boundsCheck, /*aesHash=*/false,
                             /*wasmTarget=*/true);
    std::vector<std::unique_ptr<MFunction>> selected;
    selected.reserve(pending.size());
    for (const auto& item : pending) {
        std::string err;
        auto fn = isel.select(*item.info, err);
        if (!fn) {
            errorOut = "wasm: failed to select '" + item.symbol + "': " + err;
            return false;
        }
        optimizeFunction(*fn, options.optLevel);
        selected.push_back(std::move(fn));
    }

    // Class methods, constructors, destructors and operators are not free
    // functions: their FunctionInfo carries no decl (so the loop above skipped
    // them) and the body hangs off the AST class declaration. Pair each method
    // with its resolved FunctionInfo by recomputing the mangled name exactly as
    // sema did, then select it with `this` as the implicit first parameter.
    if (options.program) {
        std::unordered_map<std::string, const Sema::FunctionInfo*> byMangled;
        for (const auto& fi : sema.functions) {
            const std::string key = symbolOf(fi);
            if (!key.empty()) byMangled[key] = &fi;
        }

        auto selectMethodInto = [&](const AST::Method& method, const std::string& mangled,
                                    bool weak) -> bool {
            if (definedSymbols.count(mangled)) return true;
            auto found = byMangled.find(mangled);
            if (found == byMangled.end()) return true;  // not analyzed; unused
            std::string err;
            auto fn = isel.selectMethod(method, *found->second, err);
            if (!fn) {
                errorOut = "wasm: failed to select method '" + mangled + "': " + err;
                return false;
            }
            if (weak) fn->linkage = MFunction::Linkage::LinkOnce;
            optimizeFunction(*fn, options.optLevel);
            definedSymbols.insert(mangled);
            pending.push_back(Pending{found->second, mangled, std::string()});
            selected.push_back(std::move(fn));
            return true;
        };

        for (const auto& node : options.program->body) {
            if (!node || node->nodeType() != AST::NodeType::ClassDeclaration) continue;
            const auto& cls = static_cast<const AST::ClassDeclaration&>(*node);
            if (!cls.genericParams.empty()) continue;  // templates are not emitted
            for (const auto& method : cls.methods) {
                std::vector<std::string> paramSpellings;
                for (const auto& p : method.parameters) paramSpellings.push_back(p.type);
                const std::string mangled = Sema::mangleClassMember(
                    cls.name, method.name, method.isConstructor, method.isOperator,
                    method.operatorSymbol, paramSpellings);
                if (!selectMethodInto(method, mangled, /*weak=*/false)) return false;
            }
        }

        // Generic class instantiations carry their own deep copy of each method
        // body (see GenericClassInstantiation::methods): sema checked that copy
        // under this instantiation's substitution, so its nodes are the ones
        // whose recorded types describe this instantiation. The type arguments
        // are still substituted into the parameter spellings to mangle the name.
        for (const auto& inst : sema.genericClassInstantiations) {
            if (!inst.templateDecl) continue;
            const auto& cls = *inst.templateDecl;
            std::unordered_map<std::string, std::string> subst;
            for (std::size_t i = 0;
                 i < cls.genericParams.size() && i < inst.typeArgs.size(); ++i) {
                subst[cls.genericParams[i]] = inst.typeArgs[i];
            }
            auto substituteAll = [&](const std::string& spelling) {
                std::string out = spelling;
                for (const auto& param : cls.genericParams) {
                    auto it = subst.find(param);
                    if (it != subst.end()) {
                        out = Sema::Checker::substituteSpelling(out, param, it->second);
                    }
                }
                return out;
            };
            for (const auto& methodOwner : inst.methods) {
                if (!methodOwner) continue;
                const AST::Method& method = *methodOwner;
                std::vector<std::string> paramSpellings;
                for (const auto& p : method.parameters) {
                    paramSpellings.push_back(substituteAll(p.type));
                }
                const std::string mangled = Sema::mangleClassMember(
                    inst.mangledName, method.name, method.isConstructor,
                    method.isOperator, method.operatorSymbol, paramSpellings);
                if (!selectMethodInto(method, mangled, /*weak=*/true)) return false;
            }
        }
    }

    // f16 conversion is an opcode rather than a call, so the requirement has to
    // be discovered by scanning. x86 has these in hardware (F16C); wasm has no
    // f16 at all and needs the software helpers from the core runtime.
    // Two sources: the explicit conversion opcodes, and any F* memory access at
    // width 2 -- on x86 those fold vcvtph2ps / vcvtps2ph into the load or store
    // itself, so on wasm each one needs the corresponding helper.
    std::set<std::string> extraRuntime;
    for (const auto& fn : selected) {
        for (const auto& block : fn->blocks()) {
            for (const auto& inst : block.insts) {
                switch (inst.op) {
                    case MOpcode::CvtF16ToF32:
                        extraRuntime.insert("__ins_f16_to_f32");
                        break;
                    case MOpcode::CvtF32ToF16:
                        extraRuntime.insert("__ins_f32_to_f16");
                        break;
                    case MOpcode::FLoad:
                    case MOpcode::FLoadInd:
                    case MOpcode::FConst:
                        if (inst.width == 2) extraRuntime.insert("__ins_f16_to_f32");
                        break;
                    case MOpcode::FStore:
                    case MOpcode::FStoreInd:
                        if (inst.width == 2) extraRuntime.insert("__ins_f32_to_f16");
                        break;
                    default:
                        break;
                }
            }
        }
    }

    // Runtime helpers the selection requested (__ins_memcpy, the __ins_fmt_*
    // string-interpolation formatters, the allocator hooks). Each is selected
    // against the runtime module's own SemaResult and appended, draining a
    // worklist because helpers call each other.
    if (!isel.requestedRuntime().empty() || !extraRuntime.empty()) {
        if (!options.runtimeModule) {
            errorOut = "wasm: this program needs runtime helpers but no runtime "
                       "module was provided to the backend";
            return false;
        }
        std::unordered_map<std::string, const Sema::FunctionInfo*> runtimeByName;
        for (const auto& info : options.runtimeModule->functions) {
            if (!info.decl || !info.decl->hasBody) continue;
            runtimeByName[symbolOf(info)] = &info;
        }
        InstructionSelector runtimeSel(*options.runtimeModule, abi,
                                       /*instantOsSyscalls=*/false, options.boundsCheck,
                                       /*aesHash=*/false, /*wasmTarget=*/true);
        std::set<std::string> done;
        std::vector<std::string> work(isel.requestedRuntime().begin(),
                                      isel.requestedRuntime().end());
        work.insert(work.end(), extraRuntime.begin(), extraRuntime.end());
        while (!work.empty()) {
            const std::string symbol = work.back();
            work.pop_back();
            if (!done.insert(symbol).second) continue;
            if (definedSymbols.count(symbol)) continue;

            auto found = runtimeByName.find(symbol);
            if (found == runtimeByName.end()) {
                errorOut = "wasm: required runtime helper '" + symbol +
                           "' not found in the runtime module";
                return false;
            }
            std::string err;
            auto fn = runtimeSel.select(*found->second, err);
            if (!fn) {
                errorOut = "wasm: failed to select runtime helper '" + symbol +
                           "': " + err;
                return false;
            }
            optimizeFunction(*fn, options.optLevel);
            definedSymbols.insert(symbol);
            pending.push_back(Pending{found->second, symbol, std::string()});
            selected.push_back(std::move(fn));

            for (const auto& dep : runtimeSel.requestedRuntime()) {
                if (!done.count(dep)) work.push_back(dep);
            }
        }
    }

    // --- dead code elimination ---------------------------------------------
    // There is no linker, so nothing else will ever remove a function that is
    // never called. Without this a program that only calls wasi.println carries
    // every function of every module it imported.
    //
    // Roots are the exported functions plus `main` (reached through the _start
    // shim rather than by a call). Everything else survives only if something
    // reachable calls it.
    {
        std::unordered_map<std::string, std::size_t> indexBySymbol;
        for (std::size_t i = 0; i < pending.size(); ++i) {
            indexBySymbol.emplace(pending[i].symbol, i);
        }

        std::vector<std::string> work;
        std::set<std::string> reachable;
        auto root = [&](const std::string& symbol) {
            if (!symbol.empty() && reachable.insert(symbol).second) {
                work.push_back(symbol);
            }
        };
        for (const auto& item : pending) {
            if (!item.exportName.empty()) root(item.symbol);
        }
        if (mainInfo) root(symbolOf(*mainInfo));

        while (!work.empty()) {
            const std::string symbol = work.back();
            work.pop_back();
            auto found = indexBySymbol.find(symbol);
            if (found == indexBySymbol.end()) continue;  // an import or a helper
            for (const auto& block : selected[found->second]->blocks()) {
                for (const auto& inst : block.insts) {
                    // f16 conversion is an opcode, not a call, so its dependency
                    // on the software helpers is invisible to a call-graph walk
                    // and has to be added explicitly.
                    switch (inst.op) {
                        case MOpcode::CvtF16ToF32:
                            root("__ins_f16_to_f32");
                            break;
                        case MOpcode::CvtF32ToF16:
                            root("__ins_f32_to_f16");
                            break;
                        case MOpcode::FLoad:
                        case MOpcode::FLoadInd:
                        case MOpcode::FConst:
                            if (inst.width == 2) root("__ins_f16_to_f32");
                            break;
                        case MOpcode::FStore:
                        case MOpcode::FStoreInd:
                        case MOpcode::FStoreOutgoing:
                            if (inst.width == 2) root("__ins_f32_to_f16");
                            break;
                        case MOpcode::Call:
                        case MOpcode::CallImport:
                            if (!inst.operands.empty() &&
                                inst.operands[0].kind == OperandKind::Symbol) {
                                root(inst.operands[0].symbol);
                            }
                            break;
                        case MOpcode::Lea:
                            // Taking a function's address makes it reachable
                            // through the function table even if nothing calls it
                            // directly.
                            if (inst.operands.size() >= 2 &&
                                inst.operands[1].kind == OperandKind::Symbol &&
                                indexBySymbol.count(inst.operands[1].symbol)) {
                                root(inst.operands[1].symbol);
                            }
                            break;
                        default:
                            break;
                    }
                }
            }
        }

        std::vector<Pending> keptPending;
        std::vector<std::unique_ptr<MFunction>> keptSelected;
        keptPending.reserve(pending.size());
        keptSelected.reserve(selected.size());
        for (std::size_t i = 0; i < pending.size(); ++i) {
            if (!reachable.count(pending[i].symbol)) continue;
            keptPending.push_back(pending[i]);
            keptSelected.push_back(std::move(selected[i]));
        }
        pending.swap(keptPending);
        selected.swap(keptSelected);

        definedSymbols.clear();
        for (const auto& item : pending) definedSymbols.insert(item.symbol);
    }

    if (pending.empty()) {
        errorOut = "wasm: no reachable functions to emit";
        return false;
    }

    // --- data layout ------------------------------------------------------
    // Only the surviving functions' string constants are placed, so eliminating a
    // function reclaims its data too.
    DataLayout layout;
    layoutData(sema, options.program, selected, layout);

    // --- resolve call targets --------------------------------------------
    // Scan every selected body for the symbols it calls. Anything not defined
    // here must come from an import, which has to be declared before the first
    // definition because imports occupy the low function indices.
    std::unordered_map<std::string, const Sema::FunctionInfo*> externBySymbol;
    for (const auto& info : sema.functions) {
        if (!info.isExternal) continue;
        externBySymbol.emplace(symbolOf(info), &info);
    }

    std::set<std::string> calledSymbols;
    for (const auto& fn : selected) {
        for (const auto& block : fn->blocks()) {
            for (const auto& inst : block.insts) {
                if (inst.op != MOpcode::Call && inst.op != MOpcode::CallImport) continue;
                if (inst.operands.empty() ||
                    inst.operands[0].kind != OperandKind::Symbol) {
                    continue;
                }
                calledSymbols.insert(inst.operands[0].symbol);
            }

        }
    }

    struct PendingImport {
        std::string module;
        std::string field;
        const Sema::FunctionInfo* info;
    };
    std::vector<PendingImport> imports;
    for (const auto& symbol : calledSymbols) {
        if (definedSymbols.count(symbol)) continue;
        // The allocator helpers are synthesized further down rather than
        // imported or selected from source.
        if (symbol == InstructionSelector::kWasmAllocSymbol ||
            symbol == InstructionSelector::kWasmFreeSymbol) {
            continue;
        }
        auto found = externBySymbol.find(symbol);
        if (found == externBySymbol.end()) {
            errorOut = "wasm: call to '" + symbol +
                       "' which is neither defined in this module nor declared extern";
            return false;
        }
        // `[dll("x")]` names the wasm module to import from; an extern with no
        // library is host-provided, so fall back to the conventional "env".
        std::string importModule = "env";
        for (const auto& attr : found->second->decl
                                    ? found->second->decl->attributes
                                    : std::vector<AST::Attribute>{}) {
            if (attr.name == "dll" || attr.name == "lib") {
                if (!attr.value.empty()) importModule = attr.value;
                break;
            }
        }
        imports.push_back(PendingImport{importModule, found->second->name.empty()
                                                          ? symbol
                                                          : found->second->name,
                                        found->second});
    }

    Module module;
    std::unordered_map<std::string, Callee> callees;

    // A WASI command needs proc_exit to turn main's return value into a process
    // exit status. Declared with the other imports, before any definition.
    bool wantStart = !freestanding && mainInfo != nullptr;
    std::uint32_t procExitIndex = 0;
    if (wantStart) {
        const std::uint32_t exitType = module.addType(FuncType{{ValType::I32}, {}});
        procExitIndex = module.importFunction(kWasiModule, "proc_exit", exitType);
    }

    for (const auto& imp : imports) {
        Callee callee;
        std::string why;
        if (!describeCallee(*imp.info, abiInfo, isel, callee, why)) {
            errorOut = "wasm: cannot import '" + imp.field + "': " + why;
            return false;
        }
        const std::uint32_t typeIndex = module.addType(callee.signature);
        callee.funcIndex = module.importFunction(imp.module, imp.field, typeIndex);
        callees[symbolOf(*imp.info)] = std::move(callee);
    }

    // The runtime allocator, if any lowering reached for it. Registered before
    // user code so its indices are known when those bodies are translated.
    const bool needsAllocator =
        calledSymbols.count(InstructionSelector::kWasmAllocSymbol) > 0 ||
        calledSymbols.count(InstructionSelector::kWasmFreeSymbol) > 0;
    // UMulHi is an opcode rather than a call, so scan the bodies for it.
    bool needsUMulHi = false;
    for (const auto& fn : selected) {
        for (const auto& block : fn->blocks()) {
            for (const auto& inst : block.insts) {
                if (inst.op == MOpcode::UMulHi) {
                    needsUMulHi = true;
                    break;
                }
            }
            if (needsUMulHi) break;
        }
        if (needsUMulHi) break;
    }
    std::uint32_t allocIndex = 0;
    std::uint32_t freeIndex = 0;
    std::uint32_t umulhiIndex = 0;
    const std::uint32_t firstDefinedIndex = module.functionCount();
    std::uint32_t definedCursor = firstDefinedIndex;
    if (needsAllocator) {
        Callee alloc;
        alloc.signature = FuncType{{ValType::I64, ValType::I64}, {ValType::I64}};
        alloc.paramRegs = {ArgSlot::integer(abiInfo.intArgRegs[0]),
                           ArgSlot::integer(abiInfo.intArgRegs[1])};
        alloc.returnsValue = true;
        alloc.returnType = ValType::I64;
        alloc.returnSigned = false;
        alloc.returnRegisterCount = 1;
        alloc.returnRegs = {ArgSlot::integer(abiInfo.intReturnReg)};
        alloc.funcIndex = definedCursor++;
        allocIndex = alloc.funcIndex;
        callees[InstructionSelector::kWasmAllocSymbol] = alloc;

        Callee freeFn;
        freeFn.signature =
            FuncType{{ValType::I64, ValType::I64, ValType::I64}, {}};
        freeFn.paramRegs = {ArgSlot::integer(abiInfo.intArgRegs[0]),
                            ArgSlot::integer(abiInfo.intArgRegs[1]),
                            ArgSlot::integer(abiInfo.intArgRegs[2])};
        freeFn.funcIndex = definedCursor++;
        freeIndex = freeFn.funcIndex;
        callees[InstructionSelector::kWasmFreeSymbol] = freeFn;
    }
    if (needsUMulHi) {
        Callee mulHi;
        mulHi.signature = FuncType{{ValType::I64, ValType::I64}, {ValType::I64}};
        mulHi.paramRegs = {ArgSlot::integer(abiInfo.intArgRegs[0]),
                           ArgSlot::integer(abiInfo.intArgRegs[1])};
        mulHi.returnsValue = true;
        mulHi.returnType = ValType::I64;
        mulHi.returnSigned = false;
        mulHi.returnRegisterCount = 1;
        mulHi.returnRegs = {ArgSlot::integer(abiInfo.intReturnReg)};
        mulHi.funcIndex = definedCursor++;
        umulhiIndex = mulHi.funcIndex;
        callees[InstructionSelector::kWasmUMulHiSymbol] = mulHi;
    }

    // Function indices for the definitions follow the imports. Assigning them all
    // before emitting any body is what lets a call reference a callee that has
    // not been translated yet -- including mutual recursion.
    for (std::size_t i = 0; i < pending.size(); ++i) {
        Callee callee;
        std::string why;
        if (!describeCallee(*pending[i].info, abiInfo, isel, callee, why)) {
            errorOut = "wasm: cannot emit '" + pending[i].symbol + "': " + why;
            return false;
        }
        callee.funcIndex = definedCursor + static_cast<std::uint32_t>(i);
        callees[pending[i].symbol] = std::move(callee);
    }

    // Linear memory, plus the shadow-stack pointer. Locals cover values whose
    // address is never taken; everything else -- structs, arrays, anything
    // reached through a pointer -- lives here.
    module.addMemory(Limits::atLeast(layout.memoryPages));
    module.exportMemory("memory", 0);
    const std::uint32_t stackPointerGlobal = module.addGlobal(
        ValType::I32, /*mutableGlobal=*/true,
        ConstExpr::i32(static_cast<std::int32_t>(layout.stackTop)));
    if (!layout.initialData.empty()) {
        module.addActiveData(kDataBase, layout.initialData);
    }

    // --- function table ----------------------------------------------------
    // Functions whose address is taken, in a stable order. Each gets a thunk with
    // the uniform all-i64 signature and a slot in table 0; `&fn` yields that slot.
    std::vector<std::string> addressTaken;
    {
        std::set<std::string> seen;
        for (const auto& fn : selected) {
            for (const auto& block : fn->blocks()) {
                for (const auto& inst : block.insts) {
                    if (inst.op != MOpcode::Lea || inst.operands.size() < 2) continue;
                    if (inst.operands[1].kind != OperandKind::Symbol) continue;
                    const std::string& symbol = inst.operands[1].symbol;
                    if (!definedSymbols.count(symbol)) continue;
                    if (seen.insert(symbol).second) addressTaken.push_back(symbol);
                }
            }
        }
    }

    // Every arity/result shape an indirect call site actually uses. Keyed as
    // argCount * 2 + hasResult, matching what the emitter looks up.
    std::unordered_map<std::uint32_t, std::uint32_t> indirectTypes;
    std::set<std::uint32_t> indirectShapes;
    for (const auto& fn : selected) {
        for (const auto& block : fn->blocks()) {
            for (const auto& inst : block.insts) {
                if (inst.op != MOpcode::CallIndirect || inst.operands.size() < 2) {
                    continue;
                }
                const std::uint32_t argCount =
                    static_cast<std::uint32_t>(inst.operands.size() - 2);
                indirectShapes.insert(argCount * 2 +
                                      (inst.operands[1].imm != 0 ? 1u : 0u));
            }
        }
    }
    // Thunks share these signatures, so a table entry is callable from any site
    // with a matching shape.
    for (const auto& sym : addressTaken) {
        const Callee& target = callees[sym];
        indirectShapes.insert(
            static_cast<std::uint32_t>(target.signature.params.size()) * 2 +
            (target.signature.results.empty() ? 0u : 1u));
    }
    for (std::uint32_t shape : indirectShapes) {
        FuncType sig;
        for (std::uint32_t i = 0; i < shape / 2; ++i) sig.params.push_back(ValType::I64);
        if (shape % 2 == 1) sig.results.push_back(ValType::I64);
        indirectTypes[shape] = module.addType(sig);
    }

    std::unordered_map<std::string, std::uint32_t> tableSlots;
    std::vector<std::uint32_t> thunkIndices;
    if (!addressTaken.empty()) {
        for (std::size_t i = 0; i < addressTaken.size(); ++i) {
            tableSlots[addressTaken[i]] = static_cast<std::uint32_t>(i);
        }
        module.addTable(ValType::FuncRef,
                        Limits::range(static_cast<std::uint32_t>(addressTaken.size()),
                                      static_cast<std::uint32_t>(addressTaken.size())));
    }

    // The heap cursor. Declared after the stack pointer so the global indices
    // match the order the synthesized bodies below expect.
    std::uint32_t heapCursorGlobal = 0;
    if (needsAllocator) {
        heapCursorGlobal = module.addGlobal(
            ValType::I32, /*mutableGlobal=*/true,
            ConstExpr::i32(static_cast<std::int32_t>(layout.heapBase)));
        const std::uint32_t allocType =
            module.addType(callees[InstructionSelector::kWasmAllocSymbol].signature);
        const std::uint32_t emittedAlloc =
            module.addFunction(allocType, buildWasmAlloc(heapCursorGlobal));
        const std::uint32_t freeType =
            module.addType(callees[InstructionSelector::kWasmFreeSymbol].signature);
        const std::uint32_t emittedFree =
            module.addFunction(freeType, buildWasmFree(heapCursorGlobal));
        if (emittedAlloc != allocIndex || emittedFree != freeIndex) {
            errorOut = "wasm: internal: allocator function indices do not match the "
                       "indices reserved for them";
            return false;
        }
    }
    if (needsUMulHi) {
        const std::uint32_t mulHiType =
            module.addType(callees[InstructionSelector::kWasmUMulHiSymbol].signature);
        const std::uint32_t emitted =
            module.addFunction(mulHiType, buildWasmUMulHi());
        if (emitted != umulhiIndex) {
            errorOut = "wasm: internal: the high-multiply helper's index does not "
                       "match the index reserved for it";
            return false;
        }
    }

    // --- pass 2: translation ----------------------------------------------
    for (std::size_t i = 0; i < pending.size(); ++i) {
        const auto& item = pending[i];
        FuncType signature;
        FunctionBody body;
        FunctionEmitter emitter(*selected[i], *item.info, abiInfo, layout,
                                stackPointerGlobal, callees, callees[item.symbol]);
        emitter.setIndirectTables(&tableSlots, &indirectTypes);
        if (!emitter.run(signature, body, errorOut)) {
            return false;
        }

        const std::uint32_t typeIndex = module.addType(signature);
        const std::uint32_t funcIndex = module.addFunction(typeIndex, std::move(body));
        if (funcIndex != callees[item.symbol].funcIndex) {
            errorOut = "wasm: internal: function index for '" + item.symbol +
                       "' does not match the index reserved for it";
            return false;
        }
        if (!item.exportName.empty()) {
            module.exportFunction(item.exportName, funcIndex);
        }
    }

    // Table thunks, then the element segment that installs them. Emitted after
    // the user functions so every call target already has an index.
    if (!addressTaken.empty()) {
        for (const auto& sym : addressTaken) {
            const Callee& target = callees[sym];
            if (target.signature.results.size() > 1) {
                errorOut = "wasm: cannot take the address of '" + sym +
                           "', which returns a multi-value aggregate; the function "
                           "table requires a single uniform signature";
                return false;
            }
            const std::uint32_t shape =
                static_cast<std::uint32_t>(target.signature.params.size()) * 2 +
                (target.signature.results.empty() ? 0u : 1u);
            thunkIndices.push_back(module.addFunction(
                indirectTypes[shape], buildIndirectThunk(target, target.funcIndex)));
        }
        module.addActiveElements(0, thunkIndices);
    }

    if (wantStart) {
        const bool mainReturnsI32 =
            mainInfo->returnType != nullptr && !mainInfo->returnType->isVoid();
        const std::uint32_t voidType = module.addType(FuncType{{}, {}});
        FunctionBody shim = buildStartShim(callees[symbolOf(*mainInfo)].funcIndex,
                                          mainReturnsI32, procExitIndex);
        const std::uint32_t shimIndex = module.addFunction(voidType, std::move(shim));
        module.exportFunction("_start", shimIndex);
    }

    if (!module.ok()) {
        errorOut = module.error();
        return false;
    }
    return WasmWriter::write(module, outPath, errorOut);
}

}  // namespace Backend::Wasm


