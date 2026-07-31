#pragma once

// x86-64 ABI descriptor. Models the two calling conventions the backend
// targets: System V (Linux/macOS) and Microsoft x64 (Win64). The register
// allocator and the call/prologue lowering consult an AbiInfo to decide where
// arguments and return values live, which registers must be preserved across
// calls, and how the stack frame is shaped.

#include <string>
#include <vector>

#include <backend/reg.hpp>

namespace Backend {

enum class Abi {
    SystemV,  // Linux / macOS / BSD: integer args in RDI,RSI,RDX,RCX,R8,R9
    Win64     // Windows: integer args in RCX,RDX,R8,R9 + 32-byte shadow space
};

struct AbiInfo {
    Abi abi = Abi::SystemV;

    // Integer/pointer argument registers, in order. Args beyond this list are
    // passed on the stack.
    std::vector<PhysReg> intArgRegs;

    // Where an integer/pointer return value is placed.
    PhysReg intReturnReg = PhysReg::RAX;

    // Floating-point (XMM) argument registers, in order, and the FP return reg.
    // On Win64 the argument *position* is shared with intArgRegs (the k-th arg
    // uses intArgRegs[k] or xmmArgRegs[k] by type); on System V the two files are
    // counted independently.
    std::vector<XmmReg> xmmArgRegs;
    XmmReg xmmReturnReg = XmmReg::XMM0;

    // Whether the FP argument register index is shared with the integer one
    // (Win64) or counted separately (System V).
    bool sharedArgRegIndex = false;

    // XMM registers the callee must preserve (Win64: xmm6..xmm15; SysV: none).
    std::vector<XmmReg> xmmCalleeSaved;
    // XMM registers a caller must assume clobbered; the allocator's XMM pool.
    std::vector<XmmReg> xmmCallerSaved;

    // Registers the callee must preserve (save/restore if it uses them).
    std::vector<PhysReg> calleeSaved;

    // Registers a caller must assume are clobbered across a call. These are the
    // allocatable scratch registers for the allocator's free pool.
    std::vector<PhysReg> callerSaved;

    // Bytes of "shadow space" the caller reserves on the stack for the callee
    // (Win64 = 32; System V = 0).
    unsigned shadowSpace = 0;

    // Required stack-pointer alignment at the point of a `call` (16 for both).
    unsigned stackAlignment = 16;

    // Convenience: the registers the allocator may freely assign to virtual
    // registers (callee-saved + caller-saved, excluding RSP/RBP which the frame
    // reserves). Computed by makeAbi().
    std::vector<PhysReg> allocatable;

    // XMM registers the allocator may assign (caller-saved + callee-saved).
    std::vector<XmmReg> xmmAllocatable;
};

// Builds the descriptor for the given convention.
AbiInfo makeAbi(Abi abi);

// Selects the convention implied by an LLVM-style target triple (e.g.
// "x86_64-pc-windows-msvc" -> Win64, otherwise System V).
Abi abiForTriple(const std::string& triple);

}  // namespace Backend
