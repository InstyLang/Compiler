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

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

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

#if defined(_WIN32)

// Quote a single argument according to the rules the Windows CRT/CommandLineToArgvW
// use, so arguments containing spaces or quotes survive the round trip through a
// single command-line string (Windows has no argv array for CreateProcess).
std::string quoteWindowsArg(const std::string& arg) {
    if (!arg.empty() &&
        arg.find_first_of(" \t\n\v\"") == std::string::npos) {
        return arg;
    }

    std::string quoted = "\"";
    for (auto it = arg.begin();; ++it) {
        unsigned backslashes = 0;
        while (it != arg.end() && *it == '\\') {
            ++it;
            ++backslashes;
        }

        if (it == arg.end()) {
            quoted.append(backslashes * 2, '\\');
            break;
        }

        if (*it == '"') {
            quoted.append(backslashes * 2 + 1, '\\');
            quoted += '"';
        } else {
            quoted.append(backslashes, '\\');
            quoted += *it;
        }
    }
    quoted += '"';
    return quoted;
}

std::string buildCommandLine(const std::vector<std::string>& args) {
    std::string commandLine;
    for (size_t index = 0; index < args.size(); ++index) {
        if (index != 0) {
            commandLine += ' ';
        }
        commandLine += quoteWindowsArg(args[index]);
    }
    return commandLine;
}

