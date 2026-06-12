#pragma once


#include <optional>
#include <string>
#include <vector>

#include <utilities/config.hpp>

namespace Driver {

class ModuleResolver {
public:
    explicit ModuleResolver(const Config::CompilerConfig& config);

    std::optional<std::string> resolve(const std::string& moduleName,
                                       const std::string& importingFile) const;

private:
    const Config::CompilerConfig& config_;
    std::vector<std::string> searchDirs(const std::string& importingFile) const;
};

}
