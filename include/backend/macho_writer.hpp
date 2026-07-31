#pragma once

// Mach-O (x86-64) relocatable object writer. Sibling to ElfWriter / CoffWriter:
// serializes the same MachineCode model into an Apple Mach-O `MH_OBJECT` that
// can be linked by ld64 / lld on macOS. No external dependency: emits the
// Mach-O container (mach_header_64, a single LC_SEGMENT_64 holding the section
// table, LC_SYMTAB, the section bytes, relocation_info records, and the symbol +
// string tables) directly.
//
// macOS on x86-64 uses the System V AMD64 calling convention (so the rest of the
// backend is shared); the differences handled here are the container format, the
// leading-underscore symbol naming convention, and the Mach-O relocation kinds.

#include <string>

#include <backend/machine_code.hpp>

namespace Backend {

class MachOWriter {
public:
    // Serializes `code` to a Mach-O x86-64 relocatable object at `path`. Returns
    // false and sets `errorOut` on I/O failure or an unrepresentable relocation.
    static bool write(const MachineCode& code, const std::string& path,
                      std::string& errorOut);
};

}  // namespace Backend
