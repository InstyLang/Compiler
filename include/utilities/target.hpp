#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Targeting {

enum class TargetKind {
    X86_64Linux,
    X86_64Mac,
    Arm64Mac,
    X86_64Windows,
    X86_64Efi,
    X86_64InstantOS,
    Custom
};

struct TargetSpec {
    TargetKind kind = TargetKind::Custom;
    std::string cliName;
    std::string arch;
    std::string vendor;
    std::string os;
    std::string abi;
    std::string llvmTriple;
    std::string objectFormat = "elf";
    std::string outputFormat = "executable";
    int pointerWidth = 64;
    std::string endian = "little";
    bool disableRedZone = false;
    std::string entrySymbol;
    std::string linkerScript;
    std::string linkerPath;
    std::string sysroot;
    std::string dynamicLinker;
    bool multiboot2 = false;
    bool isWindowsLike = false;
    bool isApple = false;
    bool isEfi = false;
    bool isInstantOS = false;
    bool supportsLinuxSyscalls = false;
    bool freestandingExecutable = false;
    std::string panicStrategy;
    std::string panicHandler;
    bool isCustom = false;

    bool isLinux() const {
        return os == "linux" || kind == TargetKind::X86_64Linux;
    }

    bool isElf() const {
        return objectFormat == "elf";
    }

    bool isMachO() const {
        return objectFormat == "mach-o" || objectFormat == "macho" || isApple;
    }

    bool isCoff() const {
        return objectFormat == "coff" || isWindowsLike;
    }

    bool supportsLinuxSyscallRuntime() const {
        return supportsLinuxSyscalls;
    }

    bool isFreestandingExecutable() const {
        return freestandingExecutable;
    }
};

inline const TargetSpec& targetSpecForKind(TargetKind kind);

inline TargetSpec makeTargetSpec(TargetKind kind,
                                 std::string cliName,
                                 std::string arch,
                                 std::string vendor,
                                 std::string os,
                                 std::string abi,
                                 std::string llvmTriple,
                                 std::string objectFormat,
                                 std::string outputFormat,
                                 int pointerWidth,
                                 std::string endian,
                                 bool disableRedZone,
                                 std::string entrySymbol,
                                 bool isWindowsLike,
                                 bool isApple,
                                 bool isEfi,
                                 bool isInstantOS,
                                 bool supportsLinuxSyscalls,
                                 bool freestandingExecutable) {
    TargetSpec spec;
    spec.kind = kind;
    spec.cliName = std::move(cliName);
    spec.arch = std::move(arch);
    spec.vendor = std::move(vendor);
    spec.os = std::move(os);
    spec.abi = std::move(abi);
    spec.llvmTriple = std::move(llvmTriple);
    spec.objectFormat = std::move(objectFormat);
    spec.outputFormat = std::move(outputFormat);
    spec.pointerWidth = pointerWidth;
    spec.endian = std::move(endian);
    spec.disableRedZone = disableRedZone;
    spec.entrySymbol = std::move(entrySymbol);
    spec.isWindowsLike = isWindowsLike;
    spec.isApple = isApple;
    spec.isEfi = isEfi;
    spec.isInstantOS = isInstantOS;
    spec.supportsLinuxSyscalls = supportsLinuxSyscalls;
    spec.freestandingExecutable = freestandingExecutable;
    return spec;
}

inline const TargetSpec& defaultTargetSpec() {
    return targetSpecForKind(TargetKind::X86_64Linux);
}

inline const TargetSpec& targetSpecForKind(TargetKind kind) {
    static const TargetSpec linuxSpec = makeTargetSpec(
        TargetKind::X86_64Linux, "x86_64_linux", "x86_64", "pc", "linux", "gnu",
        "x86_64-pc-linux-gnu", "elf", "executable", 64, "little", false, "_start",
        false, false, false, false, true, true);
    static const TargetSpec x86_64MacSpec = makeTargetSpec(
        TargetKind::X86_64Mac, "x86_64_mac", "x86_64", "apple", "macos", "",
        "x86_64-apple-macosx11.0.0", "mach-o", "executable", 64, "little", false, "_main",
        false, true, false, false, false, false);
    static const TargetSpec arm64MacSpec = makeTargetSpec(
        TargetKind::Arm64Mac, "arm64_mac", "arm64", "apple", "macos", "",
        "arm64-apple-macosx11.0.0", "mach-o", "executable", 64, "little", false, "_main",
        false, true, false, false, false, false);
    static const TargetSpec x86_64WindowsSpec = makeTargetSpec(
        TargetKind::X86_64Windows, "x86_64_windows", "x86_64", "pc", "windows", "msvc",
        "x86_64-pc-windows-msvc", "coff", "pe", 64, "little", false, "main",
        true, false, false, false, false, false);
    static const TargetSpec x86_64EfiSpec = makeTargetSpec(
        TargetKind::X86_64Efi, "x86_64_efi", "x86_64", "pc", "uefi", "efi",
        "x86_64-pc-win32-coff", "coff", "uefi", 64, "little", false, "efi_main",
        true, false, true, false, false, true);
    static const TargetSpec x86_64InstantOSSpec = makeTargetSpec(
        TargetKind::X86_64InstantOS, "x86_64_instantos", "x86_64", "unknown", "windows", "instantos",
        "x86_64-unknown-windows", "coff", "pe", 64, "little", false, "userland_entry",
        true, false, false, true, false, true);

    switch (kind) {
        case TargetKind::X86_64Linux:
            return linuxSpec;
        case TargetKind::X86_64Mac:
            return x86_64MacSpec;
        case TargetKind::Arm64Mac:
            return arm64MacSpec;
        case TargetKind::X86_64Windows:
            return x86_64WindowsSpec;
        case TargetKind::X86_64Efi:
            return x86_64EfiSpec;
        case TargetKind::X86_64InstantOS:
            return x86_64InstantOSSpec;
        case TargetKind::Custom:
            return linuxSpec;
    }

    return linuxSpec;
}

inline std::optional<TargetSpec> parseTargetSpec(std::string_view cliName) {
    if (cliName == "x86_64_linux") {
        return targetSpecForKind(TargetKind::X86_64Linux);
    }
    if (cliName == "x86_64_mac") {
        return targetSpecForKind(TargetKind::X86_64Mac);
    }
    if (cliName == "arm64_mac") {
        return targetSpecForKind(TargetKind::Arm64Mac);
    }
    if (cliName == "x86_64_windows") {
        return targetSpecForKind(TargetKind::X86_64Windows);
    }
    if (cliName == "x86_64_efi") {
        return targetSpecForKind(TargetKind::X86_64Efi);
    }
    if (cliName == "x86_64_instantos" || cliName == "instant_os" || cliName == "instantos") {
        return targetSpecForKind(TargetKind::X86_64InstantOS);
    }
    return std::nullopt;
}

inline std::string supportedTargetList() {
    return "x86_64_linux, x86_64_mac, arm64_mac, x86_64_windows, x86_64_efi, x86_64_instantos, or a target spec .toml path";
}

std::optional<TargetSpec> loadTargetSpec(std::string_view cliNameOrPath, std::string& errorMessage);

}
