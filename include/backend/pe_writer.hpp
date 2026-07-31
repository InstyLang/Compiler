#pragma once

// PE32+ (x86-64) executable writer. Serializes a MachineCode directly into a
// runnable Windows .exe -- no external linker. Builds the DOS/NT headers, the
// section table (.text/.rdata/.data/.bss), and, for any DLL-imported symbols,
// the import directory + Import Address Table (IAT). All relocations are
// resolved in-image against the chosen ImageBase, so the result needs no
// further fixups (the file has no .reloc and is marked as a fixed-base image).

#include <cstdint>
#include <string>

#include <backend/machine_code.hpp>

namespace Backend {

class PeWriter {
public:
    // Subsystem values (IMAGE_SUBSYSTEM_*) for the PE optional header.
    static constexpr std::uint16_t kSubsystemConsole = 3;       // WINDOWS_CUI
    static constexpr std::uint16_t kSubsystemEfiApplication = 10;  // EFI_APPLICATION

    // Serializes `code` to a PE executable at `path`, using `entrySymbol` as the
    // image entry point (must be a defined function symbol, e.g. "main" or the
    // synthesized "_start"/"efi_main"). `subsystem` selects the PE subsystem
    // (default: a console application; pass kSubsystemEfiApplication for UEFI).
    // Returns false and sets `errorOut` on failure (missing entry, I/O error,
    // or an unrepresentable relocation).
    static bool write(const MachineCode& code, const std::string& path,
                      const std::string& entrySymbol, std::string& errorOut,
                      std::uint16_t subsystem = kSubsystemConsole);
};

}  // namespace Backend
