#include <compiler/driver.hpp>
#include <compiler/module_resolver.hpp>

#include <filesystem>
#include <functional>
#include <iostream>
#include <set>
#include <unordered_map>

#include <codegen/codegen.hpp>
#include <lexer/lexer.hpp>
#include <optimizer/optimizer.hpp>
#include <parser/parser.hpp>
#include <sema/sema.hpp>
#include <utilities/errors.hpp>
#include <utilities/linker.hpp>
#include <utilities/utils.hpp>

namespace fs = std::filesystem;

namespace Driver {

CompilerDriver::CompilerDriver(Config::CompilerConfig config) : config_(std::move(config)) {}

std::string CompilerDriver::objectPathFor(const std::string& sourcePath) const {
    fs::path src(sourcePath);
    std::string stem = src.stem().string();
    if (!config_.objectsDir.empty()) {
        std::error_code ec;
        fs::create_directories(config_.objectsDir, ec);
        return (fs::path(config_.objectsDir) / (stem + ".o")).string();
    }
    return (src.parent_path() / (stem + ".o")).string();
}


namespace {

std::shared_ptr<AST::ProgramRoot> parseSource(const std::string& source,
                                              const std::string& path,
                                              Parser& parser) {
    ErrorReporting::initErrorReporter(source, path);
    std::string mutableSource = source;
    auto ast = parser.produceAST(mutableSource);
    return ast;
}

bool drainDiagnostics(bool printOnError) {
    bool hadError = false;
    if (ErrorReporting::globalErrorReporter) {
        hadError = ErrorReporting::globalErrorReporter->hasError();
        if (printOnError && ErrorReporting::globalErrorReporter->hasDiagnostics()) {
            ErrorReporting::globalErrorReporter->printAll();
        }
    }
    return hadError;
}

}


bool CompilerDriver::compileFile(const std::string& path,
                                 const std::vector<Sema::FunctionInfo>& imported,
                                 const std::vector<Sema::StructInfo>& importedStructs,
                                 CompiledModule& out,
                                 bool emitArtifacts) {
    const std::string source = Utilities::readFile(path);
    if (source.empty() && !fs::exists(path)) {
        std::cerr << "error: cannot read '" << path << "'\n";
        return false;
    }

    Parser parser;
    auto ast = parseSource(source, path, parser);
    if (!ast) {
        drainDiagnostics(true);
        ErrorReporting::cleanupErrorReporter();
        return false;
    }

    Sema::Analyzer analyzer(types_, ErrorReporting::globalErrorReporter.get());
    Sema::SemaResult sema = analyzer.analyze(ast, imported, importedStructs);

    out.moduleName = sema.moduleName.empty() ? ast->moduleName : sema.moduleName;
    out.sourcePath = path;
    out.exportedFunctions = sema.functions;
    out.exportedStructs = sema.structs;

    bool hadError = drainDiagnostics(true);
    if (hadError || !sema.ok) {
        ErrorReporting::cleanupErrorReporter();
        return false;
    }

    if (!emitArtifacts) {
        ErrorReporting::cleanupErrorReporter();
        out.ok = true;
        return true;
    }

    for (const auto& imp : imported) {
        bool present = false;
        for (const auto& existing : sema.functions) {
            if (existing.mangledName == imp.mangledName) {
                present = true;
                break;
            }
        }
        if (!present) {
            Sema::FunctionInfo ext = imp;
            ext.isExternal = true;
            ext.decl = nullptr;
            sema.functions.push_back(ext);
        }
    }

    CodeGenerator codegen(config_, types_, ErrorReporting::globalErrorReporter.get());
    if (!codegen.generate(ast, sema)) {
        drainDiagnostics(true);
        ErrorReporting::cleanupErrorReporter();
        return false;
    }

    if (config_.optLevel > 0 && codegen.getModule()) {
        Optimizer::optimizeModule(*codegen.getModule(), config_.optLevel);
    }

    std::string verifyError;
    if (!codegen.verify(verifyError)) {
        std::cerr << "error: LLVM verification failed for module '" << out.moduleName
                  << "':\n" << verifyError << "\n";
        ErrorReporting::cleanupErrorReporter();
        return false;
    }

    if (config_.mode == Config::OutputMode::EmitLlvm) {
        const std::string ir = codegen.emitLLVM();
        std::string outPath = config_.outputFile;
        if (outPath.empty()) {
            outPath = fs::path(path).stem().string() + ".ll";
        }
        if (config_.inputs.size() > 1 || config_.outputFile.empty()) {
            outPath = (config_.objectsDir.empty() ? fs::path(path).parent_path()
                                                  : fs::path(config_.objectsDir)) /
                      (fs::path(path).stem().string() + ".ll");
            if (!config_.objectsDir.empty()) {
                std::error_code ec;
                fs::create_directories(config_.objectsDir, ec);
            }
        }
        if (!Utilities::writeFile(outPath, ir)) {
            std::cerr << "error: could not write '" << outPath << "'\n";
            ErrorReporting::cleanupErrorReporter();
            return false;
        }
        if (config_.verbose) {
            std::cout << "wrote " << outPath << "\n";
        }
        ErrorReporting::cleanupErrorReporter();
        out.ok = true;
        return true;
    }

    out.objectPath = objectPathFor(path);
    if (!codegen.emitObject(out.objectPath)) {
        std::cerr << "error: failed to emit object for '" << path << "'\n";
        ErrorReporting::cleanupErrorReporter();
        return false;
    }
    if (config_.verbose) {
        std::cout << "wrote " << out.objectPath << "\n";
    }

    ErrorReporting::cleanupErrorReporter();
    out.ok = true;
    return true;
}


int CompilerDriver::runEmitTokens() {
    for (const auto& input : config_.inputs) {
        const std::string source = Utilities::readFile(input);
        Lexer lexer;
        auto tokens = lexer.tokenize(source);
        std::cout << "# tokens for " << input << "\n";
        for (const auto& token : tokens) {
            std::cout << stringifyToken(token) << "\n";
        }
    }
    return 0;
}

int CompilerDriver::runEmitAst() {
    int rc = 0;
    for (const auto& input : config_.inputs) {
        CompiledModule mod;
        if (!compileFile(input, {}, {}, mod, false)) {
            rc = 1;
        } else {
            std::cout << "# parsed module '" << mod.moduleName << "' from " << input
                      << " (" << mod.exportedFunctions.size() << " functions)\n";
        }
    }
    return rc;
}

int CompilerDriver::runCheckOnly() {
    int rc = 0;
    for (const auto& input : config_.inputs) {
        CompiledModule mod;
        if (!compileFile(input, {}, {}, mod, false)) {
            rc = 1;
        }
    }
    if (rc == 0 && config_.verbose) {
        std::cout << "check: ok\n";
    }
    return rc;
}

int CompilerDriver::runSingleFilePipeline() {
    if (config_.inputs.empty()) {
        std::cerr << "error: no input files\n";
        return 1;
    }

    ModuleResolver resolver(config_);

    std::vector<std::string> toCompile;
    std::set<std::string> visited;
    std::unordered_map<std::string, bool> isMainInput;

    std::function<bool(const std::string&)> visit = [&](const std::string& path) -> bool {
        std::string canonical;
        {
            std::error_code ec;
            canonical = fs::weakly_canonical(path, ec).string();
            if (canonical.empty()) canonical = path;
        }
        if (visited.count(canonical)) {
            return true;
        }
        visited.insert(canonical);

        const std::string source = Utilities::readFile(canonical);
        if (source.empty() && !fs::exists(canonical)) {
            std::cerr << "error: cannot read '" << canonical << "'\n";
            return false;
        }
        Parser parser;
        ErrorReporting::initErrorReporter(source, canonical);
        std::string mutableSource = source;
        auto ast = parser.produceAST(mutableSource);
        std::vector<std::string> imports = ast ? ast->imports : std::vector<std::string>{};
        ErrorReporting::cleanupErrorReporter();

        if (!config_.noStd) {
            for (const auto& imp : imports) {
                auto resolved = resolver.resolve(imp, canonical);
                if (!resolved) {
                    continue;
                }
                if (!visit(*resolved)) {
                    return false;
                }
            }
        }
        toCompile.push_back(canonical);
        return true;
    };

    for (const auto& input : config_.inputs) {
        if (!visit(input)) {
            return 1;
        }
    }

    std::vector<Sema::FunctionInfo> imported;
    std::vector<Sema::StructInfo> importedStructs;
    std::vector<std::string> objectFiles;
    for (const auto& source : toCompile) {
        CompiledModule mod;
        if (!compileFile(source, imported, importedStructs, mod, true)) {
            return 1;
        }
        for (const auto& fn : mod.exportedFunctions) {
            if (!fn.isExternal) {
                imported.push_back(fn);
            }
        }
        for (const auto& s : mod.exportedStructs) {
            bool present = false;
            for (const auto& existing : importedStructs) {
                if (existing.name == s.name) { present = true; break; }
            }
            if (!present) {
                importedStructs.push_back(s);
            }
        }
        if (!mod.objectPath.empty()) {
            objectFiles.push_back(mod.objectPath);
        }
    }

    if (config_.mode != Config::OutputMode::Executable) {
        return 0;
    }

    Linker::LinkOptions link;
    link.objectFiles = objectFiles;
    link.outputFile = config_.outputFile.empty() ? "a.out" : config_.outputFile;
    link.target = config_.target;
    link.freestanding = true;
    link.rawBinary = config_.rawBinary;
    link.multiboot2 = config_.multiboot2;
    link.verbose = config_.verbose;
    link.linkerPath = config_.linkerPath;
    link.linkerScript = config_.linkerScript;
    link.sysroot = config_.sysroot;
    link.outputFormat = config_.outputFormat;
    link.entrySymbol = config_.entrySymbol.empty() ? config_.target.entrySymbol
                                                   : config_.entrySymbol;

    if (!Linker::linkExecutable(link)) {
        return 1;
    }
    if (config_.verbose) {
        std::cout << "linked " << link.outputFile << "\n";
    }
    return 0;
}

int CompilerDriver::run() {
    switch (config_.mode) {
        case Config::OutputMode::Check:
            return runCheckOnly();
        case Config::OutputMode::EmitTokens:
            return runEmitTokens();
        case Config::OutputMode::EmitAst:
            return runEmitAst();
        case Config::OutputMode::EmitLlvm:
        case Config::OutputMode::Object:
        case Config::OutputMode::Executable:
            return runSingleFilePipeline();
    }
    return 1;
}

}
