#pragma once


#include <memory>
#include <string>
#include <vector>

#include <extra/type_system.hpp>
#include <sema/sema.hpp>
#include <utilities/config.hpp>

namespace Driver {

struct CompiledModule {
    std::string moduleName;
    std::string sourcePath;
    std::string objectPath;
    std::vector<Sema::FunctionInfo> exportedFunctions;
    std::vector<Sema::StructInfo> exportedStructs;
    bool ok = false;
};

class CompilerDriver {
public:
    explicit CompilerDriver(Config::CompilerConfig config);

    int run();

private:
    Config::CompilerConfig config_;
    Types::TypeContext types_;

    bool compileFile(const std::string& path,
                     const std::vector<Sema::FunctionInfo>& imported,
                     const std::vector<Sema::StructInfo>& importedStructs,
                     CompiledModule& out,
                     bool emitArtifacts);

    int runCheckOnly();
    int runEmitTokens();
    int runEmitAst();
    int runSingleFilePipeline();

    std::string objectPathFor(const std::string& sourcePath) const;
};

}
