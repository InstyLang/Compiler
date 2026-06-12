
#include <iostream>
#include <string>
#include <vector>

#include <compiler/driver.hpp>
#include <utilities/config.hpp>
#include <utilities/target.hpp>
#include <utilities/utils.hpp>

namespace {

void printUsage() {
    std::cout <<
        "Usage: insty [options] <input.ins ...>\n"
        "\n"
        "Output modes:\n"
        "  (default)              build an executable (link)\n"
        "  -c                     compile to object files only\n"
        "  --emit-llvm            emit textual LLVM IR (.ll)\n"
        "  --emit-tokens          print the token stream\n"
        "  --emit-ast             parse + semantic-check, print summary\n"
        "  --check                parse + semantic-check only\n"
        "\n"
        "Options:\n"
        "  -o <path>              output file\n"
        "  --objects-dir <dir>    directory for emitted object files\n"
        "  -L, --module-path <dir>  extra module search directory (repeatable)\n"
        "  --target <name|.toml>  target (default x86_64_linux)\n"
        "  --freestanding         no libc / no hosted runtime assumptions\n"
        "  --no-std               do not auto-resolve the std library\n"
        "  --runtime-start        emit the platform _start shim (freestanding)\n"
        "  --allocator <mode>     none | runtime | external\n"
        "  --panic <mode>         abort | handler\n"
        "  --panic-handler <sym>  panic handler symbol\n"
        "  --entry <symbol>       linker entry symbol\n"
        "  --linker <path>        override linker binary\n"
        "  --linker-script <file> linker script (-T)\n"
        "  --sysroot <dir>        sysroot for linking\n"
        "  --output-format <fmt>  executable|elf|raw-binary|pe|uefi\n"
        "  --raw-binary           emit a flat binary\n"
        "  --multiboot2           prepend a Multiboot2 header object\n"
        "  -O0 -O1 -O2 -O3        optimization level\n"
        "  -v, --verbose          verbose output\n"
        "  -h, --help             show this help\n";
}

bool needsValue(const std::string& flag, int& i, int argc, char** argv, std::string& out) {
    if (i + 1 >= argc) {
        std::cerr << "error: " << flag << " requires a value\n";
        return false;
    }
    out = argv[++i];
    return true;
}

}

