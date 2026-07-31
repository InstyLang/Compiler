#include <compiler/driver.hpp>

#include <compiler/comptime.hpp>
#include <compiler/module_resolver.hpp>

#include <filesystem>
#include <functional>
#include <iostream>
#include <set>
#include <unordered_map>

#include <backend/module_emit.hpp>
#include <compiler/runtime_core.hpp>
#include <lexer/lexer.hpp>
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

std::string functionSymbol(const Sema::FunctionInfo& info) {
    return info.mangledName.empty() ? info.name : info.mangledName;
}

bool hasEmittableBody(const Sema::FunctionInfo& info) {
    return info.decl && info.decl->hasBody && !info.isExternal;
}

std::shared_ptr<AST::ProgramRoot>
combineModulesForBackend(const std::vector<CompiledModule>& modules,
                         Sema::SemaResult& combined,
                         std::string& errorOut) {
    combined = Sema::SemaResult{};
    combined.ok = true;
    auto program = std::make_shared<AST::ProgramRoot>();
    if (!modules.empty()) {
        program->moduleName = modules.back().moduleName;
        combined.moduleName = modules.back().moduleName;
    }

    std::unordered_map<std::string, std::size_t> functionBySymbol;
    std::set<std::string> structs;
    std::set<std::string> classes;
    std::set<std::string> enums;
    std::set<std::string> sumTypes;
    std::unordered_map<std::string, std::string> globalOwner;
    std::set<std::string> genericFunctions;
    std::set<std::string> genericClasses;

    auto mergeFunction = [&](const Sema::FunctionInfo& fn) {
        std::string symbol = functionSymbol(fn);
        if (symbol.empty()) {
            symbol = fn.name;
        }
        auto it = functionBySymbol.find(symbol);
        if (it == functionBySymbol.end()) {
            functionBySymbol[symbol] = combined.functions.size();
            combined.functions.push_back(fn);
            return;
        }

        Sema::FunctionInfo& existing = combined.functions[it->second];
        const bool existingBody = hasEmittableBody(existing);
        const bool incomingBody = hasEmittableBody(fn);
        if ((!existingBody && incomingBody) ||
            (existing.isExternal && !fn.isExternal)) {
            existing = fn;
        }
    };

    for (const auto& mod : modules) {
        if (mod.ast) {
            program->imports.insert(program->imports.end(),
                                    mod.ast->imports.begin(), mod.ast->imports.end());
            program->body.insert(program->body.end(),
                                 mod.ast->body.begin(), mod.ast->body.end());
        }

        for (const auto& fn : mod.sema.functions) {
            mergeFunction(fn);
        }
        for (const auto& s : mod.sema.structs) {
            if (structs.insert(s.name).second) {
                combined.structs.push_back(s);
            }
        }
        for (const auto& cls : mod.sema.classes) {
            if (classes.insert(cls.name).second) {
                combined.classes.push_back(cls);
            }
        }
        for (const auto& e : mod.sema.enums) {
            if (enums.insert(e.name).second) {
                combined.enums.push_back(e);
            }
        }
        for (const auto& st : mod.sema.sumTypes) {
            if (sumTypes.insert(st.name).second) {
                combined.sumTypes.push_back(st);
            }
        }
        for (const auto& g : mod.sema.globals) {
            auto it = globalOwner.find(g.name);
            if (it != globalOwner.end()) {
                errorOut = "duplicate global '" + g.name + "' across modules '" +
                           it->second + "' and '" + mod.moduleName + "'";
                return nullptr;
            }
            globalOwner[g.name] = mod.moduleName;
            combined.globals.push_back(g);
        }
        combined.exprTypes.insert(mod.sema.exprTypes.begin(), mod.sema.exprTypes.end());
        combined.callTargets.insert(mod.sema.callTargets.begin(), mod.sema.callTargets.end());
        combined.insizeTypes.insert(mod.sema.insizeTypes.begin(),
                                    mod.sema.insizeTypes.end());
        combined.inalignTypes.insert(mod.sema.inalignTypes.begin(),
                                     mod.sema.inalignTypes.end());

        for (const auto& inst : mod.sema.genericInstantiations) {
            if (genericFunctions.insert(inst.mangledName).second) {
                combined.genericInstantiations.push_back(inst);
            }
        }
        for (const auto& inst : mod.sema.genericClassInstantiations) {
            if (genericClasses.insert(inst.mangledName).second) {
                combined.genericClassInstantiations.push_back(inst);
            }
        }
    }

    return program;
}

}


