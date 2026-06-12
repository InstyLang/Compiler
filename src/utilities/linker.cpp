#include <utilities/linker.hpp>
#include <utilities/logger.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace Linker {

namespace {

enum class LinkerFlavor {
    Elf,
    MachO,
    Coff
};

LinkerFlavor getLinkerFlavor(const Targeting::TargetSpec& target) {
    if (target.isCoff()) {
        return LinkerFlavor::Coff;
    }
    if (target.isMachO()) {
        return LinkerFlavor::MachO;
    }
    return LinkerFlavor::Elf;
}

std::string trim(std::string value) {
    auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch);
    });
    auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
        return std::isspace(ch);
    }).base();

    if (begin >= end) {
        return "";
    }

    return std::string(begin, end);
}

std::vector<std::string> split(const std::string& value, char delimiter) {
    std::vector<std::string> parts;
    std::stringstream stream(value);
    std::string part;
    while (std::getline(stream, part, delimiter)) {
        part = trim(part);
        if (!part.empty()) {
            parts.push_back(part);
        }
    }
    return parts;
}

std::string quoteForDisplay(const std::string& value) {
    if (value.find_first_of(" \t\n'\"") == std::string::npos) {
        return value;
    }

    std::string quoted = "'";
    for (char ch : value) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted += ch;
        }
    }
    quoted += "'";
    return quoted;
}

std::string formatCommand(const std::vector<std::string>& args) {
    std::string formatted;
    for (size_t index = 0; index < args.size(); ++index) {
        if (index != 0) {
            formatted += " ";
        }
        formatted += quoteForDisplay(args[index]);
    }
    return formatted;
}

