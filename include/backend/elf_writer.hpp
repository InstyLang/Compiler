#pragma once

// ELF64 (x86-64) relocatable object writer. Sibling to CoffWriter: serializes
// the same MachineCode model into a System V ELF `.o` that can be linked by
// ld / ld.lld / gcc / clang. No external dependency: emits the ELF container
// (header, section headers, raw data, RELA relocations, symbol table, string
// tables) directly.

#include <string>

#include <backend/machine_code.hpp>

namespace Backend {

class ElfWriter {
public:
    // Serializes `code` to an ELF64 relocatable object at `path`. Returns false
    // and sets `errorOut` on I/O failure or an unrepresentable relocation.
    static bool write(const MachineCode& code, const std::string& path,
                      std::string& errorOut);
};

}  // namespace Backend
