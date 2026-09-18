#include "selftest.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

#include "emit.h"
#include "lexer.h"
#include "model.h"
#include "parse.h"
#include "preproc.h"

namespace insbind {
namespace {

std::string readFile(const std::filesystem::path& path, bool& ok) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ok = false;
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    ok = true;
    return ss.str();
}

bool writeFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << content;
    return static_cast<bool>(out);
}

// Line endings are normalized so a CRLF checkout cannot fail tests whose
// goldens were written LF (and vice versa).
std::string normalizeNewlines(std::string text) {
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

// Prints the first line on which expected/actual differ.
void showFirstMismatch(const std::string& expected, const std::string& actual) {
    std::istringstream exp(expected), act(actual);
    std::string e, a;
    int line = 0;
    for (;;) {
        ++line;
        const bool eok = static_cast<bool>(std::getline(exp, e));
        const bool aok = static_cast<bool>(std::getline(act, a));
        if (!eok && !aok) return; // only trailing whitespace differed
        if (eok && aok && e == a) continue;
        std::cout << "      line " << line << ":\n";
        std::cout << "      expected: " << (eok ? e : "<end of output>") << "\n";
        std::cout << "      actual:   " << (aok ? a : "<end of output>") << "\n";
        return;
    }
}

// The test mode is selected by the name of the directory holding the .c file:
//   lexer/   -> lex only, golden <name>.tokens
//   preproc/ -> full preprocess, golden <name>.pp
//   parse/   -> preprocess + parse, golden <name>.model
//   emit/    -> full pipeline, golden <name>.ins (+ <name>.abicheck.ins)
// Preproc/parse/emit conventions: a case-local include/ directory is searched
// first, then the vendored freestanding headers (<tests root>/../include);
// an optional sibling <name>.defs adds -D lines (NAME or NAME=VALUE). Emit
// cases read options from a sibling <name>.bind: `module=x`, `dll=y`,
// `lib=z`, `abi-check=1` (one per line).
struct CaseMode {
    enum Kind { Lex, Preproc, Parse, Emit } kind;
    std::filesystem::path golden;
};

CaseMode caseMode(const std::filesystem::path& cfile,
                  const std::filesystem::path& testsRoot) {
    const std::string dirName = cfile.parent_path().filename().string();
    if (dirName == "preproc")
        return {CaseMode::Preproc,
                std::filesystem::path(cfile).replace_extension(".pp")};
    if (dirName == "parse")
        return {CaseMode::Parse,
                std::filesystem::path(cfile).replace_extension(".model")};
    if (dirName == "emit")
        return {CaseMode::Emit,
                std::filesystem::path(cfile).replace_extension(".ins")};
    (void)testsRoot;
    return {CaseMode::Lex,
            std::filesystem::path(cfile).replace_extension(".tokens")};
}

PreprocessOptions buildPreprocOptions(const std::filesystem::path& cfile,
                                      const std::filesystem::path& testsRoot) {
    PreprocessOptions opts;
    opts.mainFile = cfile.string();
    const std::filesystem::path caseInclude = cfile.parent_path() / "include";
    if (std::filesystem::exists(caseInclude))
        opts.includePaths.push_back(caseInclude.string());
    const std::filesystem::path vendored = testsRoot / ".." / "include";
    if (std::filesystem::exists(vendored))
        opts.includePaths.push_back(
            std::filesystem::weakly_canonical(vendored).string());
    const std::filesystem::path defs =
        std::filesystem::path(cfile).replace_extension(".defs");
    if (std::filesystem::exists(defs)) {
        bool ok = false;
        std::istringstream in(readFile(defs, ok));
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            const std::size_t eq = line.find('=');
            if (eq == std::string::npos)
                opts.defines.emplace_back(line, "1");
            else
                opts.defines.emplace_back(line.substr(0, eq),
                                          line.substr(eq + 1));
        }
    }
    return opts;
}

ParseOptions parseOptionsFor(const std::string& target) {
    ParseOptions opts;
    if (target.find("windows") != std::string::npos) {
        opts.longBits = 32;      // LLP64
        opts.longDoubleBits = 64;
    } else {
        opts.longBits = 64;      // LP64
        opts.longDoubleBits = 80;
    }
    return opts;
}

