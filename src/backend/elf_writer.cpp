#include <backend/elf_writer.hpp>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

namespace Backend {

namespace {

// --- ELF64 constants (elf.h names) -------------------------------------------
constexpr std::uint8_t ELFMAG[4] = {0x7F, 'E', 'L', 'F'};
constexpr std::uint8_t ELFCLASS64 = 2;
constexpr std::uint8_t ELFDATA2LSB = 1;
constexpr std::uint8_t EV_CURRENT = 1;
constexpr std::uint8_t ELFOSABI_SYSV = 0;

constexpr std::uint16_t ET_REL = 1;        // relocatable object
constexpr std::uint16_t EM_X86_64 = 62;    // AMD x86-64

// Section types
constexpr std::uint32_t SHT_NULL = 0;
constexpr std::uint32_t SHT_PROGBITS = 1;
constexpr std::uint32_t SHT_SYMTAB = 2;
constexpr std::uint32_t SHT_STRTAB = 3;
constexpr std::uint32_t SHT_RELA = 4;
constexpr std::uint32_t SHT_NOBITS = 8;

// Section flags
constexpr std::uint64_t SHF_WRITE = 0x1;
constexpr std::uint64_t SHF_ALLOC = 0x2;
constexpr std::uint64_t SHF_EXECINSTR = 0x4;

// Symbol binding / type (st_info = (bind << 4) | type)
constexpr std::uint8_t STB_LOCAL = 0;
constexpr std::uint8_t STB_GLOBAL = 1;
constexpr std::uint8_t STB_WEAK = 2;
constexpr std::uint8_t STT_NOTYPE = 0;
constexpr std::uint8_t STT_FUNC = 2;

// Special section indices
constexpr std::uint16_t SHN_UNDEF = 0;

// x86-64 relocation types
constexpr std::uint32_t R_X86_64_64 = 1;     // direct 64-bit
constexpr std::uint32_t R_X86_64_PC32 = 2;   // PC-relative 32-bit
constexpr std::uint32_t R_X86_64_PLT32 = 4;  // PC-relative 32-bit to PLT entry
constexpr std::uint32_t R_X86_64_32 = 10;    // direct 32-bit zero-extended

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

// A string table that interns names and returns their byte offset.
struct StrTab {
    std::vector<std::uint8_t> bytes{0};  // index 0 is the empty string
    std::uint32_t add(const std::string& s) {
        if (s.empty()) return 0;
        std::uint32_t off = static_cast<std::uint32_t>(bytes.size());
        bytes.insert(bytes.end(), s.begin(), s.end());
        bytes.push_back(0);
        return off;
    }
};

std::uint32_t mapReloc(RelocKind kind) {
    switch (kind) {
        case RelocKind::Abs64: return R_X86_64_64;
        // A `call`/`jmp` rel32 to a possibly-external symbol uses PLT32 in
        // modern toolchains; the linker relaxes it to PC32 when the target is
        // local. This is the correct choice for the encoder's callSymbol().
        case RelocKind::Rel32: return R_X86_64_PLT32;
        case RelocKind::RipData32: return R_X86_64_PC32;  // lea reg,[rip+x] to data
        case RelocKind::Rel32_1: return R_X86_64_PC32;  // closest ELF analogue
        case RelocKind::Addr32: return R_X86_64_32;
        case RelocKind::Addr32NB: return R_X86_64_32;  // no image-base concept in ELF
        case RelocKind::ImportCall32: return R_X86_64_PLT32;  // PE-only; n/a for ELF
    }
    return R_X86_64_PC32;
}

// Adjust the encoder's COFF-oriented addend to ELF's RELA convention. COFF
// REL32 is implicitly relative to the end of the 4-byte field; ELF PC-relative
// relocs make that explicit with addend = -4.
std::int64_t adjustAddend(RelocKind kind, std::int64_t addend) {
    switch (kind) {
        case RelocKind::Rel32:
        case RelocKind::RipData32:
        case RelocKind::Rel32_1:
            return addend - 4;
        default:
            return addend;
    }
}

struct OutSection {
    SectionKind kind;
    std::string name;
    std::uint32_t type;
    std::uint64_t flags;
    std::string customName;       // non-empty => a named code section (extraText)
    const Section* sec = nullptr; // the bytes/size to emit
};

}  // namespace

bool ElfWriter::write(const MachineCode& code, const std::string& path,
                      std::string& errorOut) {
    // --- Decide which content sections are present ---------------------------
    std::vector<OutSection> content;
    auto pushStd = [&](SectionKind kind, const char* name, std::uint32_t type,
                       std::uint64_t flags) {
        const Section& sec = code.sectionFor(kind);
        if (kind == SectionKind::Text || sec.size() > 0)
            content.push_back(OutSection{kind, name, type, flags, std::string(), &sec});
    };
    pushStd(SectionKind::Text, ".text", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR);
    pushStd(SectionKind::Data, ".data", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE);
    pushStd(SectionKind::RoData, ".rodata", SHT_PROGBITS, SHF_ALLOC);
    pushStd(SectionKind::Bss, ".bss", SHT_NOBITS, SHF_ALLOC | SHF_WRITE);
    for (const Section& es : code.extraText) {
        content.push_back(OutSection{SectionKind::Text, es.name, SHT_PROGBITS,
                                     SHF_ALLOC | SHF_EXECINSTR, es.name, &es});
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

    // --- Assign section header indices ---------------------------------------
    // Order: [0]=NULL, content sections..., .symtab, .strtab, .shstrtab,
    //        then one .rela.<name> per content section that has relocations.
    // `content[i]`'s section-header index is (i + 1).
    auto contentShndx = [&](std::size_t i) -> std::uint16_t {
        return static_cast<std::uint16_t>(i + 1);
    };
    std::uint16_t shndx = 1 + static_cast<std::uint16_t>(content.size());
    const std::uint16_t symtabShndx = shndx++;
    const std::uint16_t strtabShndx = shndx++;
    const std::uint16_t shstrtabShndx = shndx++;

    // Does relocation `r` belong to content section `i`?
    auto relocInContent = [&](const Relocation& r, std::size_t i) -> bool {
        if (!r.customSection.empty()) return content[i].customName == r.customSection;
        return content[i].customName.empty() && content[i].kind == r.section;
    };

    // Which content sections carry relocations, and in what order their
    // .rela.* sections appear.
    std::vector<std::size_t> relaForContent;  // index into `content`
    for (std::size_t i = 0; i < content.size(); ++i) {
        for (const auto& r : code.relocations) {
            if (relocInContent(r, i)) {
                relaForContent.push_back(i);
                break;
            }
        }
    }
    std::vector<std::uint16_t> relaShndx(content.size(), 0);
    for (std::size_t ci : relaForContent) {
        relaShndx[ci] = shndx++;
    }
    const std::uint16_t totalSections = shndx;

    // --- Build the symbol table ----------------------------------------------
    // ELF requires all STB_LOCAL symbols before any global. Index 0 is the
    // reserved undefined symbol. We also remap the caller's symbol indices
    // (used by relocations) to the reordered symtab indices.
    StrTab strtab;
    struct ElfSym {
        std::uint32_t nameOff;
        std::uint8_t info;
        std::uint16_t shndx;
        std::uint64_t value;
    };
    std::vector<ElfSym> locals;
    std::vector<ElfSym> globals;
    std::vector<std::uint32_t> origToElf(code.symbols.size(), 0);
    std::vector<bool> origIsLocal(code.symbols.size(), false);

    // Resolve a defined symbol's section-header index (handles custom sections).
    auto symShndx = [&](const Symbol& sym) -> std::uint16_t {
        int ci = contentIndexFor(sym.customSection.empty() ? sym.section : SectionKind::Text,
                                 sym.customSection);
        return ci < 0 ? SHN_UNDEF : contentShndx(static_cast<std::size_t>(ci));
    };

    for (std::size_t i = 0; i < code.symbols.size(); ++i) {
        const Symbol& sym = code.symbols[i];
        ElfSym es{};
        es.nameOff = strtab.add(sym.name);
        const std::uint8_t type = sym.isFunction ? STT_FUNC : STT_NOTYPE;
        if (!sym.defined) {
            es.info = (STB_GLOBAL << 4) | type;
            es.shndx = SHN_UNDEF;
            es.value = 0;
            globals.push_back(es);
            origIsLocal[i] = false;
        } else if (sym.binding == SymbolBinding::Local) {
            es.info = (STB_LOCAL << 4) | type;
            es.shndx = symShndx(sym);
            es.value = sym.offset;
            locals.push_back(es);
            origIsLocal[i] = true;
        } else {
            // Global or Weak. ELF distinguishes weak via STB_WEAK binding.
            // ELF has no separate link-once binding: STB_WEAK already permits several
        // definitions and folds them, so both kinds map onto it.
        std::uint8_t bind = (sym.binding == SymbolBinding::Weak ||
                             sym.binding == SymbolBinding::LinkOnce)
                                ? STB_WEAK
                                : STB_GLOBAL;
            es.info = (bind << 4) | type;
            es.shndx = symShndx(sym);
            es.value = sym.offset;
            globals.push_back(es);
            origIsLocal[i] = false;
        }
    }
    // Compute final indices: [0]=null, locals..., globals...
    {
        std::uint32_t localIdx = 1;  // after the null symbol
        std::uint32_t globalBase = 1 + static_cast<std::uint32_t>(locals.size());
        std::uint32_t li = 0, gi = 0;
        for (std::size_t i = 0; i < code.symbols.size(); ++i) {
            if (origIsLocal[i]) {
                origToElf[i] = localIdx + li++;
            } else {
                origToElf[i] = globalBase + gi++;
            }
        }
    }
    const std::uint32_t firstGlobalIdx = 1 + static_cast<std::uint32_t>(locals.size());

    // Serialize the symbol table (Elf64_Sym is 24 bytes).
    std::vector<std::uint8_t> symtab;
    auto emitSym = [&](const ElfSym& s) {
        put32(symtab, s.nameOff);   // st_name
        symtab.push_back(s.info);   // st_info
        symtab.push_back(0);        // st_other
        put16(symtab, s.shndx);     // st_shndx
        put64(symtab, s.value);     // st_value
        put64(symtab, 0);           // st_size
    };
    emitSym(ElfSym{0, 0, SHN_UNDEF, 0});  // index 0: undefined
    for (const auto& s : locals) emitSym(s);
    for (const auto& s : globals) emitSym(s);

    // --- Build .rela.* section bodies ----------------------------------------
    // Each Elf64_Rela is 24 bytes: r_offset(8), r_info(8), r_addend(8).
    // r_info = (sym_index << 32) | type.
    std::vector<std::vector<std::uint8_t>> relaBodies(content.size());
    for (std::size_t ci = 0; ci < content.size(); ++ci) {
        if (relaShndx[ci] == 0) continue;
        auto& body = relaBodies[ci];
        for (const auto& r : code.relocations) {
            if (!relocInContent(r, ci)) continue;
            if (r.symbol >= code.symbols.size()) {
                errorOut = "relocation references out-of-range symbol index";
                return false;
            }
            const std::uint64_t symIdx = origToElf[r.symbol];
            const std::uint32_t type = mapReloc(r.kind);
            put64(body, r.offset);                       // r_offset
            put64(body, (symIdx << 32) | type);          // r_info
            put64(body, static_cast<std::uint64_t>(      // r_addend
                            adjustAddend(r.kind, r.addend)));
        }
    }

    // --- Section header string table -----------------------------------------
    StrTab shstrtab;
    std::vector<std::uint32_t> contentNameOff(content.size());
    std::vector<std::uint32_t> relaNameOff(content.size(), 0);
    for (std::size_t i = 0; i < content.size(); ++i) {
        contentNameOff[i] = shstrtab.add(content[i].name);
    }
    const std::uint32_t symtabNameOff = shstrtab.add(".symtab");
    const std::uint32_t strtabNameOff = shstrtab.add(".strtab");
    const std::uint32_t shstrtabNameOff = shstrtab.add(".shstrtab");
    for (std::size_t i = 0; i < content.size(); ++i) {
        if (relaShndx[i] != 0) {
            relaNameOff[i] = shstrtab.add(std::string(".rela") + content[i].name);
        }
    }

    // --- Lay out file offsets ------------------------------------------------
    // [ELF header][section data...][section header table]
    constexpr std::uint64_t kEhSize = 64;
    constexpr std::uint64_t kShEntSize = 64;
    constexpr std::uint64_t kSymEntSize = 24;
    constexpr std::uint64_t kRelaEntSize = 24;

    std::uint64_t cursor = kEhSize;
    auto align = [](std::uint64_t v, std::uint64_t a) {
        return a <= 1 ? v : (v + a - 1) & ~(a - 1);
    };

    // Offsets for each content section's raw data (NOBITS occupies no space).
    std::vector<std::uint64_t> contentOff(content.size(), 0);
    for (std::size_t i = 0; i < content.size(); ++i) {
        const Section& sec = *content[i].sec;
        if (content[i].type == SHT_NOBITS) {
            contentOff[i] = cursor;  // no bytes consumed
            continue;
        }
        cursor = align(cursor, sec.alignment ? sec.alignment : 1);
        contentOff[i] = cursor;
        cursor += sec.bytes.size();
    }
    cursor = align(cursor, 8);
    const std::uint64_t symtabOff = cursor;
    cursor += symtab.size();
    const std::uint64_t strtabOff = cursor;
    cursor += strtab.bytes.size();
    const std::uint64_t shstrtabOff = cursor;
    cursor += shstrtab.bytes.size();

    std::vector<std::uint64_t> relaOff(content.size(), 0);
    for (std::size_t i = 0; i < content.size(); ++i) {
        if (relaShndx[i] == 0) continue;
        cursor = align(cursor, 8);
        relaOff[i] = cursor;
        cursor += relaBodies[i].size();
    }
    cursor = align(cursor, 8);
    const std::uint64_t shOff = cursor;

    // --- Emit the ELF header -------------------------------------------------
    std::vector<std::uint8_t> out;
    out.insert(out.end(), ELFMAG, ELFMAG + 4);
    out.push_back(ELFCLASS64);
    out.push_back(ELFDATA2LSB);
    out.push_back(EV_CURRENT);
    out.push_back(ELFOSABI_SYSV);
    for (int i = 0; i < 8; ++i) out.push_back(0);  // EI_PAD
    put16(out, ET_REL);          // e_type
    put16(out, EM_X86_64);       // e_machine
    put32(out, EV_CURRENT);      // e_version
    put64(out, 0);               // e_entry
    put64(out, 0);               // e_phoff
    put64(out, shOff);           // e_shoff
    put32(out, 0);               // e_flags
    put16(out, kEhSize);         // e_ehsize
    put16(out, 0);               // e_phentsize
    put16(out, 0);               // e_phnum
    put16(out, kShEntSize);      // e_shentsize
    put16(out, totalSections);   // e_shnum
    put16(out, shstrtabShndx);   // e_shstrndx

    // --- Emit section data ---------------------------------------------------
    auto padTo = [&](std::uint64_t target) {
        while (out.size() < target) out.push_back(0);
    };
    for (std::size_t i = 0; i < content.size(); ++i) {
        if (content[i].type == SHT_NOBITS) continue;
        padTo(contentOff[i]);
        const Section& sec = *content[i].sec;
        out.insert(out.end(), sec.bytes.begin(), sec.bytes.end());
    }
    padTo(symtabOff);
    out.insert(out.end(), symtab.begin(), symtab.end());
    padTo(strtabOff);
    out.insert(out.end(), strtab.bytes.begin(), strtab.bytes.end());
    padTo(shstrtabOff);
    out.insert(out.end(), shstrtab.bytes.begin(), shstrtab.bytes.end());
    for (std::size_t i = 0; i < content.size(); ++i) {
        if (relaShndx[i] == 0) continue;
        padTo(relaOff[i]);
        out.insert(out.end(), relaBodies[i].begin(), relaBodies[i].end());
    }
    padTo(shOff);

    // --- Emit the section header table ---------------------------------------
    auto emitShdr = [&](std::uint32_t name, std::uint32_t type, std::uint64_t flags,
                        std::uint64_t addr, std::uint64_t offset, std::uint64_t size,
                        std::uint32_t link, std::uint32_t info, std::uint64_t addralign,
                        std::uint64_t entsize) {
        put32(out, name);
        put32(out, type);
        put64(out, flags);
        put64(out, addr);
        put64(out, offset);
        put64(out, size);
        put32(out, link);
        put32(out, info);
        put64(out, addralign);
        put64(out, entsize);
    };

    // [0] null section
    emitShdr(0, SHT_NULL, 0, 0, 0, 0, 0, 0, 0, 0);
    // content sections
    for (std::size_t i = 0; i < content.size(); ++i) {
        const Section& sec = *content[i].sec;
        const std::uint64_t size = sec.size();
        const std::uint64_t off =
            content[i].type == SHT_NOBITS ? 0 : contentOff[i];
        emitShdr(contentNameOff[i], content[i].type, content[i].flags, 0, off,
                 size, 0, 0, sec.alignment ? sec.alignment : 1, 0);
    }
    // .symtab  (sh_link = .strtab index, sh_info = first global symbol index)
    emitShdr(symtabNameOff, SHT_SYMTAB, 0, 0, symtabOff, symtab.size(),
             strtabShndx, firstGlobalIdx, 8, kSymEntSize);
    // .strtab
    emitShdr(strtabNameOff, SHT_STRTAB, 0, 0, strtabOff, strtab.bytes.size(),
             0, 0, 1, 0);
    // .shstrtab
    emitShdr(shstrtabNameOff, SHT_STRTAB, 0, 0, shstrtabOff,
             shstrtab.bytes.size(), 0, 0, 1, 0);
    // .rela.<section>  (sh_link = .symtab index, sh_info = target section index)
    for (std::size_t i = 0; i < content.size(); ++i) {
        if (relaShndx[i] == 0) continue;
        emitShdr(relaNameOff[i], SHT_RELA, 0, 0, relaOff[i], relaBodies[i].size(),
                 symtabShndx, contentShndx(i), 8,
                 kRelaEntSize);
    }

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

