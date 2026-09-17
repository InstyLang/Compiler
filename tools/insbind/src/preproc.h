// insbind: preprocessor driver (C11 5.1.1.2 translation phase 4, with phase 2
// line splicing).
//
// Turns a header file into a single flat token stream: directives executed,
// conditionals resolved, macros expanded, #include inlined. The parser stage
// consumes the result; a file table keeps every token's origin for
// diagnostics.
//
// Scope decisions (documented in the project plan): no #include_next, no
// _Pragma, unknown #pragmas ignored, GNU extensions (`, ## __VA_ARGS__`,
// #import) rejected or ignored rather than emulated.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "token.h"

namespace insbind {

struct PreprocessOptions {
    std::string mainFile;
    std::vector<std::string> includePaths;                  // searched for <...>
    std::vector<std::pair<std::string, std::string>> defines; // -D NAME[=VALUE]
    std::string target = "x86_64_windows";                  // builtin predefines
};

struct PreprocessResult {
    std::vector<Token> tokens;        // expanded output; Token::file indexes files
    std::vector<std::string> files;   // file table (index 0 = main file)
    std::vector<std::string> errors;  // "file:line: message"
    std::vector<std::string> warnings;
    // Object-like macros defined in the MAIN file (not system/vendored
    // headers, not -D flags), in definition order. The bind stage turns the
    // constant ones into accessor functions; function-like macros are out of
    // scope by design.
    std::vector<std::pair<std::string, std::vector<Token>>> objectMacros;
};

PreprocessResult preprocess(const PreprocessOptions& options);

// Renders the result for golden tests and debugging: `#file "<name>"` marker
// lines on file changes (filename only, so goldens are machine-independent),
// `line:col kind spelling` per token, then `! <error>` / `!warn <warning>`
// lines for diagnostics.
std::string dumpPreprocessed(const PreprocessResult& result);

} // namespace insbind
