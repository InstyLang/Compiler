#pragma once

// LLVM-free machine-code model for the custom backend.
//
// This is the hand-off boundary between code generation (which emits encoded
// instruction bytes via the Fadec wrapper) and the object writers (COFF now,
// ELF later). It deliberately has no dependency on LLVM or any object-format
// library: a backend builds up `Section`s of bytes, declares `Symbol`s, and
// records `Relocation`s, then an object writer serializes the whole thing.

#include <cstdint>
#include <string>
#include <vector>

namespace Backend {

// What a symbol's address ultimately refers to / how the linker treats it.
enum class SymbolBinding {
    Local,    // file-local (not exported), e.g. a static helper
    Global,   // defined here and visible to other objects (e.g. `main`)
    Weak,     // defined here but overridable; a strong definition elsewhere wins
    // Defined here and in every other object that needs it, all copies identical
    // -- a monomorphized generic, say. The linker keeps one and drops the rest.
    // Distinct from Weak: this does not mean "overridable", and a strong
    // definition of the same name is a conflict rather than a winner.
    LinkOnce,
    External  // referenced here but defined elsewhere (e.g. an imported fn)
};

// Which section a symbol lives in. External symbols have no section.
//
// Custom-named code sections (from a `[section("name")]` attribute) reuse the
// `Text` kind but carry a non-empty `customSection` name on the Symbol; their
// bytes live in a dedicated entry of MachineCode::extraText keyed by that name.
enum class SectionKind : std::uint8_t {
    Text,  // executable code (.text)
    Data,  // initialized read/write data (.data)
    RoData,  // read-only data (.rdata) - string literals, const tables
    Bss    // zero-initialized data (.bss) - reserves size, stores no bytes
};

struct Symbol {
    std::string name;
    SymbolBinding binding = SymbolBinding::Local;
    // Section/offset are only meaningful for defined (non-External) symbols.
    SectionKind section = SectionKind::Text;
    std::uint64_t offset = 0;  // byte offset within its section
    bool isFunction = false;   // hint for object writers (COFF symbol type)
    bool defined = false;      // false => External (resolved at link time)
    // Custom section name (from `[section("name")]`). When non-empty for a Text
    // symbol, the function lives in the named section instead of `.text`; its
    // bytes are in MachineCode::extraText[customSection].
    std::string customSection;
    // DLL import: when non-empty, this symbol is a function imported from the
    // named DLL via the PE import table. Calls to it go indirectly through its
    // IAT slot (call qword [rip + iat]); the PE writer builds the import
    // descriptor + IAT and resolves ImportCall32 relocations against the slot.
    std::string dll;
};

// How a relocation patches the bytes at `offset`. Names mirror the common
// COFF/ELF relocation semantics so writers can map them to the native types.
enum class RelocKind {
    Abs64,   // 64-bit absolute address of the target symbol (+ addend)
    Rel32,   // 32-bit PC-relative (call/jmp to a function; ELF uses PLT32)
    RipData32, // 32-bit PC-relative RIP-relative data ref (lea reg,[rip+x]; ELF PC32)
    Rel32_1, // PC-relative with -1 bias (rare; provided for completeness)
    Addr32,  // 32-bit absolute (low 32 bits of the address)
    Addr32NB,  // 32-bit address relative to image base (COFF "addr32nb")
    ImportCall32  // 32-bit PC-relative ref to a DLL-import symbol's IAT slot
                  // (call/jmp qword [rip + disp]); resolved only by the PE writer
};

struct Relocation {
    SectionKind section = SectionKind::Text;  // section containing the bytes to patch
    std::string customSection;  // non-empty => a named code section (see extraText)
    std::uint64_t offset = 0;                 // offset within that section
    std::uint32_t symbol = 0;                 // index into MachineCode::symbols
    RelocKind kind = RelocKind::Rel32;
    std::int64_t addend = 0;  // constant folded into the patched value
};

struct Section {
    SectionKind kind = SectionKind::Text;
    std::string name;  // explicit name for custom/named sections ("" => default)
    std::vector<std::uint8_t> bytes;  // empty for Bss; `bssSize` used instead
    std::uint64_t bssSize = 0;        // reserved size for Bss sections
    unsigned alignment = 16;          // required alignment in bytes (power of 2)

    std::uint64_t size() const { return kind == SectionKind::Bss ? bssSize : bytes.size(); }
};

// The complete relocatable translation unit produced by the backend.
class MachineCode {
public:
    Section text{SectionKind::Text};
    Section data{SectionKind::Data};
    Section rodata{SectionKind::RoData};
    Section bss{SectionKind::Bss};

    // Custom-named executable code sections (from `[section("name")]`). Each is a
    // Text-kind Section with an explicit `name`. Functions placed here have their
    // symbol's `customSection` set to the name.
    std::vector<Section> extraText;

    std::vector<Symbol> symbols;
    std::vector<Relocation> relocations;

    Section& sectionFor(SectionKind kind);
    const Section& sectionFor(SectionKind kind) const;

    // The executable code section currently being emitted into. Defaults to the
    // primary `.text`; redirected by the lowering when a function carries a
    // custom section so its bytes/relocations land in the named section. Returns
    // a reference into either `text` or one of `extraText`.
    Section& currentCode() {
        return curCodeIndex_ < 0 ? text : extraText[static_cast<std::size_t>(curCodeIndex_)];
    }
    const std::string& currentCodeName() const { return curCodeName_; }
    // Select the active code section by name ("" => primary .text). Creates the
    // named section on first use.
    void selectCodeSection(const std::string& name) {
        if (name.empty()) { curCodeIndex_ = -1; curCodeName_.clear(); return; }
        for (std::size_t i = 0; i < extraText.size(); ++i) {
            if (extraText[i].name == name) {
                curCodeIndex_ = static_cast<std::int64_t>(i);
                curCodeName_ = name;
                return;
            }
        }
        extraText.push_back(Section{SectionKind::Text, name, {}, 0, 16});
        curCodeIndex_ = static_cast<std::int64_t>(extraText.size() - 1);
        curCodeName_ = name;
    }

    // Declares a defined symbol at the current end of its section. Returns its
    // index in `symbols` (the value used by Relocation::symbol).
    std::uint32_t defineSymbol(const std::string& name, SectionKind section,
                               std::uint64_t offset, SymbolBinding binding,
                               bool isFunction);
    // As above, but places the symbol in a custom-named code section.
    std::uint32_t defineSymbolInSection(const std::string& name,
                                        const std::string& customSection,
                                        std::uint64_t offset, SymbolBinding binding,
                                        bool isFunction);

    // Declares (or returns the existing) external symbol reference.
    std::uint32_t referenceExternal(const std::string& name);

    // Declares (or promotes to) a DLL-imported function symbol. Calls to it are
    // emitted as indirect calls through the IAT (ImportCall32 relocations); the
    // PE writer builds the import table. Returns its index in `symbols`.
    std::uint32_t referenceImport(const std::string& name, const std::string& dll);

    // Finds a symbol by name; returns -1 if absent.
    std::int64_t findSymbol(const std::string& name) const;

    void addRelocation(SectionKind section, std::uint64_t offset,
                       std::uint32_t symbol, RelocKind kind, std::int64_t addend = 0);
    // Records a relocation in a custom-named code section.
    void addRelocationInSection(const std::string& customSection,
                                std::uint64_t offset, std::uint32_t symbol,
                                RelocKind kind, std::int64_t addend = 0);

private:
    std::int64_t curCodeIndex_ = -1;  // active code section (-1 => primary text)
    std::string curCodeName_;         // name of the active code section ("" => text)
};

}  // namespace Backend

