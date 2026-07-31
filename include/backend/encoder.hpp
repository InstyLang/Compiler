#pragma once

// Thin x86-64 instruction encoder built on the vendored Fadec "encode2" API
// (fe64_* functions). It appends encoded instructions to a MachineCode text
// section and exposes helpers for the common cases the backend needs, plus a
// fully general escape hatch for emitting any Fadec instruction.

#include <cstdint>
#include <string>

#include <backend/machine_code.hpp>

namespace Backend {

class Encoder {
public:
    explicit Encoder(MachineCode& code) : code_(code) {}

    // Current write offset within the active code section (== next instruction's
    // addr within that section). The active section is `MachineCode::currentCode`
    // (the primary .text unless redirected to a named `[section]`).
    std::uint64_t offset() const { return code_.currentCode().bytes.size(); }

    // --- General escape hatch -------------------------------------------------
    // Encodes one instruction via a Fadec fe64_* function. Usage:
    //   enc.emit([](uint8_t* p){ return fe64_RET(p, 0); });
    // The callable receives a pointer to a scratch buffer and must return the
    // number of bytes written (Fadec's convention). The bytes are appended to
    // the active code section. Returns the offset at which the instruction
    // started.
    template <typename Fn>
    std::uint64_t emit(Fn&& encodeOne) {
        std::uint8_t scratch[16];  // max x86-64 instruction length is 15 bytes
        auto& sec = code_.currentCode();
        const std::uint64_t start = sec.bytes.size();
        unsigned len = static_cast<unsigned>(encodeOne(scratch));
        sec.bytes.insert(sec.bytes.end(), scratch, scratch + len);
        return start;
    }

    // --- Convenience emitters for the bring-up path --------------------------
    // mov eax/rax-style immediate into a 64-bit GP register, and ret.
    std::uint64_t movImm32ToEax(std::int32_t imm);
    std::uint64_t ret();

    // Emits a `call rel32` to an (external or local) symbol by name, recording
    // a Rel32 relocation so the linker fixes up the displacement. Returns the
    // instruction's start offset.
    std::uint64_t callSymbol(const std::string& name);

    // --- Intra-section branches (resolved by the caller via backpatching) -----
    // These emit a near branch with a 32-bit displacement placeholder and
    // return the byte offset of that 4-byte displacement field within the text
    // section. The caller patches it later with patchRel32() once the target
    // offset is known.
    enum class Branch : std::uint8_t { Jmp, Je, Jne, Jl, Jle, Jg, Jge,
                                       Jb, Jbe, Ja, Jae };
    std::uint64_t branchPlaceholder(Branch kind);

    // Patches a previously-emitted rel32 displacement field at `dispOffset` so
    // that it encodes a PC-relative jump to `targetOffset` (both are offsets
    // within the text section). rel32 = target - (dispOffset + 4).
    void patchRel32(std::uint64_t dispOffset, std::uint64_t targetOffset);

    MachineCode& code() { return code_; }

private:
    MachineCode& code_;
};

}  // namespace Backend
