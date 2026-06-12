#include <utilities/target.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace Targeting {

namespace {

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

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string stripComment(std::string line) {
    bool inString = false;
    char quote = '\0';

    for (size_t index = 0; index < line.size(); ++index) {
        char ch = line[index];
        if ((ch == '"' || ch == '\'') && (index == 0 || line[index - 1] != '\\')) {
            if (!inString) {
                inString = true;
                quote = ch;
            } else if (quote == ch) {
                inString = false;
            }
        }

        if (ch == '#' && !inString) {
            return line.substr(0, index);
        }
    }

    return line;
}

std::string parseScalar(std::string raw) {
    raw = trim(stripComment(std::move(raw)));
    if (raw.size() >= 2) {
        char first = raw.front();
        char last = raw.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            return raw.substr(1, raw.size() - 2);
        }
    }
    return raw;
}

std::unordered_map<std::string, std::string> parseTargetFile(const fs::path& path, std::string& errorMessage) {
    std::ifstream file(path);
    if (!file.is_open()) {
        errorMessage = "could not open target spec file '" + path.string() + "'";
        return {};
    }

    std::unordered_map<std::string, std::string> values;
    std::string line;
    int lineNumber = 0;

    while (std::getline(file, line)) {
        ++lineNumber;
        line = trim(stripComment(line));
        if (line.empty()) {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            continue;
        }

        size_t equals = line.find('=');
        if (equals == std::string::npos) {
            errorMessage = "invalid target spec line " + std::to_string(lineNumber) + ": expected key = value";
            return {};
        }

        std::string key = lower(trim(line.substr(0, equals)));
        std::string value = parseScalar(line.substr(equals + 1));
        if (key.empty()) {
            errorMessage = "invalid target spec line " + std::to_string(lineNumber) + ": empty key";
            return {};
        }
        values[key] = value;
    }

    return values;
}

std::string getValue(const std::unordered_map<std::string, std::string>& values,
                     const std::string& key,
                     const std::string& fallback = "") {
    auto it = values.find(key);
    return it == values.end() ? fallback : it->second;
}

bool hasValue(const std::unordered_map<std::string, std::string>& values, const std::string& key) {
    return values.find(key) != values.end();
}

bool parseBool(const std::unordered_map<std::string, std::string>& values,
               const std::string& key,
               bool fallback,
               std::string& errorMessage) {
    if (!hasValue(values, key)) {
        return fallback;
    }

    std::string value = lower(getValue(values, key));
    if (value == "true") {
        return true;
    }
    if (value == "false") {
        return false;
    }

    errorMessage = "target spec key '" + key + "' must be true or false";
    return fallback;
}

int parseInt(const std::unordered_map<std::string, std::string>& values,
             const std::string& key,
             int fallback,
             std::string& errorMessage) {
    if (!hasValue(values, key)) {
        return fallback;
    }

    try {
        size_t consumed = 0;
        int parsed = std::stoi(getValue(values, key), &consumed, 10);
        if (consumed != getValue(values, key).size()) {
            errorMessage = "target spec key '" + key + "' must be an integer";
            return fallback;
        }
        return parsed;
    } catch (...) {
        errorMessage = "target spec key '" + key + "' must be an integer";
        return fallback;
    }
}

std::vector<std::string> splitTriple(const std::string& triple) {
    std::vector<std::string> parts;
    std::stringstream stream(triple);
    std::string part;
    while (std::getline(stream, part, '-')) {
        parts.push_back(part);
    }
    return parts;
}

std::string inferObjectFormat(const std::string& triple, const std::string& os, const std::string& abi) {
    std::string loweredTriple = lower(triple);
    std::string loweredOs = lower(os);
    std::string loweredAbi = lower(abi);
    if (loweredTriple.find("apple") != std::string::npos || loweredOs == "macos" || loweredOs == "darwin") {
        return "mach-o";
    }
    if (loweredTriple.find("windows") != std::string::npos || loweredTriple.find("win32") != std::string::npos ||
        loweredOs == "windows" || loweredOs == "uefi" || loweredAbi == "efi") {
        return "coff";
    }
    return "elf";
}

int inferPointerWidth(const std::string& arch) {
    std::string loweredArch = lower(arch);
    if (loweredArch.find("64") != std::string::npos || loweredArch == "arm64" || loweredArch == "aarch64") {
        return 64;
    }
    return 32;
}

bool validateObjectFormat(const std::string& value) {
    return value == "elf" || value == "mach-o" || value == "macho" || value == "coff";
}

bool validateOutputFormat(const std::string& value) {
    return value == "executable" || value == "elf" || value == "kernel" ||
           value == "raw-binary" || value == "binary" || value == "pe" || value == "uefi";
}

bool validateEndian(const std::string& value) {
    return value == "little" || value == "big";
}

std::string resolveRelativeTo(const fs::path& baseFile, const std::string& maybeRelativePath) {
    if (maybeRelativePath.empty()) {
        return "";
    }

    fs::path path(maybeRelativePath);
    if (path.is_absolute()) {
        return path.lexically_normal().string();
    }

    return (baseFile.parent_path() / path).lexically_normal().string();
}

}

