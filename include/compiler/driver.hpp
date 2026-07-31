#pragma once


#include <memory>
#include <string>
#include <vector>

#include <extra/ast.hpp>
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
    std::vector<Sema::ClassInfo> exportedClasses;
    std::vector<Sema::EnumInfo> exportedEnums;
    // Shared libraries this module's referenced externs asked to link against
    // (via the `lib(...)` directive). Unioned across modules by the driver and
    // forwarded to the linker as `-l<name>`.
    std::vector<std::string> requiredLibs;
    std::shared_ptr<AST::ProgramRoot> ast;
    Sema::SemaResult sema;
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
                     const std::vector<Sema::ClassInfo>& importedClasses,
                     const std::vector<Sema::EnumInfo>& importedEnums,
                     CompiledModule& out,
                     bool emitArtifacts,
                     bool preferHostedEntry = false,
                     const std::vector<AST::ClassDeclaration*>& importedClassTemplates = {},
                     const std::vector<AST::FunctionDeclaration*>& importedFunctionTemplates = {},
                     const std::vector<Sema::SumTypeInfo>& importedSumTypes = {});

    int runCheckOnly();
    int runEmitTokens();
    int runEmitAst();
    // checkOnly stops after every module has been parsed and semantically
    // checked, before any artifact is produced. It still walks the import graph
    // and threads each module's exported symbols into the next, which is the
    // whole point: checking a file in isolation reports every imported name as
    // unknown.
    int runSingleFilePipeline(bool checkOnly = false);

    // Rejects command-line options the selected target cannot act on, so they
    // fail loudly instead of being silently ignored.
    bool validateTargetOptions() const;

    std::string objectPathFor(const std::string& sourcePath) const;
};

}