bool runProcess(const std::vector<std::string>& args) {
    if (args.empty()) {
        return false;
    }

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const std::string& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        return false;
    }

    if (pid == 0) {
        execvp(argv[0], argv.data());
        perror(argv[0]);
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) {
            perror("waitpid");
            return false;
        }
    }

    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool runProcessCapture(const std::vector<std::string>& args, std::string& output) {
    output.clear();
    if (args.empty()) {
        return false;
    }

    int pipeFds[2];
    if (pipe(pipeFds) == -1) {
        return false;
    }

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const std::string& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid == -1) {
        close(pipeFds[0]);
        close(pipeFds[1]);
        return false;
    }

    if (pid == 0) {
        close(pipeFds[0]);
        dup2(pipeFds[1], STDOUT_FILENO);
        close(pipeFds[1]);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    close(pipeFds[1]);
    char buffer[4096];
    ssize_t readCount = 0;
    while ((readCount = read(pipeFds[0], buffer, sizeof(buffer))) > 0) {
        output.append(buffer, static_cast<size_t>(readCount));
    }
    close(pipeFds[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) {
            return false;
        }
    }

    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

std::string compilerDriver() {
    const char* cc = std::getenv("CC");
    return (cc && *cc) ? std::string(cc) : "cc";
}

std::string toolPathFromEnv(const char* envName, const std::string& fallback) {
    const char* value = std::getenv(envName);
    return (value && *value) ? std::string(value) : fallback;
}

std::string elfEmulationFor(const Targeting::TargetSpec& target) {
    if (target.arch == "x86_64" || target.arch == "amd64") {
        return "elf_x86_64";
    }
    if (target.arch == "i386" || target.arch == "i686" || target.arch == "x86") {
        return "elf_i386";
    }
    if (target.arch == "aarch64" || target.arch == "arm64") {
        return "aarch64elf";
    }
    if (target.arch == "arm") {
        return "armelf";
    }
    if (target.arch == "riscv64") {
        return "elf64lriscv";
    }
    if (target.arch == "riscv32") {
        return "elf32lriscv";
    }
    return "";
}

std::string coffMachineFor(const Targeting::TargetSpec& target) {
    if (target.arch == "aarch64" || target.arch == "arm64") {
        return "arm64";
    }
    if (target.arch == "i386" || target.arch == "i686" || target.arch == "x86") {
        return "x86";
    }
    return "x64";
}

std::string machoArchFor(const Targeting::TargetSpec& target) {
    if (target.arch == "aarch64") {
        return "arm64";
    }
    return target.arch.empty() ? "x86_64" : target.arch;
}

std::string defaultLinkerName(const Targeting::TargetSpec& target) {
    switch (getLinkerFlavor(target)) {
        case LinkerFlavor::Elf:
            return "ld.lld";
        case LinkerFlavor::MachO:
            return "ld64.lld";
        case LinkerFlavor::Coff:
            return "lld-link";
    }
    return "";
}

std::string effectiveLinker(const LinkOptions& options) {
    if (!options.linkerPath.empty()) {
        return options.linkerPath;
    }
    if (!options.target.linkerPath.empty()) {
        return options.target.linkerPath;
    }
    return defaultLinkerName(options.target);
}

std::string effectiveLinkerScript(const LinkOptions& options) {
    return options.linkerScript.empty() ? options.target.linkerScript : options.linkerScript;
}

std::string effectiveSysroot(const LinkOptions& options) {
    return options.sysroot.empty() ? options.target.sysroot : options.sysroot;
}

std::string effectiveOutputFormat(const LinkOptions& options) {
    return options.outputFormat.empty() ? options.target.outputFormat : options.outputFormat;
}

std::string effectiveDynamicLinker(const LinkOptions& options) {
    return options.dynamicLinker.empty() ? options.target.dynamicLinker : options.dynamicLinker;
}

bool wantsRawBinary(const LinkOptions& options) {
    std::string outputFormat = effectiveOutputFormat(options);
    return options.rawBinary || outputFormat == "raw-binary" || outputFormat == "binary";
}

bool wantsMultiboot2(const LinkOptions& options) {
    return options.multiboot2 || options.target.multiboot2;
}

bool supportsMultiboot2Header(const Targeting::TargetSpec& target) {
    return target.isElf() && (target.arch == "x86_64" || target.arch == "i386" || target.arch == "i686" || target.arch == "x86");
}

std::string multibootTripleFor(const Targeting::TargetSpec& target) {
    if (target.arch == "x86_64") {
        return "x86_64-unknown-elf";
    }
    return "i386-unknown-elf";
}

std::optional<fs::path> createMultiboot2HeaderObject(const Targeting::TargetSpec& target, bool verbose) {
    if (!supportsMultiboot2Header(target)) {
        std::cerr << "Error: Multiboot2 header generation currently supports only x86/x86_64 ELF targets\n";
        return std::nullopt;
    }

    static std::atomic<unsigned> counter{0};
    fs::path tempDir = fs::temp_directory_path() / "insty-link";
    std::error_code ec;
    fs::create_directories(tempDir, ec);
    if (ec) {
        std::cerr << "Error: Could not create temporary linker directory: " << ec.message() << "\n";
        return std::nullopt;
    }

    std::string baseName = "multiboot2-" + std::to_string(getpid()) + "-" + std::to_string(counter++);
    fs::path asmPath = tempDir / (baseName + ".S");
    fs::path objPath = tempDir / (baseName + ".o");

    std::ofstream asmFile(asmPath);
    if (!asmFile.is_open()) {
        std::cerr << "Error: Could not create Multiboot2 assembly file: " << asmPath << "\n";
        return std::nullopt;
    }

    asmFile << ".section .multiboot2_header,\"a\"\n"
            << ".align 8\n"
            << ".globl __ins_multiboot2_header\n"
            << "__ins_multiboot2_header:\n"
            << "1:\n"
            << ".long 0xe85250d6\n"
            << ".long 0\n"
            << ".long 2f - 1b\n"
            << ".long -(0xe85250d6 + 0 + (2f - 1b))\n"
            << ".short 0\n"
            << ".short 0\n"
            << ".long 8\n"
            << "2:\n";
    asmFile.close();

    std::string llvmMc = toolPathFromEnv("LLVM_MC", "llvm-mc");
    std::vector<std::string> args = {
        llvmMc,
        "-triple",
        multibootTripleFor(target),
        "-filetype=obj",
        asmPath.string(),
        "-o",
        objPath.string()
    };

    if (verbose) {
        std::cout << "Generating Multiboot2 header: " << formatCommand(args) << "\n";
    }

    if (!runProcess(args)) {
        std::cerr << "Error: Failed to generate Multiboot2 header object with " << llvmMc << "\n";
        return std::nullopt;
    }

    return objPath;
}

void appendSysrootArgs(std::vector<std::string>& args, LinkerFlavor flavor, const std::string& sysroot) {
    if (sysroot.empty()) {
        return;
    }

    if (flavor == LinkerFlavor::Elf) {
        args.push_back("--sysroot=" + sysroot);
    } else if (flavor == LinkerFlavor::MachO) {
        args.push_back("-syslibroot");
        args.push_back(sysroot);
    } else {
        args.push_back("/winsysroot:" + sysroot);
    }
}

void appendLibraryPath(std::vector<std::string>& args, LinkerFlavor flavor, const std::string& path) {
    if (flavor == LinkerFlavor::Coff) {
        args.push_back("/libpath:" + path);
    } else {
        args.push_back("-L" + path);
    }
}

void appendEntry(std::vector<std::string>& args, LinkerFlavor flavor, const std::string& entrySymbol) {
    if (entrySymbol.empty()) {
        return;
    }

    if (flavor == LinkerFlavor::Coff) {
        args.push_back("/entry:" + entrySymbol);
    } else {
        args.push_back("-e");
        args.push_back(entrySymbol);
    }
}

}

