#include <backend/coff_writer.hpp>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

namespace Backend {

namespace {

// --- COFF constants (winnt.h names) -------------------------------------------
constexpr std::uint16_t IMAGE_FILE_MACHINE_AMD64 = 0x8664;

// Section characteristics
constexpr std::uint32_t IMAGE_SCN_CNT_CODE = 0x00000020;
constexpr std::uint32_t IMAGE_SCN_CNT_INITIALIZED_DATA = 0x00000040;
constexpr std::uint32_t IMAGE_SCN_CNT_UNINITIALIZED_DATA = 0x00000080;
constexpr std::uint32_t IMAGE_SCN_ALIGN_16BYTES = 0x00500000;
constexpr std::uint32_t IMAGE_SCN_MEM_EXECUTE = 0x20000000;
constexpr std::uint32_t IMAGE_SCN_MEM_READ = 0x40000000;
constexpr std::uint32_t IMAGE_SCN_MEM_WRITE = 0x80000000;

// AMD64 relocation types
constexpr std::uint16_t IMAGE_REL_AMD64_ADDR64 = 0x0001;
constexpr std::uint16_t IMAGE_REL_AMD64_ADDR32 = 0x0002;
constexpr std::uint16_t IMAGE_REL_AMD64_ADDR32NB = 0x0003;
constexpr std::uint16_t IMAGE_REL_AMD64_REL32 = 0x0004;
constexpr std::uint16_t IMAGE_REL_AMD64_REL32_1 = 0x0005;

// Symbol storage classes / section numbers
constexpr std::uint8_t IMAGE_SYM_CLASS_EXTERNAL = 2;
constexpr std::uint8_t IMAGE_SYM_CLASS_STATIC = 3;
constexpr std::uint8_t IMAGE_SYM_CLASS_WEAK_EXTERNAL = 105;  // 0x69
constexpr std::int16_t IMAGE_SYM_UNDEFINED = 0;

// Weak-external aux record search type: ALIAS resolves to the default (linked)
// symbol when no strong external definition is found at link time.
constexpr std::uint32_t IMAGE_WEAK_EXTERN_SEARCH_ALIAS = 3;

// COMDAT: the linker keeps one section out of all those sharing the COMDAT
// symbol's name and discards the rest. This -- not a weak external -- is how COFF
// folds a definition emitted by several objects. A weak external means "use this
// only if nothing else defines the name", which is a different thing: a link
// still fails when two objects each provide one.
constexpr std::uint32_t IMAGE_SCN_LNK_COMDAT = 0x00001000;
constexpr std::uint8_t IMAGE_COMDAT_SELECT_ANY = 2;

// Symbol type: function complex type in the high byte (DTYPE_FUNCTION << 4).
constexpr std::uint16_t IMAGE_SYM_TYPE_FUNCTION = 0x20;

// --- little-endian append helpers --------------------------------------------
void put16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(x & 0xFF);
    v.push_back((x >> 8) & 0xFF);
}
void put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back((x >> (8 * i)) & 0xFF);
}

struct OutSection {
    SectionKind kind;
    std::string name;               // section name as written to the header
    std::uint32_t characteristics;
    std::string customName;         // non-empty => a named code section (extraText)
    const Section* sec = nullptr;   // the bytes/size to emit
};

std::uint16_t mapReloc(RelocKind kind) {
    switch (kind) {
        case RelocKind::Abs64: return IMAGE_REL_AMD64_ADDR64;
        case RelocKind::Rel32: return IMAGE_REL_AMD64_REL32;
        case RelocKind::RipData32: return IMAGE_REL_AMD64_REL32;  // lea reg,[rip+x]
        case RelocKind::Rel32_1: return IMAGE_REL_AMD64_REL32_1;
        case RelocKind::Addr32: return IMAGE_REL_AMD64_ADDR32;
        case RelocKind::Addr32NB: return IMAGE_REL_AMD64_ADDR32NB;
        case RelocKind::ImportCall32: return IMAGE_REL_AMD64_REL32;  // PE-only; n/a for .obj
    }
    return IMAGE_REL_AMD64_REL32;
}

}  // namespace