std::string runCase(const std::filesystem::path& cfile,
                    const std::filesystem::path& testsRoot, CaseMode mode,
                    std::string* secondArtifact = nullptr) {
    bool ok = false;
    const std::string source = readFile(cfile, ok);
    if (!ok) return {};
    if (mode.kind == CaseMode::Lex) return dumpTokens(lex(source));

    const PreprocessOptions opts = buildPreprocOptions(cfile, testsRoot);
    PreprocessResult pp = preprocess(opts);
    if (mode.kind == CaseMode::Preproc) return dumpPreprocessed(pp);

    ParseOptions popts = parseOptionsFor(opts.target);
    popts.files = &pp.files;
    ParseResult model = parse(pp.tokens, popts);
    for (const std::string& e : pp.errors)
        model.errors.insert(model.errors.begin(), "preproc: " + e);
    if (mode.kind == CaseMode::Parse) return dumpModel(model);

    // Emit mode: options from the sibling .bind file.
    EmitOptions emitOpts;
    emitOpts.moduleName = cfile.stem().string();
    emitOpts.linkName = emitOpts.moduleName + ".dll";
    emitOpts.layout.msvcBitfields =
        opts.target.find("windows") != std::string::npos;
    emitOpts.linkIsDll = emitOpts.layout.msvcBitfields;
    const std::filesystem::path bindFile =
        std::filesystem::path(cfile).replace_extension(".bind");
    if (std::filesystem::exists(bindFile)) {
        std::istringstream in(readFile(bindFile, ok));
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const std::size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = line.substr(0, eq);
            const std::string value = line.substr(eq + 1);
            if (key == "module") emitOpts.moduleName = value;
            else if (key == "dll") { emitOpts.linkName = value; emitOpts.linkIsDll = true; }
            else if (key == "lib") { emitOpts.linkName = value; emitOpts.linkIsDll = false; }
            else if (key == "abi-check") emitOpts.abiCheck = value == "1";
        }
    }
    emitOpts.macros = &pp.objectMacros;
    const EmitResult emitted = emit(model, emitOpts);
    std::string out = emitted.bindings;
    for (const std::string& w : emitted.warnings) out += "!warn " + w + "\n";
    if (secondArtifact && emitOpts.abiCheck)
        *secondArtifact = emitted.abiCheck;
    return out;
}

} // namespace

int selfTest(const std::string& dir, bool bless) {
    namespace fs = std::filesystem;

    std::vector<fs::path> cases;
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".c")
            cases.push_back(entry.path());
    }
    std::sort(cases.begin(), cases.end());

    if (cases.empty()) {
        std::cerr << "insbind self-test: no .c files found under " << dir << "\n";
        return 2;
    }

    const fs::path testsRoot = fs::path(dir);
    int passed = 0, failed = 0, blessed = 0;
    for (const fs::path& cfile : cases) {
        const CaseMode mode = caseMode(cfile, testsRoot);
        std::string abiCheck;
        const std::string actual = runCase(cfile, testsRoot, mode, &abiCheck);

        std::vector<std::pair<fs::path, const std::string*>> artifacts;
        artifacts.emplace_back(mode.golden, &actual);
        fs::path abiGolden;
        if (!abiCheck.empty()) {
            abiGolden = mode.golden;
            abiGolden.replace_extension(".abicheck.ins");
            artifacts.emplace_back(abiGolden, &abiCheck);
        }

        bool caseFailed = false;
        bool ok = false;
        for (const auto& [golden, content] : artifacts) {
            if (bless) {
                if (!writeFile(golden, *content)) {
                    std::cout << "FAIL  " << cfile << " (cannot write "
                              << golden << ")\n";
                    caseFailed = true;
                }
                continue;
            }
            const std::string expected = readFile(golden, ok);
            if (!ok) {
                std::cout << "FAIL  " << cfile << " (missing golden: "
                          << golden << "; run with --bless to create)\n";
                caseFailed = true;
                continue;
            }
            if (normalizeNewlines(expected) != *content) {
                std::cout << "FAIL  " << cfile << " (" << golden.filename()
                          << ")\n";
                showFirstMismatch(normalizeNewlines(expected), *content);
                caseFailed = true;
            }
        }

        if (bless) {
            if (caseFailed) ++failed;
            else {
                std::cout << "bless " << cfile << "\n";
                ++blessed;
            }
        } else if (caseFailed) {
            ++failed;
        } else {
            ++passed;
        }
    }

    std::cout << "insbind self-test: ";
    if (bless) {
        std::cout << blessed << " blessed, " << failed << " failed\n";
    } else {
        std::cout << passed << " passed, " << failed << " failed (of "
                  << (passed + failed) << ")\n";
    }
    return failed == 0 ? 0 : 1;
}

} // namespace insbind
