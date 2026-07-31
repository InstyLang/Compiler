#include <backend/encoder.hpp>

#include <fadec-enc2.h>

namespace Backend {

std::uint64_t Encoder::movImm32ToEax(std::int32_t imm) {
    return emit([imm](std::uint8_t* p) {
        // mov eax, imm32  (zero-extends into rax). FE_AX is the 'A' register;
        // the 32-bit form is selected by MOV32ri.
        return fe64_MOV32ri(p, 0, FE_AX, imm);
    });
}

std::uint64_t Encoder::ret() {
    return emit([](std::uint8_t* p) { return fe64_RET(p, 0); });
}

std::uint64_t Encoder::callSymbol(const std::string& name) {
    // Emit `call rel32` (E8 + 4-byte displacement) directly and record a Rel32
    // relocation over the displacement field. We do not use fe64_CALL here
    // because Fadec resolves the displacement from a concrete target address,
    // whereas at object-emit time the target address is unknown and must be
    // patched by the linker.
    auto& sec = code_.currentCode();
    const std::uint64_t start = sec.bytes.size();
    sec.bytes.push_back(0xE8);
    const std::uint64_t dispOffset = sec.bytes.size();
    for (int i = 0; i < 4; ++i) {
        sec.bytes.push_back(0x00);  // placeholder displacement
    }

    const std::uint32_t sym = code_.referenceExternal(name);
    // COFF REL32 is relative to the end of the 4-byte field, which matches the
    // x86 call's "next instruction" base, so addend is 0.
    if (code_.currentCodeName().empty()) {
        code_.addRelocation(SectionKind::Text, dispOffset, sym, RelocKind::Rel32, 0);
    } else {
        code_.addRelocationInSection(code_.currentCodeName(), dispOffset, sym,
                                     RelocKind::Rel32, 0);
    }
    return start;
}

std::uint64_t Encoder::branchPlaceholder(Branch kind) {
    // Near jumps with 32-bit displacement:
    //   JMP rel32        = E9 disp32
    //   Jcc rel32 (0F 8x): JE=84 JNE=85 JL=8C JGE=8D JLE=8E JG=8F
    //   unsigned:          JB=82 JAE=83 JBE=86 JA=87
    auto& sec = code_.currentCode();
    switch (kind) {
        case Branch::Jmp:
            sec.bytes.push_back(0xE9);
            break;
        default: {
            sec.bytes.push_back(0x0F);
            std::uint8_t cc = 0;
            switch (kind) {
                case Branch::Je: cc = 0x84; break;
                case Branch::Jne: cc = 0x85; break;
                case Branch::Jl: cc = 0x8C; break;
                case Branch::Jge: cc = 0x8D; break;
                case Branch::Jle: cc = 0x8E; break;
                case Branch::Jg: cc = 0x8F; break;
                case Branch::Jb: cc = 0x82; break;
                case Branch::Jae: cc = 0x83; break;
                case Branch::Jbe: cc = 0x86; break;
                case Branch::Ja: cc = 0x87; break;
                case Branch::Jmp: break;  // unreachable
            }
            sec.bytes.push_back(cc);
            break;
        }
    }
    const std::uint64_t dispOffset = sec.bytes.size();
    for (int i = 0; i < 4; ++i) sec.bytes.push_back(0x00);
    return dispOffset;
}

void Encoder::patchRel32(std::uint64_t dispOffset, std::uint64_t targetOffset) {
    const std::int64_t rel =
        static_cast<std::int64_t>(targetOffset) - static_cast<std::int64_t>(dispOffset + 4);
    const std::int32_t rel32 = static_cast<std::int32_t>(rel);
    auto& sec = code_.currentCode();
    for (int i = 0; i < 4; ++i) {
        sec.bytes[dispOffset + i] =
            static_cast<std::uint8_t>((rel32 >> (8 * i)) & 0xFF);
    }
}

}  // namespace Backend
