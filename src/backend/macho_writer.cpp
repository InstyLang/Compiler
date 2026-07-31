#include <backend/macho_writer.hpp>

#include <cstdint>
#include <fstream>
#include <vector>

namespace Backend {

namespace {

// --- Mach-O constants (mach-o/loader.h, nlist.h, reloc.h) --------------------
constexpr std::uint32_t MH_MAGIC_64 = 0xFEEDFACFu;
constexpr std::uint32_t CPU_TYPE_X86_64 = 0x01000007u;     // CPU_TYPE_X86 | ABI64
constexpr std::uint32_t CPU_SUBTYPE_X86_64_ALL = 0x00000003u;
constexpr std::uint32_t MH_OBJECT = 0x1u;                  // relocatable object
constexpr std::uint32_t MH_SUBSECTIONS_VIA_SYMBOLS = 0x2000u;

constexpr std::uint32_t LC_SEGMENT_64 = 0x19u;
constexpr std::uint32_t LC_SYMTAB = 0x2u;

// section_64.flags
constexpr std::uint32_t S_REGULAR = 0x0u;
constexpr std::uint32_t S_ZEROFILL = 0x1u;
constexpr std::uint32_t S_CSTRING_LITERALS = 0x2u;
constexpr std::uint32_t S_ATTR_PURE_INSTRUCTIONS = 0x80000000u;
constexpr std::uint32_t S_ATTR_SOME_INSTRUCTIONS = 0x00000400u;

// nlist_64.n_type
constexpr std::uint8_t N_EXT = 0x01u;   // external symbol
constexpr std::uint8_t N_SECT = 0x0Eu;  // defined in section n_sect
constexpr std::uint8_t N_UNDF = 0x00u;  // undefined

constexpr std::uint8_t NO_SECT = 0u;    // n_sect for undefined symbols

// x86-64 relocation types (mach-o/x86_64/reloc.h)
constexpr std::uint8_t X86_64_RELOC_UNSIGNED = 0u;   // absolute
constexpr std::uint8_t X86_64_RELOC_BRANCH = 2u;     // call/jmp rel32
constexpr std::uint8_t X86_64_RELOC_SIGNED = 1u;     // rip-relative data ref
constexpr std::uint8_t X86_64_RELOC_SIGNED_1 = 6u;   // rip-relative, -1 bias

// VM protections
constexpr std::uint32_t VM_PROT_READ = 0x1u;
constexpr std::uint32_t VM_PROT_WRITE = 0x2u;
constexpr std::uint32_t VM_PROT_EXECUTE = 0x4u;

// --- little-endian append helpers --------------------------------------------
void put16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(x & 0xFF);
    v.push_back((x >> 8) & 0xFF);
}
void put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back((x >> (8 * i)) & 0xFF);
}
void put64(std::vector<std::uint8_t>& v, std::uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back((x >> (8 * i)) & 0xFF);
}
// Fixed-size 16-byte section/segment name field (NUL-padded, not NUL-required).
void putName16(std::vector<std::uint8_t>& v, const char* name) {
    std::uint8_t buf[16] = {0};
    for (int i = 0; i < 16 && name[i]; ++i) buf[i] = static_cast<std::uint8_t>(name[i]);
    v.insert(v.end(), buf, buf + 16);
}

// A string table that interns names and returns their byte offset. Mach-O string
// tables start with a leading NUL byte (offset 0 == the empty string).
struct StrTab {
    std::vector<std::uint8_t> bytes{0};
    std::uint32_t add(const std::string& s) {
        if (s.empty()) return 0;
        std::uint32_t off = static_cast<std::uint32_t>(bytes.size());
        bytes.insert(bytes.end(), s.begin(), s.end());
        bytes.push_back(0);
        return off;
    }
};