std::optional<TargetSpec> loadTargetSpec(std::string_view cliNameOrPath, std::string& errorMessage) {
    if (auto builtin = parseTargetSpec(cliNameOrPath)) {
        return builtin;
    }

    fs::path path{std::string(cliNameOrPath)};
    if (!fs::exists(path)) {
        errorMessage = "unsupported target '" + std::string(cliNameOrPath) + "'";
        return std::nullopt;
    }
    if (!fs::is_regular_file(path)) {
        errorMessage = "target spec path is not a file: '" + path.string() + "'";
        return std::nullopt;
    }

    std::unordered_map<std::string, std::string> values = parseTargetFile(path, errorMessage);
    if (!errorMessage.empty()) {
        return std::nullopt;
    }

    std::string llvmTriple = getValue(values, "llvm_triple");
    if (llvmTriple.empty()) {
        errorMessage = "target spec requires llvm_triple";
        return std::nullopt;
    }

    std::vector<std::string> tripleParts = splitTriple(llvmTriple);
    std::string arch = lower(getValue(values, "arch", tripleParts.empty() ? "" : tripleParts[0]));
    std::string vendor = lower(getValue(values, "vendor", tripleParts.size() > 1 ? tripleParts[1] : "unknown"));
    std::string os = lower(getValue(values, "os", tripleParts.size() > 2 ? tripleParts[2] : "unknown"));
    std::string abi = lower(getValue(values, "abi", tripleParts.size() > 3 ? tripleParts[3] : ""));
    std::string objectFormat = lower(getValue(values, "object_format", inferObjectFormat(llvmTriple, os, abi)));
    std::string outputFormat = lower(getValue(values, "output_format", "executable"));
    std::string endian = lower(getValue(values, "endian", "little"));
    int pointerWidth = parseInt(values, "pointer_width", inferPointerWidth(arch), errorMessage);
    if (!errorMessage.empty()) {
        return std::nullopt;
    }

    if (!validateObjectFormat(objectFormat)) {
        errorMessage = "target spec object_format must be one of: elf, mach-o, macho, coff";
        return std::nullopt;
    }
    if (!validateOutputFormat(outputFormat)) {
        errorMessage = "target spec output_format must be one of: executable, elf, kernel, raw-binary, binary, pe, uefi";
        return std::nullopt;
    }
    if (!validateEndian(endian)) {
        errorMessage = "target spec endian must be one of: little, big";
        return std::nullopt;
    }
    if (pointerWidth != 16 && pointerWidth != 32 && pointerWidth != 64) {
        errorMessage = "target spec pointer_width must be 16, 32, or 64";
        return std::nullopt;
    }

    bool isEfi = os == "uefi" || abi == "efi" || outputFormat == "uefi";
    bool isInstantOS = os == "instantos" || abi == "instantos";
    bool isApple = objectFormat == "mach-o" || objectFormat == "macho" || os == "macos" || os == "darwin";
    bool isWindowsLike = objectFormat == "coff";
    bool defaultFreestanding = os == "none" || abi == "kernel" || isEfi || isInstantOS;

    TargetSpec spec;
    spec.kind = TargetKind::Custom;
    spec.cliName = getValue(values, "name", path.stem().string());
    spec.arch = arch;
    spec.vendor = vendor;
    spec.os = os;
    spec.abi = abi;
    spec.llvmTriple = llvmTriple;
    spec.objectFormat = objectFormat;
    spec.outputFormat = outputFormat;
    spec.pointerWidth = pointerWidth;
    spec.endian = endian;
    spec.disableRedZone = parseBool(values, "disable_red_zone", false, errorMessage);
    if (!errorMessage.empty()) {
        return std::nullopt;
    }
    spec.entrySymbol = getValue(values, "entry", getValue(values, "entry_symbol"));
    spec.linkerScript = resolveRelativeTo(path, getValue(values, "linker_script"));
    spec.linkerPath = getValue(values, "linker");
    spec.sysroot = resolveRelativeTo(path, getValue(values, "sysroot"));
    spec.dynamicLinker = getValue(values, "dynamic_linker");
    spec.multiboot2 = parseBool(values, "multiboot2", false, errorMessage);
    if (!errorMessage.empty()) {
        return std::nullopt;
    }
    spec.isWindowsLike = isWindowsLike;
    spec.isApple = isApple;
    spec.isEfi = isEfi;
    spec.isInstantOS = isInstantOS;
    spec.supportsLinuxSyscalls = parseBool(values, "supports_linux_syscalls", false, errorMessage);
    if (!errorMessage.empty()) {
        return std::nullopt;
    }
    spec.freestandingExecutable = parseBool(values, "freestanding", defaultFreestanding, errorMessage);
    if (!errorMessage.empty()) {
        return std::nullopt;
    }
    spec.panicStrategy = lower(getValue(values, "panic"));
    spec.panicHandler = getValue(values, "panic_handler");
    if (!spec.panicStrategy.empty() && spec.panicStrategy != "abort" && spec.panicStrategy != "handler") {
        errorMessage = "target spec panic must be one of: abort, handler";
        return std::nullopt;
    }
    if (spec.panicStrategy == "handler" && spec.panicHandler.empty()) {
        errorMessage = "target spec panic = \"handler\" requires panic_handler";
        return std::nullopt;
    }
    spec.isCustom = true;

    return spec;
}

}