bool CompilerDriver::compileFile(const std::string& path,
                                 const std::vector<Sema::FunctionInfo>& imported,
                                 const std::vector<Sema::StructInfo>& importedStructs,
                                 const std::vector<Sema::ClassInfo>& importedClasses,
                                 const std::vector<Sema::EnumInfo>& importedEnums,
                                 CompiledModule& out,
                                 bool emitArtifacts,
                                 bool preferHostedEntry,
                                 const std::vector<AST::ClassDeclaration*>& importedClassTemplates,
                                 const std::vector<AST::FunctionDeclaration*>& importedFunctionTemplates,
                                 const std::vector<Sema::SumTypeInfo>& importedSumTypes) {
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

    // Resolve `#if` before analysis, so a branch that was not taken is never
    // type-checked and never reaches code generation. That is what lets one
    // source file hold both a raw syscall and its WASI equivalent.
    {
        std::string comptimeError;
        if (!Comptime::resolve(*ast, config_.target,
                               ErrorReporting::globalErrorReporter.get(),
                               comptimeError)) {
            std::cerr << "error: " << path << ": " << comptimeError << "\n";
            drainDiagnostics(true);
            ErrorReporting::cleanupErrorReporter();
            return false;
        }
    }

    Sema::Analyzer analyzer(types_, ErrorReporting::globalErrorReporter.get());
    Sema::SemaResult sema = analyzer.analyze(ast, imported, importedStructs,
                                             importedClasses, importedEnums,
                                             importedClassTemplates,
                                             importedFunctionTemplates,
                                             importedSumTypes);

    out.moduleName = sema.moduleName.empty() ? ast->moduleName : sema.moduleName;
    out.sourcePath = path;
    out.exportedFunctions = sema.functions;
    out.exportedStructs = sema.structs;
    out.exportedClasses = sema.classes;
    out.exportedEnums = sema.enums;
    out.ast = ast;
    out.sema = sema;

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

    out.objectPath = objectPathFor(path);
    const Backend::Abi abi = config_.target.isWindowsLike ? Backend::Abi::Win64
                                                          : Backend::Abi::SystemV;
    const Sema::SemaResult* runtimeModule = getCoreRuntimeModule(types_);
    Backend::EntryShim shim = Backend::EntryShim::None;
    if (config_.target.isWasmModule()) {
        // A WASI command module exports `_start`; a bare wasm32-unknown-unknown
        // module has no entry at all and only exports its public functions.
        shim = config_.target.freestandingExecutable ? Backend::EntryShim::None
                                                     : Backend::EntryShim::Wasi;
    } else if (config_.target.isEfi) {
        shim = Backend::EntryShim::Efi;
    } else if (config_.target.isInstantOS) {
        // InstantOS userland is a dynamic PIE linked against mlibc: crt1.o
        // provides `_start` and calls our `main`, so we emit no entry shim.
        shim = Backend::EntryShim::None;
    } else if (config_.mode == Config::OutputMode::Executable &&
               !config_.target.isMachO()) {
        shim = config_.target.isCoff() ? Backend::EntryShim::Pe
                                       : Backend::EntryShim::Elf;
    }
    // isWasmModule() must be tested first: a wasm target is none of
    // isCoff/isMachO/isElf, so it would otherwise fall through to Elf.
    const Backend::ObjectFormat fmt = config_.target.isWasmModule()
                                          ? Backend::ObjectFormat::Wasm
                                      : config_.target.isCoff()
                                          ? Backend::ObjectFormat::Coff
                                      : config_.target.isMachO()
                                          ? Backend::ObjectFormat::MachO
                                          : Backend::ObjectFormat::Elf;
    std::string err;
    // Use the hardware-AES string hash on hosted x86-64 targets. Freestanding /
    // EFI builds keep the portable hash (AES-NI/SSE may be unavailable or not set
    // up), as does wasm, which has no AESENC at all.
    const bool aesHash = !config_.freestanding &&
                         !config_.target.freestandingExecutable &&
                         !config_.target.isEfi &&
                         !config_.target.isWasmModule();
    if (!Backend::emitModuleObject(sema, abi, fmt, out.objectPath, err,
                                   /*entrySymbol=*/"main", ast.get(), runtimeModule,
                                   shim, config_.optLevel,
                                   /*instantOsSyscalls=*/config_.target.isInstantOS,
                                   preferHostedEntry, &out.requiredLibs,
                                   config_.boundsCheck, aesHash, &config_.target)) {
        std::cerr << "error: backend failed for '" << path << "': " << err << "\n";
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
    // Same reasoning as runCheckOnly: analyzing each input in isolation leaves
    // every imported name unresolved, so this reported a spurious error per call
    // into any imported module.
    return runSingleFilePipeline(/*checkOnly=*/true, /*printModuleSummary=*/true);
}

int CompilerDriver::runCheckOnly() {
    // Delegates to the real pipeline. Checking each input on its own would leave
    // every imported name unresolved, so a valid program that uses the standard
    // library would report an error per call into it.
    return runSingleFilePipeline(/*checkOnly=*/true);
}

int CompilerDriver::runSingleFilePipeline(bool checkOnly, bool printModuleSummary) {
    if (config_.inputs.empty()) {
        std::cerr << "error: no input files\n";
        return 1;
    }

    ModuleResolver resolver(config_);

    std::vector<std::string> toCompile;
    std::set<std::string> visited;

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
        // Resolve compile-time conditionals before reading the import list, so a
        // dependency inside a branch this target does not take is never fetched.
        // Errors are left to the real compile below, which reports them properly.
        if (ast) {
            std::string ignored;
            Comptime::resolve(*ast, config_.target,
                              ErrorReporting::globalErrorReporter.get(), ignored);
        }
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

    const bool wholeProgramPe = config_.mode == Config::OutputMode::Executable &&
                                config_.target.isCoff();
    // A wasm module is self-contained: there is no relocatable-object stage and
    // no linker, so -- exactly like the whole-program PE path -- the backend
    // writes the final artifact in one shot from the combined program.
    const bool wholeProgramWasm = config_.mode == Config::OutputMode::Executable &&
                                  config_.target.isWasmModule();
    const bool wholeProgram = wholeProgramPe || wholeProgramWasm;

    std::vector<Sema::FunctionInfo> imported;
    std::vector<Sema::StructInfo> importedStructs;
    std::vector<Sema::ClassInfo> importedClasses;
    std::vector<Sema::EnumInfo> importedEnums;
    // Exported generic templates (cross-module generics). The AST pointers stay
    // valid because every compiled module (and its ProgramRoot) is retained in
    // `compiledModules` for the lifetime of the build.
    std::vector<AST::ClassDeclaration*> importedClassTemplates;
    std::vector<AST::FunctionDeclaration*> importedFunctionTemplates;
    std::vector<Sema::SumTypeInfo> importedSumTypes;
    std::vector<std::string> objectFiles;
    std::vector<CompiledModule> compiledModules;
    // Shared libraries requested via `lib(...)` across all compiled modules.
    // Dependencies are compiled before the modules that import them, so by the
    // time the `main`-bearing module is compiled this already reflects its
    // dependencies' needs (used to suppress the ELF _start shim for a hosted
    // dynamic link). The module's own referenced externs are folded in by the
    // backend during its codegen as well.
    std::set<std::string> requiredLibSet;
    for (const auto& source : toCompile) {
        CompiledModule mod;
        if (!compileFile(source, imported, importedStructs,
                         importedClasses, importedEnums, mod,
                         /*emitArtifacts=*/!wholeProgram && !checkOnly,
                         /*preferHostedEntry=*/!requiredLibSet.empty(),
                         importedClassTemplates, importedFunctionTemplates,
                         importedSumTypes)) {
            return 1;
        }
        if (printModuleSummary) {
            std::cout << "# parsed module '" << mod.moduleName << "' from " << source
                      << " (" << mod.exportedFunctions.size() << " functions)\n";
        }
        for (const auto& lib : mod.requiredLibs) {
            requiredLibSet.insert(lib);
        }
        for (const auto& fn : mod.exportedFunctions) {
            // Private-by-default: only `export`ed symbols are visible to modules
            // that import this one. Exported externs propagate as external decls.
            if (fn.isExported) {
                imported.push_back(fn);
            }
        }
        for (const auto& s : mod.exportedStructs) {
            if (!s.isExported) {
                continue;
            }
            bool present = false;
            for (const auto& existing : importedStructs) {
                if (existing.name == s.name) { present = true; break; }
            }
            if (!present) {
                importedStructs.push_back(s);
            }
        }
        for (const auto& c : mod.exportedClasses) {
            if (!c.isExported) {
                continue;
            }
            bool present = false;
            for (const auto& existing : importedClasses) {
                if (existing.name == c.name) { present = true; break; }
            }
            if (!present) {
                importedClasses.push_back(c);
            }
        }
        for (const auto& e : mod.exportedEnums) {
            if (!e.isExported) {
                continue;
            }
            bool present = false;
            for (const auto& existing : importedEnums) {
                if (existing.name == e.name) { present = true; break; }
            }
            if (!present) {
                importedEnums.push_back(e);
            }
        }
        // Propagate exported generic templates so later modules can instantiate
        // them (cross-module generics). The template ASTs live in `mod.ast`, kept
        // alive in `compiledModules` below.
        for (AST::ClassDeclaration* tmpl : mod.sema.genericClassTemplates) {
            if (!tmpl || !tmpl->isExported) continue;
            bool present = false;
            for (const auto* existing : importedClassTemplates) {
                if (existing && existing->name == tmpl->name) { present = true; break; }
            }
            if (!present) importedClassTemplates.push_back(tmpl);
        }
        for (AST::FunctionDeclaration* tmpl : mod.sema.genericFunctionTemplates) {
            if (!tmpl || !tmpl->isExported) continue;
            bool present = false;
            for (const auto* existing : importedFunctionTemplates) {
                if (existing && existing->name == tmpl->name) { present = true; break; }
            }
            if (!present) importedFunctionTemplates.push_back(tmpl);
        }
        // Propagate exported tagged-union metadata so importers can construct /
        // match their variants (cross-module sum types).
        for (const auto& st : mod.sema.sumTypes) {
            if (!st.isExported) continue;
            bool present = false;
            for (const auto& existing : importedSumTypes) {
                if (existing.name == st.name) { present = true; break; }
            }
            if (!present) importedSumTypes.push_back(st);
        }
        if (!mod.objectPath.empty()) {
            objectFiles.push_back(mod.objectPath);
        }
        compiledModules.push_back(std::move(mod));
    }

    // --check stops here: every module has been parsed and type-checked with its
    // dependencies' exports in scope, which is all that was asked for. Nothing
    // below this point produces diagnostics, only artifacts.
    if (checkOnly) {
        if (config_.verbose) {
            std::cout << "check: ok\n";
        }
        return 0;
    }

    if (config_.mode != Config::OutputMode::Executable) {
        return 0;
    }
    if (wholeProgramWasm) {
        Sema::SemaResult combined;
        std::string combineError;
        auto combinedProgram =
            combineModulesForBackend(compiledModules, combined, combineError);
        if (!combinedProgram) {
            std::cerr << "error: " << combineError << "\n";
            return 1;
        }
        const std::string outPath =
            config_.outputFile.empty() ? "a.wasm" : config_.outputFile;
        std::string err;
        if (!Backend::emitModuleObject(combined, Backend::Abi::SystemV,
                                       Backend::ObjectFormat::Wasm, outPath, err,
                                       /*entrySymbol=*/"main", combinedProgram.get(),
                                       getCoreRuntimeModule(types_),
                                       Backend::EntryShim::Wasi, config_.optLevel,
                                       /*instantOsSyscalls=*/false,
                                       /*preferHostedEntry=*/false,
                                       /*requiredLibsOut=*/nullptr,
                                       config_.boundsCheck, /*aesHash=*/false,
                                       &config_.target)) {
            std::cerr << "error: wasm backend failed: " << err << "\n";
            return 1;
        }
        if (config_.verbose) {
            std::cout << "wrote " << outPath << " (WebAssembly module)\n";
        }
        return 0;
    }
    if (wholeProgramPe) {
        Sema::SemaResult combined;
        std::string combineError;
        auto combinedProgram =
            combineModulesForBackend(compiledModules, combined, combineError);
        if (!combinedProgram) {
            std::cerr << "error: " << combineError << "\n";
            return 1;
        }
        const Backend::Abi abi = config_.target.isWindowsLike ? Backend::Abi::Win64
                                                              : Backend::Abi::SystemV;
        const Sema::SemaResult* runtimeModule = getCoreRuntimeModule(types_);
        const std::string entry =
            config_.entrySymbol.empty()
                ? (config_.target.entrySymbol.empty() ? "main" : config_.target.entrySymbol)
                : config_.entrySymbol;
        const std::string exePath =
            config_.outputFile.empty() ? "a.exe" : config_.outputFile;
        std::string err;
        const bool peAesHash = !config_.freestanding &&
                               !config_.target.freestandingExecutable &&
                               !config_.target.isEfi;
        if (!Backend::emitModuleObject(combined, abi, Backend::ObjectFormat::Pe,
                                       exePath, err, entry, combinedProgram.get(),
                                       runtimeModule, Backend::EntryShim::Pe,
                                       config_.optLevel, /*instantOsSyscalls=*/false,
                                       /*preferHostedEntry=*/false,
                                       /*requiredLibsOut=*/nullptr,
                                       config_.boundsCheck, peAesHash,
                                       &config_.target)) {
            std::cerr << "error: custom backend (whole-program PE) failed: "
                      << err << "\n";
            return 1;
        }
        if (config_.verbose) {
            std::cout << "wrote " << exePath << " (whole-program PE executable)\n";
        }
        return 0;
    }

    Linker::LinkOptions link;
    link.objectFiles = objectFiles;
    link.outputFile = config_.outputFile.empty() ? "a.out" : config_.outputFile;
    link.target = config_.target;
    // Any module that referenced a `lib(...)`-tagged extern forces a hosted,
    // dynamically-linked build (system crt + libc + the requested .so files).
    const bool needsSharedLibraries = !requiredLibSet.empty();
    link.libraries.assign(requiredLibSet.begin(), requiredLibSet.end());
    // InstantOS userland is a hosted, dynamically-linked PIE (mlibc + crt1.o +
    // ld-instantos.so), so it must NOT be linked freestanding: the linker needs
    // to pull in crt1.o, libc and embed the dynamic-linker PT_INTERP. A plain
    // Linux target is otherwise freestanding+static (syscalls only) unless it
    // needs shared libraries, in which case it becomes a hosted dynamic link.
    link.freestanding = !config_.target.isInstantOS && !needsSharedLibraries;
    link.rawBinary = config_.rawBinary;
    link.multiboot2 = config_.multiboot2;
    link.verbose = config_.verbose;
    link.linkerPath = config_.linkerPath;
    link.linkerScript = config_.linkerScript;
    link.sysroot = config_.sysroot;
    link.outputFormat = config_.outputFormat;
    if (config_.target.isCoff() && config_.mode == Config::OutputMode::Executable) {
        link.entrySymbol = config_.entrySymbol.empty() ? "_start" : config_.entrySymbol;
        if (link.entrySymbol == "main") {
            link.entrySymbol = "_start";
        }
    } else {
        link.entrySymbol = config_.entrySymbol.empty() ? config_.target.entrySymbol
                                                       : config_.entrySymbol;
    }

    if (!Linker::linkExecutable(link)) {
        return 1;
    }
    if (config_.verbose) {
        std::cout << "linked " << link.outputFile << "\n";
    }
    return 0;
}

// Rejects options the target cannot act on, rather than accepting them and
// quietly producing something the user did not ask for. All of these describe
// details of a native image -- a boot header, a flat binary, a link script --
// that a wasm module has no room for.
bool CompilerDriver::validateTargetOptions() const {
    if (!config_.target.isWasmModule()) return true;

    struct Rejection {
        bool active;
        const char* flag;
        const char* why;
    };
    const Rejection rejections[] = {
        {config_.multiboot2, "--multiboot2",
         "a Multiboot2 header is an x86 boot protocol; nothing loads a wasm module "
         "that way"},
        {config_.rawBinary, "--raw-binary",
         "a wasm module is already a self-contained container and cannot be reduced "
         "to a flat image"},
        {!config_.linkerScript.empty(), "--linker-script",
         "wasm modules are linked at compile time by this backend, so no linker "
         "script is consulted"},
    };
    for (const auto& r : rejections) {
        if (!r.active) continue;
        std::cerr << "error: " << r.flag << " is not supported for '"
                  << config_.target.cliName << "': " << r.why << "\n";
        return false;
    }
    return true;
}

int CompilerDriver::run() {
    if (!validateTargetOptions()) {
        return 1;
    }
    switch (config_.mode) {
        case Config::OutputMode::Check:
            return runCheckOnly();
        case Config::OutputMode::EmitTokens:
            return runEmitTokens();
        case Config::OutputMode::EmitAst:
            return runEmitAst();
        case Config::OutputMode::Object:
            // A wasm module is self-contained and there is no wasm linker in the
            // toolchain, so a per-module object would have nothing to combine it
            // with. Say so rather than emitting a file that cannot be used.
            if (config_.target.isWasmModule()) {
                std::cerr << "error: '-c' is not supported for '"
                          << config_.target.cliName
                          << "': a WebAssembly module is self-contained and is "
                             "linked at compile time, so there are no separate "
                             "object files. Build an executable instead (drop -c "
                             "and pass -o <file>.wasm).\n";
                return 1;
            }
            return runSingleFilePipeline();
        case Config::OutputMode::Executable:
            return runSingleFilePipeline();
    }
    return 1;
}

}


