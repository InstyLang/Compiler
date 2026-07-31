#pragma once

// x86-64 physical general-purpose registers, in the canonical hardware
// encoding order (rax=0 .. r15=15). Shared by the ABI descriptor, the Machine
// IR, the register allocator, and lowering. Float/vector registers (xmm) are
// intentionally omitted from this first scaffold.

#include <cstdint>

namespace Backend {

enum class PhysReg : std::uint8_t {
    RAX = 0,
    RCX = 1,
    RDX = 2,
    RBX = 3,
    RSP = 4,
    RBP = 5,
    RSI = 6,
    RDI = 7,
    R8 = 8,
    R9 = 9,
    R10 = 10,
    R11 = 11,
    R12 = 12,
    R13 = 13,
    R14 = 14,
    R15 = 15,

    None = 0xFF
};

constexpr unsigned kNumGPRegs = 16;

inline std::uint8_t regIndex(PhysReg r) { return static_cast<std::uint8_t>(r); }

const char* physRegName(PhysReg r);

// x86-64 SSE/AVX vector registers (xmm0..xmm15). Floats and doubles live here.
// Kept in a separate enum/namespace from the GP file so the two register files
// can be allocated independently (a vreg belongs to exactly one class).
enum class XmmReg : std::uint8_t {
    XMM0 = 0,  XMM1,  XMM2,  XMM3,  XMM4,  XMM5,  XMM6,  XMM7,
    XMM8,      XMM9,  XMM10, XMM11, XMM12, XMM13, XMM14, XMM15,
    None = 0xFF
};

constexpr unsigned kNumXmmRegs = 16;

inline std::uint8_t xmmIndex(XmmReg r) { return static_cast<std::uint8_t>(r); }

// The register class a virtual register belongs to. Selection tags each vreg so
// the allocator draws from the matching physical pool and lowering picks the
// right (GP vs SSE) instruction form.
enum class RegClass : std::uint8_t { GPR, XMM };

}  // namespace Backend