std::vector<std::string> getSystemCRTFiles() {
    std::vector<std::string> crtFiles;
    for (const std::string& fileName : {"crt1.o", "crti.o", "crtn.o"}) {
        std::string output;
        if (!runProcessCapture({compilerDriver(), "-print-file-name=" + fileName}, output)) {
            continue;
        }

        std::string path = trim(output);
        if (!path.empty() && path != fileName && fs::exists(path)) {
            crtFiles.push_back(path);
        }
    }

    return crtFiles;
}

std::string getInstyRuntimePath() {
    const char* envPath = std::getenv("ECXRT_PATH");
    if (envPath && *envPath && fs::exists(envPath)) {
        return envPath;
    }

    for (const auto& path : {"runtime/ecxrt.o", "../runtime/ecxrt.o", "./ecxrt.o"}) {
        if (fs::exists(path)) {
            return path;
        }
    }

    return "";
}

std::vector<std::string> getSystemLibraryPaths() {
    std::string output;
    if (!runProcessCapture({compilerDriver(), "-print-search-dirs"}, output)) {
        return {};
    }

    std::vector<std::string> paths;
    std::stringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.rfind("libraries:", 0) != 0) {
            continue;
        }

        size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }

        for (const auto& path : split(line.substr(equals + 1), ':')) {
            if (fs::is_directory(path)) {
                paths.push_back(path);
            }
        }
    }

    return paths;
}

