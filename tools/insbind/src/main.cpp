// insbind: C/C++ header -> Insty binding generator.
//
// Dependency-free (C++ std only) so the shipped binary needs nothing
// installed -- the same zero-install rule as the rest of the toolchain.
// Stages land file-by-file (lexer -> preproc -> parse -> layout -> emit) so
// the eventual port to Insty is a file-by-file translation verified against
// this implementation's golden-test output.
//
// Usage:
//   insbind tokens <file.c>            lex and dump tokens (debug/golden view)
//   insbind pp <file.c> [options]      preprocess and dump the token stream
//   insbind self-test <dir> [--bless]  run golden tests under <dir>
//   insbind bind <header> [options]    generate .ins bindings (not implemented)
//
// Shared options for pp/bind:  -I <dir> | -I<dir>   add an include path
//                              -D <name[=value]>    define a macro
//                              --target <name>      builtin predefines set
//                                                   (x86_64_windows|x86_64_linux)

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "emit.h"
#include "lexer.h"
#include "model.h"
#include "parse.h"
#include "preproc.h"
#include "selftest.h"

namespace {

void usage() {
    std::cout <<
        "insbind: C/C++ header -> Insty binding generator\n"
        "\n"
        "  insbind tokens <file.c>            lex and dump tokens\n"
        "  insbind pp <file.c> [options]      preprocess and dump tokens\n"
        "  insbind parse <file.c> [options]   parse and dump the decl model\n"
        "  insbind self-test <dir> [--bless]  run golden tests under <dir>\n"
        "  insbind bind <header> [options]    generate .ins bindings\n"
        "\n"
        "  pp/parse/bind options: -I <dir>  -D <name[=value]>  --target <name>\n";
}

int cmdTokens(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "insbind: cannot open " << path << "\n";
        return 2;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::cout << insbind::dumpTokens(insbind::lex(ss.str()));
    return 0;
}

// Parses shared options starting at argv[from]; bind additionally accepts
// --module/--dll/--lib/--abi-check (collected into `bindOpts` when given).
// Returns false on bad usage.
bool parseSharedOptions(int argc, char** argv, int from,
                        insbind::PreprocessOptions& opts,
                        insbind::EmitOptions* bindOpts = nullptr) {
    auto needValue = [&](int& i, const char* inlineValue,
                         const char* flag) -> const char* {
        if (inlineValue) return inlineValue;
        if (i + 1 >= argc) {
            std::cerr << "insbind: " << flag << " needs a value\n";
            return nullptr;
        }
        return argv[++i];
    };
    for (int i = from; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-I" || arg.rfind("-I", 0) == 0) {
            const char* v =
                needValue(i, arg.size() > 2 ? arg.c_str() + 2 : nullptr, "-I");
            if (!v) return false;
            opts.includePaths.emplace_back(v);
        } else if (arg == "-D" || arg.rfind("-D", 0) == 0) {
            const char* v =
                needValue(i, arg.size() > 2 ? arg.c_str() + 2 : nullptr, "-D");
            if (!v) return false;
            const std::string def = v;
            const std::size_t eq = def.find('=');
            if (eq == std::string::npos)
                opts.defines.emplace_back(def, "1");
            else
                opts.defines.emplace_back(def.substr(0, eq),
                                          def.substr(eq + 1));
        } else if (arg == "--target") {
            const char* v = needValue(i, nullptr, "--target");
            if (!v) return false;
            opts.target = v;
        } else if (bindOpts && arg == "--module") {
            const char* v = needValue(i, nullptr, "--module");
            if (!v) return false;
            bindOpts->moduleName = v;
        } else if (bindOpts && arg == "--dll") {
            const char* v = needValue(i, nullptr, "--dll");
            if (!v) return false;
            bindOpts->linkName = v;
            bindOpts->linkIsDll = true;
        } else if (bindOpts && arg == "--lib") {
            const char* v = needValue(i, nullptr, "--lib");
            if (!v) return false;
            bindOpts->linkName = v;
            bindOpts->linkIsDll = false;
        } else if (bindOpts && arg == "--abi-check") {
            bindOpts->abiCheck = true;
        } else {
            std::cerr << "insbind: unknown option " << arg << "\n";
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string cmd = argv[1];

    if (cmd == "tokens" && argc == 3) {
        return cmdTokens(argv[2]);
    }
    if (cmd == "self-test" && (argc == 3 || argc == 4)) {
        const bool bless = argc == 4 && std::string(argv[3]) == "--bless";
        if (argc == 4 && !bless) {
            usage();
            return 2;
        }
        return insbind::selfTest(argv[2], bless);
    }
    if (cmd == "pp" && argc >= 3) {
        insbind::PreprocessOptions opts;
        opts.mainFile = argv[2];
        if (!parseSharedOptions(argc, argv, 3, opts)) return 2;
        const insbind::PreprocessResult result = insbind::preprocess(opts);
        std::cout << insbind::dumpPreprocessed(result);
        for (const std::string& e : result.errors)
            std::cerr << "error: " << e << "\n";
        for (const std::string& w : result.warnings)
            std::cerr << "warning: " << w << "\n";
        return result.errors.empty() ? 0 : 1;
    }
    if (cmd == "parse" && argc >= 3) {
        insbind::PreprocessOptions opts;
        opts.mainFile = argv[2];
        if (!parseSharedOptions(argc, argv, 3, opts)) return 2;
        const insbind::PreprocessResult pp = insbind::preprocess(opts);
        insbind::ParseOptions popts;
        if (opts.target.find("windows") != std::string::npos) {
            popts.longBits = 32;
            popts.longDoubleBits = 64;
        } else {
            popts.longBits = 64;
            popts.longDoubleBits = 80;
        }
        popts.files = &pp.files;
        insbind::ParseResult model = insbind::parse(pp.tokens, popts);
        std::cout << insbind::dumpModel(model);
        for (const std::string& e : pp.errors)
            std::cerr << "error (preproc): " << e << "\n";
        for (const std::string& e : model.errors)
            std::cerr << "error: " << e << "\n";
        return (pp.errors.empty() && model.errors.empty()) ? 0 : 1;
    }
    if (cmd == "bind" && argc >= 3) {
        insbind::PreprocessOptions opts;
        opts.mainFile = argv[2];
        insbind::EmitOptions emitOpts;
        if (!parseSharedOptions(argc, argv, 3, opts, &emitOpts)) return 2;

        const bool windows = opts.target.find("windows") != std::string::npos;
        const std::string stem =
            std::filesystem::path(opts.mainFile).stem().string();
        if (emitOpts.moduleName.empty()) emitOpts.moduleName = stem;
        if (emitOpts.linkName.empty()) {
            emitOpts.linkIsDll = windows;
            emitOpts.linkName = windows ? stem + ".dll" : stem;
        }
        emitOpts.layout.msvcBitfields = windows;

        const insbind::PreprocessResult pp = insbind::preprocess(opts);
        for (const std::string& e : pp.errors)
            std::cerr << "error (preproc): " << e << "\n";
        if (!pp.errors.empty()) return 1;

        insbind::ParseOptions popts;
        popts.longBits = windows ? 32 : 64;
        popts.longDoubleBits = windows ? 64 : 80;
        popts.files = &pp.files;
        const insbind::ParseResult model = insbind::parse(pp.tokens, popts);
        for (const std::string& e : model.errors)
            std::cerr << "error: " << e << "\n";
        if (!model.errors.empty()) return 1;

        emitOpts.macros = &pp.objectMacros;
        const insbind::EmitResult emitted = insbind::emit(model, emitOpts);
        std::cout << emitted.bindings;
        // Aggregate repeated warning kinds: a header like gl.h produces a
        // thousand identical "extern global skipped" notes, which is spam.
        {
            std::map<std::string, int> counts;
            for (const std::string& w : emitted.warnings) {
                const std::size_t colon = w.find(':');
                counts[colon == std::string::npos ? w : w.substr(0, colon)]++;
            }
            for (const auto& [kind, count] : counts) {
                if (count == 1) {
                    std::cerr << "warning: " << kind << "\n";
                } else {
                    std::cerr << "warning: " << kind << " (x" << count << ")\n";
                }
            }
        }
        if (emitOpts.abiCheck) {
            const std::string path = emitOpts.moduleName + ".abicheck.ins";
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << emitted.abiCheck;
            if (!out) {
                std::cerr << "insbind: cannot write " << path << "\n";
                return 1;
            }
            std::cerr << "insbind: wrote " << path << "\n";
        }
        return 0;
    }

    usage();
    return 2;
}