bool CoffWriter::write(const MachineCode& code, const std::string& path,
                       std::string& errorOut) {
    constexpr std::uint32_t IMAGE_SCN_ALIGN_1BYTES = 0x00100000;

    // Build the ordered list of output sections. The four standard sections come
    // first (skipping empty ones, but always keeping .text), followed by any
    // named code sections produced by `[section("name")]`.
    std::vector<OutSection> sections;
    auto pushStd = [&](SectionKind kind, const char* name, std::uint32_t chars) {
        const Section& sec = code.sectionFor(kind);
        if (kind == SectionKind::Text || sec.size() > 0) {
            sections.push_back(OutSection{kind, name, chars, std::string(), &sec});
        }
    };
    pushStd(SectionKind::Text, ".text",
            IMAGE_SCN_CNT_CODE | IMAGE_SCN_ALIGN_16BYTES | IMAGE_SCN_MEM_EXECUTE |
                IMAGE_SCN_MEM_READ);
    pushStd(SectionKind::Data, ".data",
            IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_ALIGN_16BYTES |
                IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE);
    pushStd(SectionKind::RoData, ".rdata",
            IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_ALIGN_16BYTES |
                IMAGE_SCN_MEM_READ);
    pushStd(SectionKind::Bss, ".bss",
            IMAGE_SCN_CNT_UNINITIALIZED_DATA | IMAGE_SCN_ALIGN_16BYTES |
                IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE);
    // A named code section that backs a weak definition becomes a COMDAT, so the
    // linker folds the copies the other objects emit instead of rejecting them.
    std::vector<std::string> comdatSections;
    for (const Symbol& s : code.symbols) {
        if (s.defined && s.binding == SymbolBinding::LinkOnce && !s.customSection.empty()) {
            comdatSections.push_back(s.customSection);
        }
    }
    auto isComdatSection = [&](const std::string& name) {
        for (const auto& n : comdatSections) {
            if (n == name) return true;
        }
        return false;
    };
    for (const Section& es : code.extraText) {
        std::uint32_t chars = IMAGE_SCN_CNT_CODE | IMAGE_SCN_ALIGN_16BYTES |
                              IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;
        if (isComdatSection(es.name)) chars |= IMAGE_SCN_LNK_COMDAT;
        sections.push_back(
            OutSection{SectionKind::Text, es.name, chars, es.name, &es});
    }

    // Resolve a symbol's 1-based COFF section number. Standard symbols carry a
    // SectionKind; symbols with a `customSection` resolve by name.
    auto sectionNumberOf = [&](const Symbol& sym) -> std::int16_t {
        for (std::size_t i = 0; i < sections.size(); ++i) {
            if (!sym.customSection.empty()) {
                if (sections[i].customName == sym.customSection)
                    return static_cast<std::int16_t>(i + 1);
            } else if (sections[i].customName.empty() && sections[i].kind == sym.section) {
                return static_cast<std::int16_t>(i + 1);
            }
        }
        return IMAGE_SYM_UNDEFINED;
    };

    // Does relocation `r` belong to output section `i`?
    auto relocInSection = [&](const Relocation& r, std::size_t i) -> bool {
        if (!r.customSection.empty()) return sections[i].customName == r.customSection;
        return sections[i].customName.empty() && sections[i].kind == r.section;
    };

    // --- Lay out file offsets ------------------------------------------------
    // [file header][section headers][raw data per section][relocs per section]
    // [symbol table][string table]
    const std::uint32_t kFileHeaderSize = 20;
    const std::uint32_t kSectionHeaderSize = 40;
    const std::uint32_t kSymbolRecordSize = 18;
    const std::uint32_t kRelocRecordSize = 10;
    (void)kSymbolRecordSize;

    std::uint32_t cursor =
        kFileHeaderSize + kSectionHeaderSize * static_cast<std::uint32_t>(sections.size());

    struct SecLayout {
        std::uint32_t rawPtr = 0;
        std::uint32_t rawSize = 0;
        std::uint32_t relocPtr = 0;
        std::uint32_t relocCount = 0;
    };
    std::vector<SecLayout> secLayouts(sections.size());

    // Raw data pointers (Bss stores no bytes -> rawPtr 0).
    for (std::size_t i = 0; i < sections.size(); ++i) {
        const Section& sec = *sections[i].sec;
        if (sections[i].kind == SectionKind::Bss) {
            secLayouts[i].rawSize = static_cast<std::uint32_t>(sec.bssSize);
            secLayouts[i].rawPtr = 0;
        } else if (!sec.bytes.empty()) {
            secLayouts[i].rawSize = static_cast<std::uint32_t>(sec.bytes.size());
            secLayouts[i].rawPtr = cursor;
            cursor += secLayouts[i].rawSize;
        }
    }

    // Relocation pointers.
    for (std::size_t i = 0; i < sections.size(); ++i) {
        std::uint32_t count = 0;
        for (const auto& r : code.relocations) {
            if (relocInSection(r, i)) ++count;
        }
        if (count > 0) {
            secLayouts[i].relocCount = count;
            secLayouts[i].relocPtr = cursor;
            cursor += count * kRelocRecordSize;
        }
    }

    const std::uint32_t symbolTablePtr = cursor;

    // --- Plan the COFF symbol table ------------------------------------------
    // Most symbols map 1:1 to a COFF symbol record. A *weak definition*
    // (SymbolBinding::Weak on a defined symbol) is encoded the way MSVC/clang do
    // it: the public name becomes an IMAGE_SYM_CLASS_WEAK_EXTERNAL record (UNDEF,
    // value 0) carrying one weak-external aux record whose TagIndex points at a
    // separate real definition emitted under a hidden ".weak.<name>.default"
    // name. The aux records and the extra definition symbols shift COFF indices,
    // so we precompute a map from the caller's symbol index to its COFF record
    // index and remap relocations through it.
    struct SymPlan {
        const Symbol* sym;       // the source symbol
        bool weakDef;            // emit WEAK_EXTERNAL + aux + real definition
        bool comdat;             // emit section symbol + aux + EXTERNAL definition
        std::uint32_t coffIndex; // index of this symbol's primary COFF record
        std::uint32_t defIndex;  // (weakDef only) index of the real definition record
    };
    std::vector<SymPlan> plan(code.symbols.size());
    std::vector<std::uint32_t> origToCoff(code.symbols.size(), 0);
    std::uint32_t coffSymCount = 0;
    for (std::size_t i = 0; i < code.symbols.size(); ++i) {
        const Symbol& sym = code.symbols[i];
        const bool weakDef = sym.defined && sym.binding == SymbolBinding::Weak;
        const bool comdat = sym.defined && sym.binding == SymbolBinding::LinkOnce &&
                            !sym.customSection.empty();
        plan[i].sym = &sym;
        plan[i].weakDef = weakDef;
        plan[i].comdat = comdat;
        plan[i].coffIndex = coffSymCount;
        origToCoff[i] = coffSymCount;
        if (comdat) {
            // [coffSymCount]   = the section symbol, carrying the COMDAT selection
            // [coffSymCount+1] = its section-definition aux record
            // [coffSymCount+2] = the definition itself, EXTERNAL
            // The COMDAT selection lives on the section symbol, which must be the
            // first symbol referring to that section -- hence this ordering.
            // Relocations must target the definition, not the section symbol.
            origToCoff[i] = coffSymCount + 2;
            coffSymCount += 3;
        } else if (weakDef) {
            // [coffSymCount]   = WEAK_EXTERNAL (public name), 1 aux record
            // [coffSymCount+2] = real definition (.weak.<name>.default)
            plan[i].defIndex = coffSymCount + 2;
            coffSymCount += 3;  // weak record + 1 aux + real-def record
        } else {
            coffSymCount += 1;
        }
    }
    const std::uint32_t symbolCount = coffSymCount;

    // --- Build the string table (names > 8 bytes go here) --------------------
    // COFF string table begins with its own 4-byte size and is referenced by
    // a 4-byte offset (with the symbol-name field's first 4 bytes set to 0).
    std::vector<std::uint8_t> stringTable;
    put32(stringTable, 0);  // size placeholder, patched below
    auto internName = [&](const std::string& name, std::uint8_t shortName[8]) {
        if (name.size() <= 8) {
            std::memset(shortName, 0, 8);
            std::memcpy(shortName, name.data(), name.size());
        } else {
            std::uint32_t off = static_cast<std::uint32_t>(stringTable.size());
            std::memset(shortName, 0, 8);
            // name field: 4 zero bytes then 4-byte offset into string table.
            shortName[4] = off & 0xFF;
            shortName[5] = (off >> 8) & 0xFF;
            shortName[6] = (off >> 16) & 0xFF;
            shortName[7] = (off >> 24) & 0xFF;
            stringTable.insert(stringTable.end(), name.begin(), name.end());
            stringTable.push_back(0);
        }
    };
    // Section-header names follow the same long-name convention but use a textual
    // "/offset" form (the slash plus the decimal string-table offset).
    auto internSectionName = [&](const std::string& name, std::uint8_t field[8]) {
        std::memset(field, 0, 8);
        if (name.size() <= 8) {
            std::memcpy(field, name.data(), name.size());
        } else {
            std::uint32_t off = static_cast<std::uint32_t>(stringTable.size());
            stringTable.insert(stringTable.end(), name.begin(), name.end());
            stringTable.push_back(0);
            std::string slash = "/" + std::to_string(off);
            std::memcpy(field, slash.data(), std::min<std::size_t>(slash.size(), 8));
        }
    };
    (void)IMAGE_SCN_ALIGN_1BYTES;

    // --- Emit the file -------------------------------------------------------
    std::vector<std::uint8_t> out;

    // File header (IMAGE_FILE_HEADER)
    put16(out, IMAGE_FILE_MACHINE_AMD64);
    put16(out, static_cast<std::uint16_t>(sections.size()));
    put32(out, 0);  // TimeDateStamp
    put32(out, symbolTablePtr);
    put32(out, symbolCount);
    put16(out, 0);  // SizeOfOptionalHeader (0 for object files)
    put16(out, 0);  // Characteristics

    // Section headers
    for (std::size_t i = 0; i < sections.size(); ++i) {
        std::uint8_t name[8] = {0};
        internSectionName(sections[i].name, name);
        out.insert(out.end(), name, name + 8);
        put32(out, 0);                          // VirtualSize (0 for obj)
        put32(out, 0);                          // VirtualAddress (0 for obj)
        put32(out, secLayouts[i].rawSize);      // SizeOfRawData
        put32(out, secLayouts[i].rawPtr);       // PointerToRawData
        put32(out, secLayouts[i].relocPtr);     // PointerToRelocations
        put32(out, 0);                          // PointerToLinenumbers
        put16(out, static_cast<std::uint16_t>(secLayouts[i].relocCount));
        put16(out, 0);                          // NumberOfLinenumbers
        put32(out, sections[i].characteristics);
    }

    // Raw section data (in the same order as headers; Bss emits nothing).
    for (std::size_t i = 0; i < sections.size(); ++i) {
        if (sections[i].kind == SectionKind::Bss) continue;
        const Section& sec = *sections[i].sec;
        out.insert(out.end(), sec.bytes.begin(), sec.bytes.end());
    }

    // Relocation records per section.
    for (std::size_t i = 0; i < sections.size(); ++i) {
        for (const auto& r : code.relocations) {
            if (!relocInSection(r, i)) continue;
            if (r.symbol >= code.symbols.size()) {
                errorOut = "relocation references out-of-range symbol index";
                return false;
            }
            put32(out, static_cast<std::uint32_t>(r.offset));  // VirtualAddress
            put32(out, origToCoff[r.symbol]);                  // SymbolTableIndex
            put16(out, mapReloc(r.kind));                      // Type
        }
    }

    // Symbol table. Most symbols emit a single 18-byte record; a weak definition
    // emits three records (WEAK_EXTERNAL + aux + the real definition).
    auto emitName = [&](const std::string& name) {
        std::uint8_t shortName[8];
        internName(name, shortName);
        out.insert(out.end(), shortName, shortName + 8);
    };
    for (const auto& p : plan) {
        const Symbol& sym = *p.sym;
        if (p.comdat) {
            const std::int16_t secNum = sectionNumberOf(sym);
            const std::size_t si = static_cast<std::size_t>(secNum - 1);
            // (1) Section symbol: named after the section, STATIC, one aux record.
            emitName(sections[si].name);
            put32(out, 0);                                        // Value
            put16(out, static_cast<std::uint16_t>(secNum));        // SectionNumber
            put16(out, 0);                                         // Type
            out.push_back(IMAGE_SYM_CLASS_STATIC);                 // StorageClass
            out.push_back(1);                                      // NumberOfAuxSymbols
            // (1a) Section-definition aux record: the Selection field is what makes
            // this a COMDAT the linker may discard in favour of an identical copy.
            put32(out, secLayouts[si].rawSize);                    // Length
            put16(out, static_cast<std::uint16_t>(secLayouts[si].relocCount));
            put16(out, 0);                                         // NumberOfLinenumbers
            put32(out, 0);                                         // CheckSum
            put16(out, static_cast<std::uint16_t>(secNum));         // Number
            out.push_back(IMAGE_COMDAT_SELECT_ANY);                // Selection
            out.push_back(0);
            out.push_back(0);
            out.push_back(0);                                      // Unused
            // (2) The definition itself, an ordinary external in that section.
            emitName(sym.name);
            put32(out, static_cast<std::uint32_t>(sym.offset));    // Value
            put16(out, static_cast<std::uint16_t>(secNum));        // SectionNumber
            put16(out, sym.isFunction ? IMAGE_SYM_TYPE_FUNCTION : 0);
            out.push_back(IMAGE_SYM_CLASS_EXTERNAL);               // StorageClass
            out.push_back(0);                                      // NumberOfAuxSymbols
            continue;
        }
        if (p.weakDef) {
            // (1) Public weak symbol: WEAK_EXTERNAL, undefined, value 0, 1 aux.
            emitName(sym.name);
            put32(out, 0);                                   // Value
            put16(out, static_cast<std::uint16_t>(IMAGE_SYM_UNDEFINED));  // SectionNumber
            put16(out, sym.isFunction ? IMAGE_SYM_TYPE_FUNCTION : 0);     // Type
            out.push_back(IMAGE_SYM_CLASS_WEAK_EXTERNAL);    // StorageClass
            out.push_back(1);                                // NumberOfAuxSymbols
            // (1a) Weak-external aux record (18 bytes): TagIndex -> default def,
            // Characteristics = ALIAS, rest zero.
            put32(out, p.defIndex);                          // TagIndex
            put32(out, IMAGE_WEAK_EXTERN_SEARCH_ALIAS);      // Characteristics
            for (int k = 0; k < 10; ++k) out.push_back(0);   // padding to 18 bytes
            // (2) Real definition under a hidden default name. This is the
            // overridable kind of weak: a strong definition of `sym.name` in
            // another object wins, and this body is dropped. Folding several
            // identical definitions is a different problem, handled by the COMDAT
            // path above.
            emitName(".weak." + sym.name + ".default");
            put32(out, static_cast<std::uint32_t>(sym.offset));  // Value
            put16(out, static_cast<std::uint16_t>(sectionNumberOf(sym)));  // SectionNumber
            put16(out, sym.isFunction ? IMAGE_SYM_TYPE_FUNCTION : 0);      // Type
            out.push_back(IMAGE_SYM_CLASS_EXTERNAL);         // StorageClass
            out.push_back(0);                                // NumberOfAuxSymbols
            continue;
        }

        const std::string coffName =
            (!sym.dll.empty() && !sym.defined &&
             sym.name.rfind("__imp_", 0) != 0)
                ? "__imp_" + sym.name
                : sym.name;
        emitName(coffName);
        put32(out, static_cast<std::uint32_t>(sym.offset));  // Value (section offset)
        std::int16_t secNum =
            sym.defined ? sectionNumberOf(sym) : IMAGE_SYM_UNDEFINED;
        put16(out, static_cast<std::uint16_t>(secNum));      // SectionNumber
        put16(out, sym.isFunction ? IMAGE_SYM_TYPE_FUNCTION : 0);  // Type
        // Local => STATIC; Global/External => EXTERNAL.
        std::uint8_t storage = (sym.binding == SymbolBinding::Local)
                                   ? IMAGE_SYM_CLASS_STATIC
                                   : IMAGE_SYM_CLASS_EXTERNAL;
        out.push_back(storage);
        out.push_back(0);  // NumberOfAuxSymbols
    }

    // Patch string-table size, then append it.
    std::uint32_t stSize = static_cast<std::uint32_t>(stringTable.size());
    stringTable[0] = stSize & 0xFF;
    stringTable[1] = (stSize >> 8) & 0xFF;
    stringTable[2] = (stSize >> 16) & 0xFF;
    stringTable[3] = (stSize >> 24) & 0xFF;
    out.insert(out.end(), stringTable.begin(), stringTable.end());

    // --- Write to disk -------------------------------------------------------
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        errorOut = "could not open object output '" + path + "'";
        return false;
    }
    file.write(reinterpret_cast<const char*>(out.data()),
               static_cast<std::streamsize>(out.size()));
    if (!file.good()) {
        errorOut = "failed writing object '" + path + "'";
        return false;
    }
    return true;
}

}  // namespace Backend