// Maps a RelocKind to the Mach-O (type, pcrel, length) triple. length is the
// log2 of the field size (2 => 4 bytes, 3 => 8 bytes).
struct MachReloc { std::uint8_t type; std::uint8_t pcrel; std::uint8_t length; bool ok; };
MachReloc mapReloc(RelocKind kind) {
    switch (kind) {
        case RelocKind::Abs64:      return {X86_64_RELOC_UNSIGNED, 0, 3, true};
        case RelocKind::Rel32:      return {X86_64_RELOC_BRANCH, 1, 2, true};
        case RelocKind::RipData32:  return {X86_64_RELOC_SIGNED, 1, 2, true};
        case RelocKind::Rel32_1:    return {X86_64_RELOC_SIGNED_1, 1, 2, true};
        // No clean Mach-O analogue for absolute 32-bit (macOS is PIC-only) or the
        // PE-only import-call relocation.
        case RelocKind::Addr32:
        case RelocKind::Addr32NB:
        case RelocKind::ImportCall32:
        default:                    return {0, 0, 0, false};
    }
}

// A content section to emit, paired with its segment/section names and flags.
struct OutSection {
    SectionKind kind;
    std::string customName;       // non-empty => a named code section (extraText)
    const char* segName;
    const char* sectName;
    std::uint32_t flags;
    bool zerofill;                // bss: occupies addr space, no file bytes
    const Section* sec = nullptr;
};

}  // namespace