int main(int argc, char** argv) {
    Config::CompilerConfig config;
    config.stdLibDir = Utilities::executableDirectory() + "/libs";

    bool sawObjectOnly = false;
    bool sawExplicitMode = false;
    std::string allocatorMode;
    std::string panicMode;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        std::string value;

        if (arg == "-h" || arg == "--help") {
            printUsage();
            return 0;
        } else if (arg == "-c") {
            sawObjectOnly = true;
        } else if (arg == "--emit-llvm") {
            config.mode = Config::OutputMode::EmitLlvm;
            sawExplicitMode = true;
        } else if (arg == "--emit-tokens") {
            config.mode = Config::OutputMode::EmitTokens;
            sawExplicitMode = true;
        } else if (arg == "--emit-ast") {
            config.mode = Config::OutputMode::EmitAst;
            sawExplicitMode = true;
        } else if (arg == "--check") {
            config.mode = Config::OutputMode::Check;
            sawExplicitMode = true;
        } else if (arg == "-o") {
            if (!needsValue(arg, i, argc, argv, config.outputFile)) return 2;
        } else if (arg == "--objects-dir") {
            if (!needsValue(arg, i, argc, argv, config.objectsDir)) return 2;
        } else if (arg == "--target") {
            if (!needsValue(arg, i, argc, argv, value)) return 2;
            config.targetName = value;
        } else if (arg == "--freestanding") {
            config.freestanding = true;
        } else if (arg == "--no-std") {
            config.noStd = true;
        } else if (arg == "--runtime-start") {
            config.runtimeStart = true;
        } else if (arg == "--allocator") {
            if (!needsValue(arg, i, argc, argv, allocatorMode)) return 2;
        } else if (arg == "--panic") {
            if (!needsValue(arg, i, argc, argv, panicMode)) return 2;
        } else if (arg == "--panic-handler") {
            if (!needsValue(arg, i, argc, argv, config.panicHandler)) return 2;
        } else if (arg == "--entry") {
            if (!needsValue(arg, i, argc, argv, config.entrySymbol)) return 2;
        } else if (arg == "--linker") {
            if (!needsValue(arg, i, argc, argv, config.linkerPath)) return 2;
        } else if (arg == "--linker-script") {
            if (!needsValue(arg, i, argc, argv, config.linkerScript)) return 2;
        } else if (arg == "--sysroot") {
            if (!needsValue(arg, i, argc, argv, config.sysroot)) return 2;
        } else if (arg == "--output-format") {
            if (!needsValue(arg, i, argc, argv, config.outputFormat)) return 2;
        } else if (arg == "--raw-binary") {
            config.rawBinary = true;
        } else if (arg == "--multiboot2") {
            config.multiboot2 = true;
        } else if (arg == "-O0") {
            config.optLevel = 0;
        } else if (arg == "-O1") {
            config.optLevel = 1;
        } else if (arg == "-O2") {
            config.optLevel = 2;
        } else if (arg == "-O3") {
            config.optLevel = 3;
        } else if (arg == "-v" || arg == "--verbose") {
            config.verbose = true;
        } else if (arg == "-L" || arg == "--module-path") {
            if (!needsValue(arg, i, argc, argv, value)) return 2;
            config.moduleSearchPaths.push_back(value);
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "error: unknown option '" << arg << "'\n";
            return 2;
        } else {
            config.inputs.push_back(arg);
        }
    }

    if (config.inputs.empty() && config.mode != Config::OutputMode::Check) {
        std::cerr << "error: no input files\n";
        printUsage();
        return 2;
    }

    if (!sawExplicitMode) {
        config.mode = sawObjectOnly ? Config::OutputMode::Object
                                    : Config::OutputMode::Executable;
    } else if (sawObjectOnly && config.mode == Config::OutputMode::EmitLlvm) {
    }

    std::string targetError;
    auto target = Targeting::loadTargetSpec(config.targetName, targetError);
    if (!target) {
        std::cerr << "error: " << (targetError.empty() ? "unsupported target" : targetError)
                  << " (" << Targeting::supportedTargetList() << ")\n";
        return 2;
    }
    config.target = *target;

    if (config.target.isCustom && config.target.isFreestandingExecutable()) {
        config.freestanding = true;
    }

    if (!allocatorMode.empty()) {
        if (allocatorMode == "none") {
            config.allocatorMode = Runtime::AllocatorMode::None;
        } else if (allocatorMode == "runtime") {
            config.allocatorMode = Runtime::AllocatorMode::Runtime;
        } else if (allocatorMode == "external") {
            config.allocatorMode = Runtime::AllocatorMode::External;
        } else {
            std::cerr << "error: --allocator must be none, runtime, or external\n";
            return 2;
        }
    }

    std::string targetPanic = config.target.panicStrategy;
    if (panicMode.empty()) {
        panicMode = targetPanic;
    }
    if (panicMode == "handler") {
        config.panicStrategy = Runtime::PanicStrategy::Handler;
        if (config.panicHandler.empty()) {
            config.panicHandler = config.target.panicHandler;
        }
    } else if (panicMode == "abort" || panicMode.empty()) {
        config.panicStrategy = Runtime::PanicStrategy::Abort;
    } else {
        std::cerr << "error: --panic must be abort or handler\n";
        return 2;
    }

    if (config.entrySymbol.empty()) {
        config.entrySymbol = config.target.entrySymbol;
    }

    Driver::CompilerDriver driver(std::move(config));
    return driver.run();
}
