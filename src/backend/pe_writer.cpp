#include <backend/pe_writer.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>

namespace Backend {

namespace {

// --- PE constants (winnt.h names) --------------------------------------------
constexpr std::uint16_t IMAGE_DOS_SIGNATURE = 0x5A4D;  // "MZ"
constexpr std::uint32_t IMAGE_NT_SIGNATURE = 0x00004550;  // "PE\0\0"
constexpr std::uint16_t IMAGE_FILE_MACHINE_AMD64 = 0x8664;
constexpr std::uint16_t IMAGE_FILE_EXECUTABLE_IMAGE = 0x0002;
constexpr std::uint16_t IMAGE_FILE_LARGE_ADDRESS_AWARE = 0x0020;
constexpr std::uint16_t IMAGE_NT_OPTIONAL_HDR64_MAGIC = 0x020B;
constexpr std::uint16_t IMAGE_SUBSYSTEM_WINDOWS_CUI = 3;
// DllCharacteristics: no relocations needed -> omit DYNAMIC_BASE so the loader
// honors our preferred ImageBase and our pre-resolved absolute fixups hold.
constexpr std::uint16_t IMAGE_DLLCHARACTERISTICS_NX_COMPAT = 0x0100;

constexpr std::uint32_t IMAGE_SCN_CNT_CODE = 0x00000020;
constexpr std::uint32_t IMAGE_SCN_CNT_INITIALIZED_DATA = 0x00000040;
constexpr std::uint32_t IMAGE_SCN_CNT_UNINITIALIZED_DATA = 0x00000080;
constexpr std::uint32_t IMAGE_SCN_MEM_EXECUTE = 0x20000000;
constexpr std::uint32_t IMAGE_SCN_MEM_READ = 0x40000000;
constexpr std::uint32_t IMAGE_SCN_MEM_WRITE = 0x80000000;

constexpr std::uint64_t kImageBase = 0x140000000ULL;
constexpr std::uint32_t kSectionAlign = 0x1000;
constexpr std::uint32_t kFileAlign = 0x200;

std::uint32_t alignUp(std::uint32_t v, std::uint32_t a) {
    return (v + a - 1) & ~(a - 1);
}

// little-endian writers into a byte buffer at an offset
void w16(std::vector<std::uint8_t>& b, std::size_t off, std::uint16_t v) {
    b[off] = v & 0xFF; b[off + 1] = (v >> 8) & 0xFF;
}
void w32(std::vector<std::uint8_t>& b, std::size_t off, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[off + i] = (v >> (8 * i)) & 0xFF;
}
void w64(std::vector<std::uint8_t>& b, std::size_t off, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b[off + i] = (v >> (8 * i)) & 0xFF;
}
// little-endian appenders
void a16(std::vector<std::uint8_t>& b, std::uint16_t v) {
    b.push_back(v & 0xFF); b.push_back((v >> 8) & 0xFF);
}
void a32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back((v >> (8 * i)) & 0xFF);
}
void a64(std::vector<std::uint8_t>& b, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back((v >> (8 * i)) & 0xFF);
}

struct OutSec {
    const char* name;
    std::uint32_t characteristics;
    std::uint32_t rva = 0;       // virtual address (RVA)
    std::uint32_t vsize = 0;     // virtual size (unpadded)
    std::uint32_t fileOff = 0;   // pointer to raw data
    std::uint32_t fileSize = 0;  // size of raw data (file-aligned)
    std::vector<std::uint8_t> data;  // raw bytes (empty for .bss)
};

}  // namespace