bool MachOWriter::write(const MachineCode& code, const std::string& path,
                        std::string& errorOut) {
    // --- Decide which content sections are present ---------------------------
    std::vector<OutSection> content;
    auto pushStd = [&](SectionKind kind, const char* seg, const char* sect,
                       std::uint32_t flags, bool zerofill) {
        const Section& sec = code.sectionFor(kind);
        if (kind == SectionKind::Text || sec.size() > 0)
            content.push_back(OutSection{kind, std::string(), seg, sect, flags,
                                         zerofill, &sec});
    };
    pushStd(SectionKind::Text, "__TEXT", "__text",
            S_REGULAR | S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS, false);
    pushStd(SectionKind::Data, "__DATA", "__data", S_REGULAR, false);
    // String literals / const tables live in __TEXT,__const (no cstring merging
    // so internal labels into the middle of the table stay valid).
    pushStd(SectionKind::RoData, "__TEXT", "__const", S_REGULAR, false);
    pushStd(SectionKind::Bss, "__DATA", "__bss", S_ZEROFILL, true);
    for (const Section& es : code.extraText) {
        content.push_back(OutSection{SectionKind::Text, es.name, "__TEXT", "__text",
                                     S_REGULAR | S_ATTR_PURE_INSTRUCTIONS |
                                         S_ATTR_SOME_INSTRUCTIONS,
                                     false, &es});
    }

    // Resolve a (kind, customName) pair to its 0-based index in `content`.
    auto contentIndexFor = [&](SectionKind kind, const std::string& custom) -> int {
        for (std::size_t i = 0; i < content.size(); ++i) {
            if (!custom.empty()) {
                if (content[i].customName == custom) return static_cast<int>(i);
            } else if (content[i].customName.empty() && content[i].kind == kind) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };
    // 1-based Mach-O section number for content[i].
    auto sectNo = [&](std::size_t i) -> std::uint8_t {
        return static_cast<std::uint8_t>(i + 1);
    };

    // Does relocation `r` belong to content section `i`?
    auto relocInContent = [&](const Relocation& r, std::size_t i) -> bool {
        if (!r.customSection.empty()) return content[i].customName == r.customSection;
        return content[i].customName.empty() && content[i].kind == r.section;
    };

    // --- Build the symbol table ----------------------------------------------
    // Mach-O symbols are grouped: local, then external-defined, then undefined.
    // Defined symbol names get the platform leading-underscore prefix. We remap
    // the caller's symbol indices (used by relocations) to the final order.
    StrTab strtab;
    struct MachSym {
        std::uint32_t nameOff;
        std::uint8_t type;
        std::uint8_t sect;   // 1-based section number, or NO_SECT
        std::uint16_t desc;
        std::uint64_t value;
    };
    std::vector<MachSym> localSyms, extSyms, undefSyms;
    // origToFinal[i] = final index in the concatenated [local, ext, undef] table.
    std::vector<std::uint32_t> origToFinal(code.symbols.size(), 0);
    std::vector<int> bucket(code.symbols.size(), 0);  // 0=local,1=ext,2=undef
    std::vector<std::uint32_t> withinBucket(code.symbols.size(), 0);

    auto symSect = [&](const Symbol& sym) -> std::uint8_t {
        int ci = contentIndexFor(sym.customSection.empty() ? sym.section
                                                           : SectionKind::Text,
                                 sym.customSection);
        return ci < 0 ? NO_SECT : sectNo(static_cast<std::size_t>(ci));
    };

    for (std::size_t i = 0; i < code.symbols.size(); ++i) {
        const Symbol& sym = code.symbols[i];
        MachSym ms{};
        // All emitted symbols carry the leading-underscore C ABI prefix on macOS.
        ms.nameOff = strtab.add("_" + sym.name);
        if (!sym.defined) {
            ms.type = N_UNDF | N_EXT;
            ms.sect = NO_SECT;
            ms.value = 0;
            bucket[i] = 2;
            withinBucket[i] = static_cast<std::uint32_t>(undefSyms.size());
            undefSyms.push_back(ms);
        } else if (sym.binding == SymbolBinding::Local) {
            ms.type = N_SECT;  // defined, not external
            ms.sect = symSect(sym);
            ms.value = sym.offset;
            bucket[i] = 0;
            withinBucket[i] = static_cast<std::uint32_t>(localSyms.size());
            localSyms.push_back(ms);
        } else {
            // Global or Weak: defined + external. (Weak attributes via N_WEAK_DEF
            // in n_desc could be added later; treat as a plain external def.)
            ms.type = N_SECT | N_EXT;
            ms.sect = symSect(sym);
            ms.value = sym.offset;
            bucket[i] = 1;
            withinBucket[i] = static_cast<std::uint32_t>(extSyms.size());
            extSyms.push_back(ms);
        }
    }
    const std::uint32_t nLocal = static_cast<std::uint32_t>(localSyms.size());
    const std::uint32_t nExt = static_cast<std::uint32_t>(extSyms.size());
    for (std::size_t i = 0; i < code.symbols.size(); ++i) {
        std::uint32_t base = bucket[i] == 0 ? 0
                          : bucket[i] == 1 ? nLocal
                                           : (nLocal + nExt);
        origToFinal[i] = base + withinBucket[i];
    }

    // --- Lay out section virtual addresses + file offsets --------------------
    // In an MH_OBJECT all sections share one segment starting at address 0; each
    // section's addr is the running (aligned) virtual cursor. Zerofill sections
    // must be ordered last in address space but consume no file bytes.
    auto align = [](std::uint64_t v, std::uint64_t a) {
        return a <= 1 ? v : (v + a - 1) & ~(a - 1);
    };

    constexpr std::uint64_t kHeaderSize = 32;       // mach_header_64
    constexpr std::uint64_t kSegCmdSize = 72;       // segment_command_64
    constexpr std::uint64_t kSectSize = 80;         // section_64
    constexpr std::uint64_t kSymtabCmdSize = 24;    // symtab_command
    constexpr std::uint64_t kNlistSize = 16;        // nlist_64
    constexpr std::uint64_t kRelocSize = 8;         // relocation_info

    const std::uint64_t loadCmdsSize =
        kSegCmdSize + kSectSize * content.size() + kSymtabCmdSize;
    const std::uint64_t fileDataStart = kHeaderSize + loadCmdsSize;

    // Virtual addresses (addr) and file offsets for non-zerofill content.
    std::vector<std::uint64_t> sectAddr(content.size(), 0);
    std::vector<std::uint64_t> sectFileOff(content.size(), 0);
    std::uint64_t vmCursor = 0;
    std::uint64_t fileCursor = fileDataStart;
    // First pass: non-zerofill sections (file-backed).
    for (std::size_t i = 0; i < content.size(); ++i) {
        if (content[i].zerofill) continue;
        const Section& sec = *content[i].sec;
        std::uint64_t a = sec.alignment ? sec.alignment : 1;
        vmCursor = align(vmCursor, a);
        fileCursor = align(fileCursor, a);
        sectAddr[i] = vmCursor;
        sectFileOff[i] = fileCursor;
        vmCursor += sec.size();
        fileCursor += sec.size();
    }
    // Second pass: zerofill sections (addr space only, no file bytes).
    for (std::size_t i = 0; i < content.size(); ++i) {
        if (!content[i].zerofill) continue;
        const Section& sec = *content[i].sec;
        std::uint64_t a = sec.alignment ? sec.alignment : 1;
        vmCursor = align(vmCursor, a);
        sectAddr[i] = vmCursor;
        sectFileOff[i] = 0;  // zerofill: offset ignored
        vmCursor += sec.size();
    }
    const std::uint64_t segVmSize = vmCursor;
    const std::uint64_t segFileSize = fileCursor - fileDataStart;

    // --- Build relocation_info records, per content section ------------------
    std::vector<std::vector<std::uint8_t>> relBodies(content.size());
    std::vector<std::uint32_t> relCount(content.size(), 0);
    for (std::size_t ci = 0; ci < content.size(); ++ci) {
        auto& body = relBodies[ci];
        for (const auto& r : code.relocations) {
            if (!relocInContent(r, ci)) continue;
            if (r.symbol >= code.symbols.size()) {
                errorOut = "relocation references out-of-range symbol index";
                return false;
            }
            MachReloc mr = mapReloc(r.kind);
            if (!mr.ok) {
                errorOut = "Mach-O writer: unsupported relocation kind";
                return false;
            }
            // Non-scattered, external relocation: r_symbolnum is a symbol-table
            // index (r_extern = 1). The addend is folded into the field bytes by
            // the encoder; Mach-O carries no separate addend field for these.
            put32(body, static_cast<std::uint32_t>(r.offset));  // r_address
            std::uint32_t packed = (origToFinal[r.symbol] & 0x00FFFFFFu) |
                                   (static_cast<std::uint32_t>(mr.pcrel & 1) << 24) |
                                   (static_cast<std::uint32_t>(mr.length & 3) << 25) |
                                   (1u << 27) |  // r_extern
                                   (static_cast<std::uint32_t>(mr.type & 0xF) << 28);
            put32(body, packed);
            ++relCount[ci];
        }
    }
    // Place relocation tables after the section data.
    fileCursor = align(fileCursor, 4);
    std::vector<std::uint64_t> relFileOff(content.size(), 0);
    for (std::size_t i = 0; i < content.size(); ++i) {
        if (relCount[i] == 0) continue;
        relFileOff[i] = fileCursor;
        fileCursor += relBodies[i].size();
    }

    // --- Symbol + string tables ----------------------------------------------
    std::vector<std::uint8_t> symtabBytes;
    auto emitSym = [&](const MachSym& s) {
        put32(symtabBytes, s.nameOff);  // n_strx
        symtabBytes.push_back(s.type);  // n_type
        symtabBytes.push_back(s.sect);  // n_sect
        put16(symtabBytes, s.desc);     // n_desc
        put64(symtabBytes, s.value);    // n_value
    };
    for (const auto& s : localSyms) emitSym(s);
    for (const auto& s : extSyms) emitSym(s);
    for (const auto& s : undefSyms) emitSym(s);
    const std::uint32_t totalSyms = nLocal + nExt +
                                    static_cast<std::uint32_t>(undefSyms.size());

    fileCursor = align(fileCursor, 8);
    const std::uint64_t symtabOff = fileCursor;
    fileCursor += symtabBytes.size();
    const std::uint64_t strtabOff = fileCursor;
    fileCursor += strtab.bytes.size();

    // --- Emit the file -------------------------------------------------------
    std::vector<std::uint8_t> out;

    // mach_header_64.
    put32(out, MH_MAGIC_64);
    put32(out, CPU_TYPE_X86_64);
    put32(out, CPU_SUBTYPE_X86_64_ALL);
    put32(out, MH_OBJECT);
    put32(out, 2);                                  // ncmds: LC_SEGMENT_64 + LC_SYMTAB
    put32(out, static_cast<std::uint32_t>(loadCmdsSize));  // sizeofcmds
    put32(out, MH_SUBSECTIONS_VIA_SYMBOLS);         // flags
    put32(out, 0);                                  // reserved

    // LC_SEGMENT_64 (one segment, name "" for MH_OBJECT).
    put32(out, LC_SEGMENT_64);
    put32(out, static_cast<std::uint32_t>(kSegCmdSize + kSectSize * content.size()));
    putName16(out, "");                             // segname
    put64(out, 0);                                  // vmaddr
    put64(out, segVmSize);                          // vmsize
    put64(out, fileDataStart);                      // fileoff
    put64(out, segFileSize);                        // filesize
    put32(out, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE);  // maxprot
    put32(out, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE);  // initprot
    put32(out, static_cast<std::uint32_t>(content.size()));      // nsects
    put32(out, 0);                                  // flags

    auto log2align = [](std::uint64_t a) -> std::uint32_t {
        std::uint32_t l = 0;
        while ((std::uint64_t(1) << l) < (a ? a : 1)) ++l;
        return l;
    };
    for (std::size_t i = 0; i < content.size(); ++i) {
        const Section& sec = *content[i].sec;
        putName16(out, content[i].sectName);        // sectname
        putName16(out, content[i].segName);         // segname
        put64(out, sectAddr[i]);                    // addr
        put64(out, sec.size());                     // size
        put32(out, content[i].zerofill
                       ? 0u
                       : static_cast<std::uint32_t>(sectFileOff[i]));  // offset
        put32(out, log2align(sec.alignment ? sec.alignment : 1));      // align
        put32(out, relCount[i] ? static_cast<std::uint32_t>(relFileOff[i]) : 0u);  // reloff
        put32(out, relCount[i]);                    // nreloc
        put32(out, content[i].flags);               // flags
        put32(out, 0);                              // reserved1
        put32(out, 0);                              // reserved2
        put32(out, 0);                              // reserved3
    }

    // LC_SYMTAB.
    put32(out, LC_SYMTAB);
    put32(out, static_cast<std::uint32_t>(kSymtabCmdSize));
    put32(out, static_cast<std::uint32_t>(symtabOff));    // symoff
    put32(out, totalSyms);                                // nsyms
    put32(out, static_cast<std::uint32_t>(strtabOff));    // stroff
    put32(out, static_cast<std::uint32_t>(strtab.bytes.size()));  // strsize

    auto padTo = [&](std::uint64_t target) {
        while (out.size() < target) out.push_back(0);
    };

    // Section data (file-backed only).
    for (std::size_t i = 0; i < content.size(); ++i) {
        if (content[i].zerofill) continue;
        padTo(sectFileOff[i]);
        const Section& sec = *content[i].sec;
        out.insert(out.end(), sec.bytes.begin(), sec.bytes.end());
    }
    // Relocation tables.
    for (std::size_t i = 0; i < content.size(); ++i) {
        if (relCount[i] == 0) continue;
        padTo(relFileOff[i]);
        out.insert(out.end(), relBodies[i].begin(), relBodies[i].end());
    }
    // Symbol table + string table.
    padTo(symtabOff);
    out.insert(out.end(), symtabBytes.begin(), symtabBytes.end());
    padTo(strtabOff);
    out.insert(out.end(), strtab.bytes.begin(), strtab.bytes.end());

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
