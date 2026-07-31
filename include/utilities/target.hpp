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
    Wasm32,
    Custom
};

struct TargetSpec {
    TargetKind kind = TargetKind::Custom;
    std::string cliName;
    std::string arch;
    std::string vendor;
    std::string os;
    std::string abi;
    std::string triple;
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
    // WebAssembly. Unlike every other supported target this is not an x86-64
    // machine: it has no physical registers, no addressable native stack, and
    // only structured control flow, so it bypasses regalloc/lowering entirely.
    bool isWasm = false;
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

    // Note: a wasm target is deliberately none of isElf/isCoff/isMachO, so any
    // container dispatch that forgets wasm falls through to ELF. Test isWasm()
    // *before* those predicates.
    bool isWasmModule() const {
        return isWasm || objectFormat == "wasm";
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
                                 std::string triple,
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
    spec.triple = std::move(triple);
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
    static const TargetSpec x86_64InstantOSSpec = []() {
        TargetSpec spec = makeTargetSpec(
            TargetKind::X86_64InstantOS, "x86_64_instantos", "x86_64", "unknown", "instantos", "instantos",
            "x86_64-unknown-elf", "elf", "executable", 64, "little", false, "_start",
            false, false, false, true, false, false);
        // InstantOS userland binaries are dynamically-linked PIEs that run on top
        // of mlibc + the ld-instantos.so runtime loader. The program supplies
        // `main`; crt1.o (from the sysroot) provides `_start`. The dynamic linker
        // path is embedded as PT_INTERP. The sysroot default points at the
        // mlibc-root produced by InstantOS's tools/build-mlibc.sh; override with
        // --sysroot.
        spec.dynamicLinker = "/lib/mlibc/ld-instantos.so";
        spec.sysroot = "C:/Users/Administrator/projects/InstantOS/.bash-cache/mlibc-root";
        return spec;
    }();
    static const TargetSpec wasm32Spec = []() {
        // WASI preview1 "command" module: a self-contained .wasm exporting
        // `_start` (()->()) and `memory`, importing its OS surface from the
        // "wasi_snapshot_preview1" module. No linker is involved -- a wasm
        // module is already self-contained, so the backend writes the final
        // artifact directly (as it already does for whole-program PE).
        //
        // pointerWidth is recorded truthfully as 32, but codegen does not yet
        // consult it: the backend keeps its 64-bit address model and wraps to
        // i32 at each memory access. Making this field authoritative is a
        // separate change across isel's widthOf/sizeOf/fieldOffsetOf.
        TargetSpec spec = makeTargetSpec(
            TargetKind::Wasm32, "wasm32_wasi", "wasm32", "unknown", "wasi", "",
            "wasm32-unknown-wasi", "wasm", "wasm", 32, "little", false, "_start",
            false, false, false, false, false, false);
        spec.isWasm = true;
        return spec;
    }();

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
        case TargetKind::Wasm32:
            return wasm32Spec;
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
    if (cliName == "wasm32_wasi" || cliName == "wasm32-wasi" || cliName == "wasi" ||
        cliName == "wasm32" || cliName == "wasm") {
        return targetSpecForKind(TargetKind::Wasm32);
    }
    // wasm32-unknown-unknown: no WASI imports, no host runtime. A bare module
    // that exports its public functions; the embedder supplies everything.
    if (cliName == "wasm32_freestanding" || cliName == "wasm32-freestanding" ||
        cliName == "wasm32_unknown" || cliName == "wasm32-unknown-unknown") {
        TargetSpec spec = targetSpecForKind(TargetKind::Wasm32);
        spec.cliName = "wasm32_freestanding";
        spec.os = "unknown";
        spec.triple = "wasm32-unknown-unknown";
        spec.entrySymbol.clear();
        spec.freestandingExecutable = true;
        return spec;
    }
    return std::nullopt;
}

inline std::string supportedTargetList() {
    return "x86_64_linux, x86_64_mac, arm64_mac, x86_64_windows, x86_64_efi, "
           "x86_64_instantos, wasm32_wasi, wasm32_freestanding, or a target spec .toml path";
}

std::optional<TargetSpec> loadTargetSpec(std::string_view cliNameOrPath, std::string& errorMessage);

}
