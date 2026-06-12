#pragma once


#include <string>
#include <vector>

#include <utilities/runtime.hpp>
#include <utilities/target.hpp>

namespace Config {

enum class OutputMode {
    Check,
    EmitTokens,
    EmitAst,
    EmitLlvm,
    Object,
    Executable
};

struct CompilerConfig {
    std::vector<std::string> inputs;
    std::string outputFile;
    std::string objectsDir;

    OutputMode mode = OutputMode::Executable;

    Targeting::TargetSpec target = Targeting::defaultTargetSpec();
    std::string targetName = "x86_64_linux";

    bool freestanding = false;
    bool noStd = false;
    bool runtimeStart = false;
    bool multiboot2 = false;
    bool rawBinary = false;
    bool verbose = false;
    int optLevel = 0;

    Runtime::AllocatorMode allocatorMode = Runtime::AllocatorMode::None;
    Runtime::PanicStrategy panicStrategy = Runtime::PanicStrategy::Abort;
    std::string panicHandler;

    std::string entrySymbol;
    std::string linkerPath;
    std::string linkerScript;
    std::string sysroot;
    std::string outputFormat;

    std::vector<std::string> moduleSearchPaths;
    std::string stdLibDir;

    Runtime::Options runtimeOptions() const {
        Runtime::Options options;
        options.freestanding = freestanding;
        options.emitPlatformStart = !freestanding || runtimeStart;
        options.allocatorMode = allocatorMode;
        options.panicStrategy = panicStrategy;
        options.panicHandler = panicHandler;
        return options;
    }
};

}