bool PeWriter::write(const MachineCode& code, const std::string& path,
                     const std::string& entrySymbol, std::string& errorOut,
                     std::uint16_t subsystem) {
    // --- locate the entry symbol --------------------------------------------
    std::int64_t entryIdx = code.findSymbol(entrySymbol);
    if (entryIdx < 0 || !code.symbols[entryIdx].defined ||
        code.symbols[entryIdx].section != SectionKind::Text) {
        errorOut = "PE writer: entry symbol '" + entrySymbol + "' is not a defined function";
        return false;
    }

    // --- gather DLL imports --------------------------------------------------
    // Group imported symbols by DLL, preserving first-seen order. Each import
    // gets an IAT slot; we record its symbol index so relocations resolve.
    struct Import { std::uint32_t symIdx; std::string name; };
    std::vector<std::string> dllOrder;
    std::map<std::string, std::vector<Import>> byDll;
    for (std::uint32_t i = 0; i < code.symbols.size(); ++i) {
        const Symbol& s = code.symbols[i];
        if (s.dll.empty()) continue;
        if (byDll.find(s.dll) == byDll.end()) dllOrder.push_back(s.dll);
        byDll[s.dll].push_back(Import{i, s.name});
    }

    // --- build the .text / .data / .bss section buffers ----------------------
    // Section RVAs are assigned in order; .rdata is built last because it hosts
    // the import directory + IAT whose contents depend on RVAs.
    OutSec text{".text", IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ};
    text.data = code.text.bytes;

    // Named code sections from `[section("name")]`. Each becomes its own
    // executable section placed right after .text. We keep stable storage in a
    // deque-like vector and index it by name for symbol/relocation resolution.
    std::vector<OutSec> extra;
    extra.reserve(code.extraText.size());
    std::vector<std::string> extraNames;  // owns the name bytes for OutSec::name
    extraNames.reserve(code.extraText.size());
    for (const Section& es : code.extraText) {
        extraNames.push_back(es.name);
    }
    for (std::size_t i = 0; i < code.extraText.size(); ++i) {
        OutSec s{extraNames[i].c_str(),
                 IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ};
        s.data = code.extraText[i].bytes;
        extra.push_back(std::move(s));
    }
    auto extraByName = [&](const std::string& name) -> OutSec* {
        for (std::size_t i = 0; i < code.extraText.size(); ++i) {
            if (code.extraText[i].name == name) return &extra[i];
        }
        return nullptr;
    };

    OutSec rdata{".rdata", IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ};
    OutSec data{".data", IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE};
    data.data = code.data.bytes;
    OutSec bss{".bss", IMAGE_SCN_CNT_UNINITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE};

    // .rdata begins with the original rodata bytes; the import blob is appended
    // after, so we must know rodata's size before assigning RVAs. We assemble
    // .rdata content in two stages: (1) rodata, (2) import directory + tables.
    rdata.data = code.rodata.bytes;
    // rodataSize = offset within .rdata where the import blob starts (after
    // 8-byte alignment for the IAT entries).
    std::uint32_t importBlobStart = alignUp(static_cast<std::uint32_t>(rdata.data.size()), 16);
    rdata.data.resize(importBlobStart, 0);

    // --- assign section RVAs/file offsets ------------------------------------
    // Header size: DOS(64) + stub(?), we use a fixed 0x40 DOS header pointing PE
    // at 0x40, then NT headers + section headers, padded to file alignment.
    std::vector<OutSec*> secs;
    secs.push_back(&text);
    for (auto& s : extra) secs.push_back(&s);
    secs.push_back(&rdata);
    if (!data.data.empty()) secs.push_back(&data);
    if (code.bss.bssSize > 0) { bss.vsize = static_cast<std::uint32_t>(code.bss.bssSize); secs.push_back(&bss); }

    const std::uint32_t numSecs = static_cast<std::uint32_t>(secs.size());
    const std::uint32_t kDosSize = 0x40;
    const std::uint32_t kNtHeaders = 4 /*sig*/ + 20 /*file hdr*/ + 240 /*opt hdr64*/;
    const std::uint32_t kSecHdr = 40;
    std::uint32_t headersSize = kDosSize + kNtHeaders + kSecHdr * numSecs;
    std::uint32_t sizeOfHeaders = alignUp(headersSize, kFileAlign);

    // Plan: import blob is built into rdata.data now that we can compute RVAs.
    // First assign RVAs assuming rdata.data already has its final size; but the
    // import blob's contents reference RVAs, so we lay out import structures
    // with deterministic sizes first, then fill in addresses.

    // --- compute import blob layout (sizes) ----------------------------------
    // Layout within .rdata (after importBlobStart):
    //   [IAT: per-dll arrays of 8-byte slots, each terminated by a NULL slot]
    //   [Import Directory Table: (numDll+1) IMAGE_IMPORT_DESCRIPTOR (20 bytes)]
    //   [ILT: per-dll arrays of 8-byte entries, NULL-terminated] (mirror of IAT)
    //   [Hint/Name table: IMAGE_IMPORT_BY_NAME per import]
    //   [DLL name strings]
    const std::uint32_t numDll = static_cast<std::uint32_t>(dllOrder.size());

    // IAT: each dll contributes (count+1) slots.
    std::uint32_t iatOff = importBlobStart;  // offset within .rdata
    std::map<std::string, std::uint32_t> dllIatStart;   // .rdata offset of dll's IAT
    std::map<std::uint32_t, std::uint32_t> symIatOff;   // symIdx -> .rdata offset of its slot
    std::uint32_t cur = iatOff;
    for (const auto& dll : dllOrder) {
        dllIatStart[dll] = cur;
        for (const auto& imp : byDll[dll]) {
            symIatOff[imp.symIdx] = cur;
            cur += 8;
        }
        cur += 8;  // NULL terminator slot
    }
    std::uint32_t idtOff = cur;  // import directory table
    std::uint32_t idtSize = (numDll + 1) * 20;
    cur += idtSize;
    std::uint32_t iltOff = cur;  // import lookup table (ILT), mirrors IAT
    std::map<std::string, std::uint32_t> dllIltStart;
    std::map<std::uint32_t, std::uint32_t> symIltOff;
    for (const auto& dll : dllOrder) {
        dllIltStart[dll] = cur;
        for (const auto& imp : byDll[dll]) {
            symIltOff[imp.symIdx] = cur;
            cur += 8;
        }
        cur += 8;
    }
    // Hint/Name entries: 2-byte hint + name + NUL, then 2-byte aligned.
    std::map<std::uint32_t, std::uint32_t> symHintNameOff;  // symIdx -> .rdata offset
    for (const auto& dll : dllOrder) {
        for (const auto& imp : byDll[dll]) {
            symHintNameOff[imp.symIdx] = cur;
            std::uint32_t entry = 2 + static_cast<std::uint32_t>(imp.name.size()) + 1;
            entry = alignUp(entry, 2);
            cur += entry;
        }
    }
    // DLL name strings.
    std::map<std::string, std::uint32_t> dllNameOff;
    for (const auto& dll : dllOrder) {
        dllNameOff[dll] = cur;
        cur += static_cast<std::uint32_t>(dll.size()) + 1;
    }
    std::uint32_t importBlobEnd = cur;

    // Grow .rdata to hold the whole import blob.
    rdata.data.resize(importBlobEnd, 0);

    // --- now assign final RVAs and file offsets ------------------------------
    std::uint32_t rva = alignUp(sizeOfHeaders, kSectionAlign);
    std::uint32_t fileCur = sizeOfHeaders;
    for (OutSec* s : secs) {
        s->rva = rva;
        if (s == &bss) {  // .bss occupies virtual space only
            s->vsize = static_cast<std::uint32_t>(code.bss.bssSize);
            s->fileOff = 0;
            s->fileSize = 0;
        } else {
            s->vsize = static_cast<std::uint32_t>(s->data.size());
            s->fileOff = fileCur;
            s->fileSize = alignUp(s->vsize, kFileAlign);
            fileCur += s->fileSize;
        }
        rva += alignUp(std::max<std::uint32_t>(s->vsize, 1), kSectionAlign);
    }

    auto rvaOf = [&](SectionKind k, std::uint64_t off) -> std::uint32_t {
        switch (k) {
            case SectionKind::Text: return text.rva + static_cast<std::uint32_t>(off);
            case SectionKind::RoData: return rdata.rva + static_cast<std::uint32_t>(off);
            case SectionKind::Data: return data.rva + static_cast<std::uint32_t>(off);
            case SectionKind::Bss: return bss.rva + static_cast<std::uint32_t>(off);
        }
        return 0;
    };
    // RVA of a defined symbol, accounting for named code sections.
    auto symRva = [&](const Symbol& s) -> std::uint32_t {
        if (!s.customSection.empty()) {
            OutSec* es = extraByName(s.customSection);
            return es ? es->rva + static_cast<std::uint32_t>(s.offset) : 0;
        }
        return rvaOf(s.section, s.offset);
    };

    // --- fill the import blob now that RVAs are known ------------------------
    // IAT + ILT entries both point at the hint/name RVA (loader overwrites IAT
    // with the resolved address at load time).
    for (const auto& dll : dllOrder) {
        for (const auto& imp : byDll[dll]) {
            std::uint64_t hnRva = rdata.rva + symHintNameOff[imp.symIdx];
            w64(rdata.data, symIatOff[imp.symIdx], hnRva);
            w64(rdata.data, symIltOff[imp.symIdx], hnRva);
        }
    }
    // Import directory descriptors.
    {
        std::uint32_t d = idtOff;
        for (const auto& dll : dllOrder) {
            w32(rdata.data, d + 0, rdata.rva + dllIltStart[dll]);   // OriginalFirstThunk (ILT)
            w32(rdata.data, d + 4, 0);                              // TimeDateStamp
            w32(rdata.data, d + 8, 0);                              // ForwarderChain
            w32(rdata.data, d + 12, rdata.rva + dllNameOff[dll]);   // Name
            w32(rdata.data, d + 16, rdata.rva + dllIatStart[dll]);  // FirstThunk (IAT)
            d += 20;
        }
        // Null terminator descriptor already zero.
    }
    // Hint/Name entries.
    for (const auto& dll : dllOrder) {
        for (const auto& imp : byDll[dll]) {
            std::uint32_t o = symHintNameOff[imp.symIdx];
            w16(rdata.data, o, 0);  // hint
            std::memcpy(rdata.data.data() + o + 2, imp.name.data(), imp.name.size());
            // trailing NUL already zero
        }
    }
    // DLL name strings.
    for (const auto& dll : dllOrder) {
        std::uint32_t o = dllNameOff[dll];
        std::memcpy(rdata.data.data() + o, dll.data(), dll.size());
    }

    // --- resolve relocations into the section bytes --------------------------
    for (const auto& r : code.relocations) {
        if (r.symbol >= code.symbols.size()) {
            errorOut = "PE writer: relocation references out-of-range symbol";
            return false;
        }
        const Symbol& tgt = code.symbols[r.symbol];
        // The section bytes we patch.
        std::vector<std::uint8_t>* buf = nullptr;
        std::uint32_t siteRva = 0;
        if (!r.customSection.empty()) {
            OutSec* es = extraByName(r.customSection);
            if (!es) { errorOut = "PE writer: relocation in unknown section '" +
                                  r.customSection + "'"; return false; }
            buf = &es->data;
            siteRva = es->rva + static_cast<std::uint32_t>(r.offset);
        } else {
            switch (r.section) {
                case SectionKind::Text: buf = &text.data; siteRva = text.rva + static_cast<std::uint32_t>(r.offset); break;
                case SectionKind::RoData: buf = &rdata.data; siteRva = rdata.rva + static_cast<std::uint32_t>(r.offset); break;
                case SectionKind::Data: buf = &data.data; siteRva = data.rva + static_cast<std::uint32_t>(r.offset); break;
                default: errorOut = "PE writer: relocation in unsupported section"; return false;
            }
        }
        if (r.kind == RelocKind::ImportCall32) {
            auto it = symIatOff.find(r.symbol);
            if (it == symIatOff.end()) {
                errorOut = "PE writer: ImportCall32 against non-import symbol '" + tgt.name + "'";
                return false;
            }
            std::uint32_t iatRva = rdata.rva + it->second;
            std::int32_t disp = static_cast<std::int32_t>(
                static_cast<std::int64_t>(iatRva) - (static_cast<std::int64_t>(siteRva) + 4));
            w32(*buf, r.offset, static_cast<std::uint32_t>(disp));
            continue;
        }
        // All other relocations target a defined symbol's RVA.
        if (!tgt.defined) {
            errorOut = "PE writer: unresolved external symbol '" + tgt.name +
                       "' (no DLL import declared)";
            return false;
        }
        std::uint32_t tgtRva = symRva(tgt) + static_cast<std::uint32_t>(r.addend);
        switch (r.kind) {
            case RelocKind::Rel32:
            case RelocKind::RipData32: {
                std::int32_t disp = static_cast<std::int32_t>(
                    static_cast<std::int64_t>(tgtRva) - (static_cast<std::int64_t>(siteRva) + 4));
                w32(*buf, r.offset, static_cast<std::uint32_t>(disp));
                break;
            }
            case RelocKind::Abs64:
                w64(*buf, r.offset, kImageBase + tgtRva);
                break;
            case RelocKind::Addr32:
                w32(*buf, r.offset, static_cast<std::uint32_t>(kImageBase + tgtRva));
                break;
            case RelocKind::Addr32NB:
                w32(*buf, r.offset, tgtRva);
                break;
            default:
                errorOut = "PE writer: unsupported relocation kind";
                return false;
        }
    }

    // --- compute image size and entry ----------------------------------------
    std::uint32_t sizeOfImage = rva;  // rva has advanced past the last section
    std::uint32_t entryRva = text.rva + static_cast<std::uint32_t>(code.symbols[entryIdx].offset);
    std::uint32_t sizeOfCode = alignUp(static_cast<std::uint32_t>(text.data.size()), kFileAlign);

    std::uint32_t importDirRva = numDll ? (rdata.rva + idtOff) : 0;
    std::uint32_t importDirSize = numDll ? idtSize : 0;
    std::uint32_t iatDirRva = numDll ? (rdata.rva + iatOff) : 0;
    std::uint32_t iatDirSize = numDll ? (idtOff - iatOff) : 0;

    // --- emit the file -------------------------------------------------------
    std::vector<std::uint8_t> out;
    out.resize(sizeOfHeaders, 0);

    // DOS header: "MZ", e_lfanew at 0x3C -> 0x40.
    w16(out, 0, IMAGE_DOS_SIGNATURE);
    w32(out, 0x3C, kDosSize);

    std::size_t p = kDosSize;
    // PE signature
    w32(out, p, IMAGE_NT_SIGNATURE); p += 4;
    // IMAGE_FILE_HEADER
    w16(out, p + 0, IMAGE_FILE_MACHINE_AMD64);
    w16(out, p + 2, static_cast<std::uint16_t>(numSecs));
    w32(out, p + 4, 0);   // TimeDateStamp
    w32(out, p + 8, 0);   // PointerToSymbolTable
    w32(out, p + 12, 0);  // NumberOfSymbols
    w16(out, p + 16, 240);  // SizeOfOptionalHeader
    w16(out, p + 18, IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE);
    p += 20;
    // IMAGE_OPTIONAL_HEADER64
    std::size_t opt = p;
    w16(out, opt + 0, IMAGE_NT_OPTIONAL_HDR64_MAGIC);
    out[opt + 2] = 14;  // MajorLinkerVersion
    out[opt + 3] = 0;
    w32(out, opt + 4, sizeOfCode);
    w32(out, opt + 8, alignUp(static_cast<std::uint32_t>(rdata.data.size()), kFileAlign) +
                          alignUp(static_cast<std::uint32_t>(data.data.size()), kFileAlign));  // SizeOfInitializedData
    w32(out, opt + 12, 0);  // SizeOfUninitializedData
    w32(out, opt + 16, entryRva);  // AddressOfEntryPoint
    w32(out, opt + 20, text.rva);  // BaseOfCode
    w64(out, opt + 24, kImageBase);  // ImageBase
    w32(out, opt + 32, kSectionAlign);
    w32(out, opt + 36, kFileAlign);
    w16(out, opt + 40, 6);  // MajorOperatingSystemVersion
    w16(out, opt + 42, 0);
    w16(out, opt + 44, 0);  // MajorImageVersion
    w16(out, opt + 46, 0);
    w16(out, opt + 48, 6);  // MajorSubsystemVersion
    w16(out, opt + 50, 0);
    w32(out, opt + 52, 0);  // Win32VersionValue
    w32(out, opt + 56, sizeOfImage);
    w32(out, opt + 60, sizeOfHeaders);
    w32(out, opt + 64, 0);  // CheckSum
    w16(out, opt + 68, subsystem);
    w16(out, opt + 70, IMAGE_DLLCHARACTERISTICS_NX_COMPAT);
    w64(out, opt + 72, 0x100000);  // SizeOfStackReserve
    w64(out, opt + 80, 0x1000);    // SizeOfStackCommit
    w64(out, opt + 88, 0x100000);  // SizeOfHeapReserve
    w64(out, opt + 96, 0x1000);    // SizeOfHeapCommit
    w32(out, opt + 104, 0);  // LoaderFlags
    w32(out, opt + 108, 16); // NumberOfRvaAndSizes
    // Data directories (16 entries, 8 bytes each) starting at opt+112.
    std::size_t dd = opt + 112;
    // [1] Import directory
    w32(out, dd + 1 * 8 + 0, importDirRva);
    w32(out, dd + 1 * 8 + 4, importDirSize);
    // [12] IAT directory
    w32(out, dd + 12 * 8 + 0, iatDirRva);
    w32(out, dd + 12 * 8 + 4, iatDirSize);
    p = opt + 240;

    // Section headers
    for (OutSec* s : secs) {
        std::uint8_t name[8] = {0};
        std::memcpy(name, s->name, std::strlen(s->name));
        for (int i = 0; i < 8; ++i) out[p + i] = name[i];
        w32(out, p + 8, s->vsize);                 // VirtualSize
        w32(out, p + 12, s->rva);                  // VirtualAddress
        w32(out, p + 16, s->fileSize);             // SizeOfRawData
        w32(out, p + 20, s->fileOff);              // PointerToRawData
        w32(out, p + 24, 0);                       // PointerToRelocations
        w32(out, p + 28, 0);                       // PointerToLinenumbers
        w16(out, p + 32, 0);                       // NumberOfRelocations
        w16(out, p + 34, 0);                       // NumberOfLinenumbers
        w32(out, p + 36, s->characteristics);
        p += 40;
    }

    // Raw section data, each padded to file alignment.
    for (OutSec* s : secs) {
        if (s->fileSize == 0) continue;  // .bss
        std::size_t start = out.size();
        out.resize(start + s->fileSize, 0);
        std::memcpy(out.data() + start, s->data.data(), s->data.size());
    }

    // --- write to disk -------------------------------------------------------
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) { errorOut = "PE writer: cannot open '" + path + "'"; return false; }
    f.write(reinterpret_cast<const char*>(out.data()),
            static_cast<std::streamsize>(out.size()));
    if (!f.good()) { errorOut = "PE writer: failed writing '" + path + "'"; return false; }
    return true;
}

}  // namespace Backend
