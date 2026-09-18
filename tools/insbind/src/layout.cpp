#include "layout.h"

#include <algorithm>

namespace insbind {
namespace {

std::uint32_t alignUp(std::uint32_t v, std::uint32_t a) {
    return (v + a - 1) / a * a;
}

} // namespace

void LayoutEngine::enumUnderlying(const CEnumDef& def, unsigned& bits,
                                  bool& isSigned) {
    std::int64_t lo = 0, hi = 0;
    for (const auto& [name, value] : def.values) {
        lo = std::min(lo, value);
        hi = std::max(hi, value);
    }
    if (lo >= -2147483648LL && hi <= 2147483647LL) {
        bits = 32;
        isSigned = true;
    } else if (lo >= 0 && hi <= 4294967295LL) {
        bits = 32;
        isSigned = false;
    } else if (hi <= 9223372036854775807LL) {
        bits = 64;
        isSigned = true;
    } else {
        bits = 64;
        isSigned = false;
    }
}

std::uint32_t LayoutEngine::typeSize(std::uint32_t idx) {
    if (idx == kNoType || idx >= model.types.size()) return 0;
    const CType& t = model.types[idx];
    switch (t.kind) {
        case CTypeKind::Void: return 0;
        case CTypeKind::Bool: return 1;
        case CTypeKind::Int: return t.bits / 8;
        case CTypeKind::Float: return t.bits == 80 ? 16 : t.bits / 8;
        case CTypeKind::Pointer: return 8;
        case CTypeKind::Array:
            return t.count == kNoCount
                       ? 0
                       : static_cast<std::uint32_t>(t.count) *
                             typeSize(t.target);
        case CTypeKind::Function: return 8; // decayed pointer in practice
        case CTypeKind::Record:
            return record(t.target).size;
        case CTypeKind::Enum: {
            unsigned bits;
            bool s;
            enumUnderlying(model.enums[t.target], bits, s);
            return bits / 8;
        }
        case CTypeKind::TypedefRef:
            return t.target != kNoType ? typeSize(t.target) : 0;
    }
    return 0;
}

std::uint32_t LayoutEngine::typeAlign(std::uint32_t idx) {
    if (idx == kNoType || idx >= model.types.size()) return 1;
    const CType& t = model.types[idx];
    switch (t.kind) {
        case CTypeKind::Bool: return 1;
        case CTypeKind::Int: return t.bits / 8;
        case CTypeKind::Float: return t.bits == 80 ? 16 : t.bits / 8;
        case CTypeKind::Pointer: return 8;
        case CTypeKind::Function: return 8;
        case CTypeKind::Array: return typeAlign(t.target);
        case CTypeKind::Record: return record(t.target).align;
        case CTypeKind::Enum: {
            unsigned bits;
            bool s;
            enumUnderlying(model.enums[t.target], bits, s);
            return bits / 8;
        }
        case CTypeKind::TypedefRef:
            return t.target != kNoType ? typeAlign(t.target) : 1;
        case CTypeKind::Void: return 1;
    }
    return 1;
}

RecordLayout LayoutEngine::record(std::uint32_t recordIdx) {
    RecordLayout out;
    if (recordIdx == kNoType || recordIdx >= model.records.size()) return out;
    const CRecordDef& def = model.records[recordIdx];
    if (def.incomplete) return out;

    out.fields.resize(def.fields.size());
    std::uint32_t offset = 0;

    // Bitfield run state: the current storage unit being filled.
    std::uint32_t unitBitsTotal = 0; // capacity of the open unit
    std::uint32_t unitBitsUsed = 0;  // bits assigned in the open unit
    std::uint32_t unitOffset = 0;    // byte offset of the open unit
    std::uint32_t unitTypeIdx = kNoType; // declared type that opened the unit
    bool unitOpen = false;

    auto closeUnit = [&] {
        unitOpen = false;
        unitBitsTotal = unitBitsUsed = 0;
        unitTypeIdx = kNoType;
    };

    for (std::size_t fi = 0; fi < def.fields.size(); ++fi) {
        const CField& f = def.fields[fi];
        FieldLayout& fl = out.fields[fi];

        if (f.bitfield) {
            const std::uint32_t ftSize = typeSize(f.type);
            const std::uint32_t ftAlign = typeAlign(f.type);
            const std::uint32_t ftBits = ftSize * 8;

            if (f.bits == 0) {
                // Zero width: close the run and align to the declared type.
                closeUnit();
                offset = alignUp(offset, ftAlign);
                fl.offset = fl.unitOffset = offset;
                continue;
            }

            bool needNewUnit = !unitOpen || f.bits > unitBitsTotal - unitBitsUsed;
            if (opts.msvcBitfields && unitOpen) {
                // MSVC: a new DECLARED type opens a new unit. Arena indices
                // differ per field even for identical types, so compare the
                // type content.
                const CType& ut = model.types[unitTypeIdx];
                const CType& ft = model.types[f.type];
                if (ut.kind != ft.kind || ut.bits != ft.bits ||
                    ut.isSigned != ft.isSigned)
                    needNewUnit = true;
            }

            if (needNewUnit) {
                // A bitfield storage unit is atomic: it occupies its full
                // declared size in the layout no matter how few bits the run
                // uses, so the NEXT field starts after the whole unit.
                offset = alignUp(offset, ftAlign);
                unitOpen = true;
                unitBitsTotal = ftBits;
                unitBitsUsed = 0;
                unitOffset = offset;
                unitTypeIdx = f.type;
                offset += ftSize;
            }
            fl.unitOffset = unitOffset;
            fl.unitSize = ftSize;
            fl.bitOffset = unitBitsUsed;
            fl.offset = unitOffset;
            unitBitsUsed += f.bits;
            continue;
        }

        // Plain field: close any open bitfield run. Field-level _Alignas
        // raises this field's alignment (C11 6.7.5).
        if (unitOpen) closeUnit();
        const std::uint32_t fAlign =
            std::max(typeAlign(f.type), f.alignAs ? f.alignAs : 1);
        offset = alignUp(offset, fAlign);
        fl.offset = offset;
        if (def.isUnion) {
            fl.offset = 0;
            out.size = std::max(out.size, typeSize(f.type));
            out.align = std::max(out.align, fAlign);
        } else {
            offset += typeSize(f.type);
        }
    }

    if (def.isUnion) {
        out.align = std::max(out.align, def.alignAs ? def.alignAs : 1);
        out.size = alignUp(out.size, out.align);
    } else {
        out.align = 1;
        // Struct alignment: max field alignment (including field-level
        // _Alignas, bitfield unit types and arrays' element alignment).
        for (const CField& f : def.fields)
            out.align = std::max(
                out.align,
                std::max(typeAlign(f.type), f.alignAs ? f.alignAs : 1));
        if (def.alignAs) out.align = std::max(out.align, def.alignAs);
        out.size = alignUp(offset, out.align);
    }
    return out;
}

} // namespace insbind
