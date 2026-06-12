#include <compiler/module_resolver.hpp>

#include <filesystem>

#include <utilities/utils.hpp>

namespace fs = std::filesystem;

namespace Driver {

ModuleResolver::ModuleResolver(const Config::CompilerConfig& config) : config_(config) {}

std::vector<std::string> ModuleResolver::searchDirs(const std::string& importingFile) const {
    std::vector<std::string> dirs;
    if (!importingFile.empty()) {
        fs::path parent = fs::path(importingFile).parent_path();
        if (!parent.empty()) {
            dirs.push_back(parent.string());
            dirs.push_back((parent / "src").string());
            dirs.push_back((parent / "lib").string());
        }
    }
    for (const auto& dir : config_.moduleSearchPaths) {
        dirs.push_back(dir);
    }
    if (!config_.noStd && !config_.stdLibDir.empty()) {
        dirs.push_back(config_.stdLibDir);
    }
    return dirs;
}

std::optional<std::string> ModuleResolver::resolve(const std::string& moduleName,
                                                   const std::string& importingFile) const {
    std::string relative = moduleName;
    for (std::string::size_type pos = relative.find("::");
         pos != std::string::npos; pos = relative.find("::", pos + 1)) {
        relative.replace(pos, 2, "/");
    }
    const std::string fileName = relative + ".ins";
    for (const auto& dir : searchDirs(importingFile)) {
        fs::path candidate = fs::path(dir) / fileName;
        std::error_code ec;
        if (fs::exists(candidate, ec) && fs::is_regular_file(candidate, ec)) {
            return fs::weakly_canonical(candidate, ec).string();
        }
    }
    return std::nullopt;
}

}
