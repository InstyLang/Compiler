// insbind: C ABI layout engine.
//
// Computes the size/alignment of types and the offset of every record field
// exactly as a C compiler for the target would: natural alignment, _Alignas
// overrides, and bitfield storage-unit grouping (MSVC rule for Windows
// targets, SysV/Itanium rule otherwise -- the two ABIs agree on plain fields
// and differ almost only in bitfields).
//
// The emitter needs this for union blob sizes, bitfield storage units and
// masks, and the --abi-check self-verification program.
#pragma once

#include <cstdint>
#include <vector>

#include "model.h"

namespace insbind {

struct LayoutOptions {
    bool msvcBitfields = true; // true for Windows targets, false for SysV
};

struct FieldLayout {
    std::uint32_t offset = 0;    // byte offset of the field (or of its unit)
    std::uint32_t bitOffset = 0; // bit offset within the unit (bitfields only)
    std::uint32_t unitOffset = 0;// byte offset of the storage unit (bitfields)
    std::uint32_t unitSize = 0;  // storage unit size in bytes (bitfields)
};

struct RecordLayout {
    std::uint32_t size = 0;
    std::uint32_t align = 1;
    std::vector<FieldLayout> fields; // parallel to CRecordDef::fields
};

struct LayoutEngine {
    const ParseResult& model;
    LayoutOptions opts;

    std::uint32_t typeSize(std::uint32_t typeIdx);
    std::uint32_t typeAlign(std::uint32_t typeIdx);
    RecordLayout record(std::uint32_t recordIdx);

    // Enum underlying integer type (also used by the emitter): smallest of
    // i32/u32/i64/u64 that holds every value, matching what C compilers pick.
    static void enumUnderlying(const CEnumDef& def, unsigned& bits,
                               bool& isSigned);
};

} // namespace insbind
