#include <backend/abi.hpp>
#include <backend/reg.hpp>

#include <string>

namespace Backend {

const char* physRegName(PhysReg r) {
    switch (r) {
        case PhysReg::RAX: return "rax";
        case PhysReg::RCX: return "rcx";
        case PhysReg::RDX: return "rdx";
        case PhysReg::RBX: return "rbx";
        case PhysReg::RSP: return "rsp";
        case PhysReg::RBP: return "rbp";
        case PhysReg::RSI: return "rsi";
        case PhysReg::RDI: return "rdi";
        case PhysReg::R8: return "r8";
        case PhysReg::R9: return "r9";
        case PhysReg::R10: return "r10";
        case PhysReg::R11: return "r11";
        case PhysReg::R12: return "r12";
        case PhysReg::R13: return "r13";
        case PhysReg::R14: return "r14";
        case PhysReg::R15: return "r15";
        case PhysReg::None: return "none";
    }
    return "?";
}

AbiInfo makeAbi(Abi abi) {
    AbiInfo info;
    info.abi = abi;
    info.intReturnReg = PhysReg::RAX;
    info.xmmReturnReg = XmmReg::XMM0;
    info.stackAlignment = 16;

    if (abi == Abi::SystemV) {
        info.intArgRegs = {PhysReg::RDI, PhysReg::RSI, PhysReg::RDX,
                           PhysReg::RCX, PhysReg::R8,  PhysReg::R9};
        // System V callee-saved: RBX, RBP, R12-R15, RSP. RBP/RSP are reserved
        // by the frame, so they are not in the allocatable list below.
        info.calleeSaved = {PhysReg::RBX, PhysReg::R12, PhysReg::R13,
                            PhysReg::R14, PhysReg::R15};
        // Caller-saved (scratch): RAX, RCX, RDX, RSI, RDI, R8-R11.
        info.callerSaved = {PhysReg::RAX, PhysReg::RCX, PhysReg::RDX,
                           PhysReg::RSI, PhysReg::RDI, PhysReg::R8,
                           PhysReg::R9,  PhysReg::R10, PhysReg::R11};
        info.shadowSpace = 0;
        // FP args in XMM0..XMM7 (counted independently of integer args). All XMM
        // registers are caller-saved on System V.
        info.xmmArgRegs = {XmmReg::XMM0, XmmReg::XMM1, XmmReg::XMM2, XmmReg::XMM3,
                           XmmReg::XMM4, XmmReg::XMM5, XmmReg::XMM6, XmmReg::XMM7};
        info.sharedArgRegIndex = false;
        info.xmmCalleeSaved = {};
        info.xmmCallerSaved = {XmmReg::XMM0,  XmmReg::XMM1,  XmmReg::XMM2,
                               XmmReg::XMM3,  XmmReg::XMM4,  XmmReg::XMM5,
                               XmmReg::XMM6,  XmmReg::XMM7,  XmmReg::XMM8,
                               XmmReg::XMM9,  XmmReg::XMM10, XmmReg::XMM11,
                               XmmReg::XMM12, XmmReg::XMM13, XmmReg::XMM14,
                               XmmReg::XMM15};
    } else {  // Win64
        info.intArgRegs = {PhysReg::RCX, PhysReg::RDX, PhysReg::R8, PhysReg::R9};
        // Win64 callee-saved: RBX, RBP, RDI, RSI, RSP, R12-R15.
        info.calleeSaved = {PhysReg::RBX, PhysReg::RDI, PhysReg::RSI,
                            PhysReg::R12, PhysReg::R13, PhysReg::R14,
                            PhysReg::R15};
        // Caller-saved (scratch): RAX, RCX, RDX, R8-R11.
        info.callerSaved = {PhysReg::RAX, PhysReg::RCX, PhysReg::RDX,
                           PhysReg::R8,  PhysReg::R9,  PhysReg::R10,
                           PhysReg::R11};
        info.shadowSpace = 32;
        // FP args in XMM0..XMM3, sharing the positional index with the integer
        // arg registers (the k-th argument uses RCX/RDX/R8/R9 or XMM0..XMM3).
        info.xmmArgRegs = {XmmReg::XMM0, XmmReg::XMM1, XmmReg::XMM2, XmmReg::XMM3};
        info.sharedArgRegIndex = true;
        // Win64: XMM0..XMM5 volatile (caller-saved), XMM6..XMM15 callee-saved.
        info.xmmCalleeSaved = {XmmReg::XMM6,  XmmReg::XMM7,  XmmReg::XMM8,
                               XmmReg::XMM9,  XmmReg::XMM10, XmmReg::XMM11,
                               XmmReg::XMM12, XmmReg::XMM13, XmmReg::XMM14,
                               XmmReg::XMM15};
        info.xmmCallerSaved = {XmmReg::XMM0, XmmReg::XMM1, XmmReg::XMM2,
                               XmmReg::XMM3, XmmReg::XMM4, XmmReg::XMM5};
    }

    // Allocatable pool: caller-saved first (cheap, no save/restore), then
    // callee-saved (require preservation). The allocator prefers earlier
    // entries. RSP and RBP are deliberately excluded (frame-reserved).
    info.allocatable = info.callerSaved;
    for (PhysReg r : info.calleeSaved) {
        info.allocatable.push_back(r);
    }
    // XMM allocatable pool: caller-saved (volatile) registers EXCLUDING XMM4/XMM5
    // (lowering reserves them as fixed scratch for reloading spilled float
    // operands), followed by the callee-saved XMM registers. Callee-saved XMM
    // regs are saved/restored in the prologue/epilogue (movsd to reserved frame
    // slots) when used, so they are safe to allocate. Caller-saved come first so
    // leaf/simple functions avoid touching callee-saved (and the save/restore).
    info.xmmAllocatable.clear();
    for (XmmReg r : info.xmmCallerSaved) {
        if (r == XmmReg::XMM4 || r == XmmReg::XMM5) continue;
        info.xmmAllocatable.push_back(r);
    }
    for (XmmReg r : info.xmmCalleeSaved) {
        info.xmmAllocatable.push_back(r);
    }
    return info;
}

Abi abiForTriple(const std::string& triple) {
    if (triple.find("windows") != std::string::npos ||
        triple.find("win32") != std::string::npos ||
        triple.find("msvc") != std::string::npos ||
        triple.find("mingw") != std::string::npos) {
        return Abi::Win64;
    }
    return Abi::SystemV;
}

}  // namespace Backend
