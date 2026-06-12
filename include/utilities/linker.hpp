#pragma once


#include <string>
#include <vector>

#include <utilities/target.hpp>

namespace Linker {

struct LinkOptions {
    std::vector<std::string> objectFiles;
    std::string outputFile;
    Targeting::TargetSpec target;

    bool freestanding = false;
    bool rawBinary = false;
    bool multiboot2 = false;
    bool verbose = false;

    std::string linkerPath;
    std::string linkerScript;
    std::string sysroot;
    std::string dynamicLinker;
    std::string outputFormat;
    std::string entrySymbol;

    std::vector<std::string> libraries;
    std::vector<std::string> libraryPaths;
    std::vector<std::string> cimports;
};

bool linkExecutable(const LinkOptions& options);

std::vector<std::string> getSystemCRTFiles();
std::vector<std::string> getSystemLibraryPaths();
std::string getInstyRuntimePath();

}
