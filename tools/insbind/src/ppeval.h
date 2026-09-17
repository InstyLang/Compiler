// insbind: #if / #elif constant-expression evaluation (C11 6.10.1).
//
// Pipeline per the standard: `defined` and `__has_include` forms are rewritten
// to 1/0 BEFORE expansion (their operands must not be expanded), the rest of
// the line is macro-expanded, leftover identifiers become 0, and the result is
// constant-folded with full C precedence in intmax_t/uintmax_t (i64/u64).
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "macro.h"
#include "token.h"

namespace insbind {

struct IfEvalContext {
    MacroTable& macros;
    Expander& expander;
    // Reports whether a header would be found by #include resolution.
    std::function<bool(const std::string& header, bool angled)> includeExists;
    std::vector<std::string>& errors;
    std::string location; // "file:line" prefix for diagnostics
};

// Evaluates the token list following `#if`/`#elif`. On malformed input an
// error is recorded and false is returned (so a bad #if takes no branch).
bool evalIf(const std::vector<Token>& tokens, IfEvalContext& ctx);

} // namespace insbind
