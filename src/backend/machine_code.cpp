#include <backend/machine_code.hpp>

namespace Backend {

Section& MachineCode::sectionFor(SectionKind kind) {
    switch (kind) {
        case SectionKind::Text: return text;
        case SectionKind::Data: return data;
        case SectionKind::RoData: return rodata;
        case SectionKind::Bss: return bss;
    }
    return text;
}

const Section& MachineCode::sectionFor(SectionKind kind) const {
    switch (kind) {
        case SectionKind::Text: return text;
        case SectionKind::Data: return data;
        case SectionKind::RoData: return rodata;
        case SectionKind::Bss: return bss;
    }
    return text;
}

std::uint32_t MachineCode::defineSymbol(const std::string& name, SectionKind section,
                                        std::uint64_t offset, SymbolBinding binding,
                                        bool isFunction) {
    // If the name was previously referenced as external, promote it in place.
    std::int64_t existing = findSymbol(name);
    if (existing >= 0 && !symbols[existing].defined) {
        Symbol& sym = symbols[existing];
        sym.binding = binding;
        sym.section = section;
        sym.offset = offset;
        sym.isFunction = isFunction;
        sym.defined = true;
        return static_cast<std::uint32_t>(existing);
    }

    Symbol sym;
    sym.name = name;
    sym.binding = binding;
    sym.section = section;
    sym.offset = offset;
    sym.isFunction = isFunction;
    sym.defined = true;
    symbols.push_back(std::move(sym));
    return static_cast<std::uint32_t>(symbols.size() - 1);
}

std::uint32_t MachineCode::defineSymbolInSection(const std::string& name,
                                                 const std::string& customSection,
                                                 std::uint64_t offset,
                                                 SymbolBinding binding,
                                                 bool isFunction) {
    std::uint32_t idx = defineSymbol(name, SectionKind::Text, offset, binding, isFunction);
    symbols[idx].customSection = customSection;
    return idx;
}

std::uint32_t MachineCode::referenceExternal(const std::string& name) {    std::int64_t existing = findSymbol(name);
    if (existing >= 0) {
        return static_cast<std::uint32_t>(existing);
    }
    Symbol sym;
    sym.name = name;
    sym.binding = SymbolBinding::External;
    sym.defined = false;
    symbols.push_back(std::move(sym));
    return static_cast<std::uint32_t>(symbols.size() - 1);
}

std::uint32_t MachineCode::referenceImport(const std::string& name,
                                           const std::string& dll) {
    std::int64_t existing = findSymbol(name);
    if (existing >= 0) {
        // Attach/refresh the DLL on an existing reference (e.g. promoted from a
        // plain external reference created by an earlier call site).
        symbols[existing].dll = dll;
        symbols[existing].isFunction = true;
        return static_cast<std::uint32_t>(existing);
    }
    Symbol sym;
    sym.name = name;
    sym.binding = SymbolBinding::External;
    sym.defined = false;
    sym.isFunction = true;
    sym.dll = dll;
    symbols.push_back(std::move(sym));
    return static_cast<std::uint32_t>(symbols.size() - 1);
}

std::int64_t MachineCode::findSymbol(const std::string& name) const {
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        if (symbols[i].name == name) {
            return static_cast<std::int64_t>(i);
        }
    }
    return -1;
}

void MachineCode::addRelocation(SectionKind section, std::uint64_t offset,
                                std::uint32_t symbol, RelocKind kind,
                                std::int64_t addend) {
    Relocation reloc;
    reloc.section = section;
    reloc.offset = offset;
    reloc.symbol = symbol;
    reloc.kind = kind;
    reloc.addend = addend;
    relocations.push_back(reloc);
}

void MachineCode::addRelocationInSection(const std::string& customSection,
                                         std::uint64_t offset, std::uint32_t symbol,
                                         RelocKind kind, std::int64_t addend) {
    Relocation reloc;
    reloc.section = SectionKind::Text;
    reloc.customSection = customSection;
    reloc.offset = offset;
    reloc.symbol = symbol;
    reloc.kind = kind;
    reloc.addend = addend;
    relocations.push_back(reloc);
}

}  // namespace Backend