bool linkExecutable(const LinkOptions& options) {
    LOG_FUNCTION_ENTRY("Linker");
    LOG_INFO("Linker", "Starting linking process");

    if (options.objectFiles.empty()) {
        LOG_ERROR("Linker", "No object files to link");
        std::cerr << "Error: No object files to link\n";
        return false;
    }

    const auto flavor = getLinkerFlavor(options.target);
    std::string linkerName = effectiveLinker(options);
    if (linkerName.empty()) {
        LOG_ERROR("Linker", "Unsupported target platform");
        std::cerr << "Error: Unsupported target platform for linking\n";
        return false;
    }

    if (options.freestanding && !options.cimports.empty()) {
        std::cerr << "Error: freestanding links cannot use cimport auto-link libraries\n";
        return false;
    }

    std::string sysroot = effectiveSysroot(options);
    std::string linkerScript = effectiveLinkerScript(options);
    std::string dynamicLinker = effectiveDynamicLinker(options);
    bool rawBinary = wantsRawBinary(options);

    std::vector<std::string> linkObjects = options.objectFiles;
    if (wantsMultiboot2(options)) {
        auto headerObject = createMultiboot2HeaderObject(options.target, options.verbose);
        if (!headerObject) {
            return false;
        }
        linkObjects.insert(linkObjects.begin(), headerObject->string());
    }

    bool needsHosted = !options.freestanding && !options.target.isFreestandingExecutable();
    if (!options.freestanding && options.target.supportsLinuxSyscallRuntime() && !options.cimports.empty()) {
        needsHosted = true;
    }

    std::vector<std::string> args;
    args.push_back(linkerName);

    if (flavor == LinkerFlavor::Elf) {
        std::string emulation = elfEmulationFor(options.target);
        if (!emulation.empty()) {
            args.push_back("-m");
            args.push_back(emulation);
        }
        appendSysrootArgs(args, flavor, sysroot);
        if (!linkerScript.empty()) {
            args.push_back("-T");
            args.push_back(linkerScript);
        }
        if (rawBinary) {
            args.push_back("--oformat=binary");
        }
        args.push_back("--gc-sections");
        args.push_back("--icf=all");
        args.push_back("--lto-O3");
        args.push_back("--strip-all");
    } else if (flavor == LinkerFlavor::MachO) {
        args.push_back("-arch");
        args.push_back(machoArchFor(options.target));
        appendSysrootArgs(args, flavor, sysroot);
        args.push_back("-platform_version");
        args.push_back("macos");
        args.push_back("11.0.0");
        args.push_back("11.0.0");
    } else {
        args.push_back("/machine:" + coffMachineFor(options.target));
        appendSysrootArgs(args, flavor, sysroot);
        args.push_back("/opt:ref");
        args.push_back("/opt:icf");
        if (options.target.isInstantOS) {
            args.push_back("/base:0x00007FFFFFE00000");
        }
    }

    if (needsHosted && flavor == LinkerFlavor::Elf) {
        for (const auto& crt : getSystemCRTFiles()) {
            args.push_back(crt);
        }
    }

    if (flavor == LinkerFlavor::Coff) {
        args.push_back("/out:" + options.outputFile);
        for (const auto& obj : linkObjects) {
            args.push_back(obj);
        }
    } else {
        for (const auto& obj : linkObjects) {
            args.push_back(obj);
        }
        args.push_back("-o");
        args.push_back(options.outputFile);
    }

    std::vector<std::string> autoLibraries = options.libraries;
    for (const auto& cimport : options.cimports) {
        autoLibraries.push_back(cimport);
    }

    const bool needsLibrarySearch = !options.freestanding && (needsHosted || !autoLibraries.empty());
    if (needsLibrarySearch) {
        for (const auto& path : getSystemLibraryPaths()) {
            appendLibraryPath(args, flavor, path);
        }
        for (const auto& path : options.libraryPaths) {
            appendLibraryPath(args, flavor, path);
        }

        for (const auto& lib : autoLibraries) {
            if (flavor == LinkerFlavor::Coff) {
                args.push_back(lib + ".lib");
            } else {
                args.push_back("-l" + lib);
            }
        }

        if (flavor == LinkerFlavor::Elf) {
            args.push_back("-lc");
            if (!dynamicLinker.empty()) {
                args.push_back("-dynamic-linker");
                args.push_back(dynamicLinker);
            }
        } else if (flavor == LinkerFlavor::MachO) {
            args.push_back("-lSystem");
        } else if (!options.target.isEfi && !options.target.isInstantOS) {
            args.push_back("libcmt.lib");
        }
    } else {
        if (flavor == LinkerFlavor::Elf) {
            args.push_back("-static");
            appendEntry(args, flavor, options.entrySymbol);
        } else if (flavor == LinkerFlavor::Coff) {
            if (options.target.isEfi || effectiveOutputFormat(options) == "uefi") {
                args.push_back("/subsystem:efi_application");
            } else {
                args.push_back("/subsystem:console");
            }
            appendEntry(args, flavor, options.entrySymbol);
            if (options.freestanding || options.target.isEfi || options.target.isInstantOS) {
                args.push_back("/nodefaultlib");
            }
        }
    }

    if (options.verbose) {
        std::cout << "Using " << linkerName << " for target " << options.target.cliName << "\n";
        std::cout << "Linking: " << formatCommand(args) << "\n";
    }

    LOG_DEBUG("Linker", "Executing linker");
    if (!runProcess(args)) {
        LOG_ERROR("Linker", "Linking failed");
        std::cerr << "Linking failed\n";
        std::cerr << "Make sure " << linkerName << " is installed and in PATH\n";
        if (!options.cimports.empty()) {
            std::cerr << "Also ensure the requested C libraries are available in the sysroot or linker search paths\n";
        }
        LOG_FUNCTION_EXIT("Linker");
        return false;
    }

    LOG_INFO("Linker", "Linking successful: ", options.outputFile);
    LOG_FUNCTION_EXIT("Linker");
    return true;
}

}
