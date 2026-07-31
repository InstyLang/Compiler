#pragma once

// COFF (x86-64) relocatable object writer. Serializes a MachineCode into a
// .obj that can be linked by lld-link / link.exe. No external dependency:
// emits the COFF container (header, section headers, raw data, relocations,
// symbol table, string table) directly.

#include <string>

#include <backend/machine_code.hpp>

namespace Backend {

class CoffWriter {
public:
    // Serializes `code` to a COFF object file at `path`. Returns false and
    // sets `errorOut` on I/O failure or an unrepresentable relocation.
    static bool write(const MachineCode& code, const std::string& path,
                      std::string& errorOut);
};

}  // namespace Backend