// Launch a process and (optionally) capture its stdout. Replaces the POSIX
// fork/exec/pipe/waitpid model with CreateProcess + anonymous pipe + wait.
bool spawnProcess(const std::vector<std::string>& args, std::string* output) {
    if (args.empty()) {
        return false;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (output) {
        if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
            return false;
        }
        // The read end must not be inherited by the child.
        SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    if (output) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = writePipe;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }

    PROCESS_INFORMATION pi{};
    std::string commandLine = buildCommandLine(args);
    std::vector<char> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back('\0');

    const BOOL created = CreateProcessA(
        nullptr,
        mutableCommandLine.data(),
        nullptr,
        nullptr,
        output ? TRUE : FALSE,
        0,
        nullptr,
        nullptr,
        &si,
        &pi);

    if (!created) {
        if (output) {
            CloseHandle(readPipe);
            CloseHandle(writePipe);
        }
        return false;
    }

    if (output) {
        // Close our copy of the write end so the read loop terminates on exit.
        CloseHandle(writePipe);
        output->clear();
        char buffer[4096];
        DWORD readCount = 0;
        while (ReadFile(readPipe, buffer, sizeof(buffer), &readCount, nullptr) && readCount > 0) {
            output->append(buffer, static_cast<size_t>(readCount));
        }
        CloseHandle(readPipe);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return exitCode == 0;
}

bool runProcess(const std::vector<std::string>& args) {
    return spawnProcess(args, nullptr);
}

bool runProcessCapture(const std::vector<std::string>& args, std::string& output) {
    output.clear();
    return spawnProcess(args, &output);
}

#else

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

#endif

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
    // On Windows, CreateProcess only auto-appends ".exe" when the program name
    // has no extension. "ld.lld" / "ld64.lld" already contain a '.', so the
    // ".lld" is mistaken for an extension and the lookup fails; spell the ".exe"
    // explicitly. "lld-link" has no dot and resolves fine, but we suffix it too
    // for consistency.
#if defined(_WIN32)
    switch (getLinkerFlavor(target)) {
        case LinkerFlavor::Elf:
            return "ld.lld.exe";
        case LinkerFlavor::MachO:
            return "ld64.lld.exe";
        case LinkerFlavor::Coff:
            return "lld-link.exe";
    }
    return "";
#else
    switch (getLinkerFlavor(target)) {
        case LinkerFlavor::Elf:
            return "ld.lld";
        case LinkerFlavor::MachO:
            return "ld64.lld";
        case LinkerFlavor::Coff:
            return "lld-link";
    }
    return "";
#endif
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

#if defined(_WIN32)
    const auto processId = static_cast<unsigned long>(GetCurrentProcessId());
#else
    const auto processId = static_cast<unsigned long>(getpid());
#endif
    std::string baseName = "multiboot2-" + std::to_string(processId) + "-" + std::to_string(counter++);
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

    std::string assembler = toolPathFromEnv("INSTY_AS", "as");
    std::vector<std::string> args = {
        assembler
    };
    if (target.arch == "x86_64") {
        args.push_back("--64");
    } else {
        args.push_back("--32");
    }
    args.push_back(asmPath.string());
    args.push_back("-o");
    args.push_back(objPath.string());

    if (verbose) {
        std::cout << "Generating Multiboot2 header: " << formatCommand(args) << "\n";
    }

    if (!runProcess(args)) {
        std::cerr << "Error: Failed to generate Multiboot2 header object with " << assembler << "\n";
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

#if defined(_WIN32)
        const char pathSeparator = ';';
#else
        const char pathSeparator = ':';
#endif
        for (const auto& path : split(line.substr(equals + 1), pathSeparator)) {
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

    // InstantOS userland: a dynamically-linked PIE on top of mlibc + the
    // ld-instantos.so runtime loader. We deliberately bypass the generic
    // hosted-ELF path (which would inject host crt/library discovery) and emit
    // the exact recipe InstantOS's tools/build-mlibc-hello.sh uses:
    //
    //   ld.lld --gc-sections --build-id=none --hash-style=sysv
    //          -z max-page-size=0x1000 -pie -e _start
    //          --dynamic-linker /lib/mlibc/ld-instantos.so -rpath /lib/mlibc
    //          -o out <sysroot>/lib/crt1.o <objects> -L <sysroot>/lib -lc
    //
    // crt1.o supplies `_start` and calls our `main`; libc.so is mlibc.
    if (options.target.isInstantOS) {
        if (sysroot.empty()) {
            std::cerr << "Error: InstantOS link requires an mlibc sysroot "
                         "(set --sysroot or the target's sysroot)\n";
            return false;
        }
        const std::string libDir = sysroot + "/lib";
        const std::string crt1 = libDir + "/crt1.o";
        if (!fs::exists(crt1)) {
            std::cerr << "Error: InstantOS sysroot is missing crt1.o: " << crt1 << "\n";
            std::cerr << "Build it with InstantOS tools/build-mlibc.sh\n";
            return false;
        }
        const std::string entry = options.entrySymbol.empty() ? "_start" : options.entrySymbol;

        args.push_back("--gc-sections");
        args.push_back("--build-id=none");
        args.push_back("--hash-style=sysv");
        args.push_back("-z");
        args.push_back("max-page-size=0x1000");
        args.push_back("-pie");
        args.push_back("-e");
        args.push_back(entry);
        args.push_back("--dynamic-linker");
        args.push_back(dynamicLinker.empty() ? "/lib/mlibc/ld-instantos.so" : dynamicLinker);
        args.push_back("-rpath");
        args.push_back("/lib/mlibc");
        args.push_back("-o");
        args.push_back(options.outputFile);
        args.push_back(crt1);
        for (const auto& obj : linkObjects) {
            args.push_back(obj);
        }
        args.push_back("-L" + libDir);
        for (const auto& path : options.libraryPaths) {
            args.push_back("-L" + path);
        }
        args.push_back("-lc");
        for (const auto& lib : options.libraries) {
            args.push_back("-l" + lib);
        }

        if (options.verbose) {
            std::cout << "Using " << linkerName << " for target " << options.target.cliName << "\n";
            std::cout << "Linking: " << formatCommand(args) << "\n";
        }
        if (!runProcess(args)) {
            std::cerr << "Linking failed\n";
            std::cerr << "Make sure " << linkerName << " is installed and in PATH, and the "
                         "InstantOS mlibc sysroot is complete\n";
            return false;
        }
        return true;
    }

    // Hosted, dynamically-linked Linux executable. Reached when a plain Linux
    // build pulled in shared libraries via the `lib(...)` directive (the driver
    // then clears `freestanding`). The system C runtime provides `_start`, which
    // sets up argc/argv/env and calls `main` through __libc_start_main, so the
    // backend emits no `_start` shim for this mode. Recipe (mirrors what a `cc`
    // driver would run for `cc objs -o out -l<libs>`):
    //
    //   ld.lld -m elf_x86_64 --eh-frame-hdr --dynamic-linker <ld.so>
    //          -o out crt1.o crti.o <objects> -L<sysdirs> -l<libs> -lc crtn.o
    //
    if (options.target.isLinux() && !options.freestanding && !rawBinary) {
        std::string emulation = elfEmulationFor(options.target);
        if (!emulation.empty()) {
            args.push_back("-m");
            args.push_back(emulation);
        }
        args.push_back("--eh-frame-hdr");
        const std::string interp =
            !dynamicLinker.empty() ? dynamicLinker : std::string("/lib64/ld-linux-x86-64.so.2");
        args.push_back("--dynamic-linker");
        args.push_back(interp);
        args.push_back("-o");
        args.push_back(options.outputFile);

        // crt1.o + crti.o precede the user objects; crtn.o trails everything.
        std::string crtn;
        for (const auto& crt : getSystemCRTFiles()) {
            if (fs::path(crt).filename() == "crtn.o") {
                crtn = crt;
            } else {
                args.push_back(crt);
            }
        }
        for (const auto& obj : linkObjects) {
            args.push_back(obj);
        }
        for (const auto& path : getSystemLibraryPaths()) {
            args.push_back("-L" + path);
        }
        for (const auto& path : options.libraryPaths) {
            args.push_back("-L" + path);
        }
        for (const auto& lib : options.libraries) {
            args.push_back("-l" + lib);
        }
        args.push_back("-lc");
        if (!crtn.empty()) {
            args.push_back(crtn);
        }

        if (options.verbose) {
            std::cout << "Using " << linkerName << " for target " << options.target.cliName
                      << " (hosted dynamic)\n";
            std::cout << "Linking: " << formatCommand(args) << "\n";
        }
        if (!runProcess(args)) {
            std::cerr << "Linking failed\n";
            std::cerr << "Make sure " << linkerName << ", the C runtime (crt1.o/crti.o/crtn.o) "
                         "and the requested libraries are installed and in the linker search path\n";
            return false;
        }
        return true;
    }

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
            if (!options.target.isEfi && !options.target.isInstantOS) {
                args.push_back("kernel32.lib");
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
