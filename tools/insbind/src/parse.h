// insbind: C declaration parser (the declaration subset of C11 6.5-6.7).
//
// Consumes the preprocessor's token stream and produces the CDecl model.
// Function bodies are brace-skipped (static inline is expected in headers);
// statements never appear otherwise. Typedef names feed back into parsing
// (the classic "lexer hack", here a parser-side table), and tag namespaces
// are tracked separately from ordinary identifiers, as C requires.
//
// Deliberate scope lines: no K&R definitions, no C++ (extern "C" errors),
// cv-qualifiers dropped (the FFI has no const model), __attribute__/__declspec
// skipped, _Atomic/_Thread_local dropped with a warning.
#pragma once

#include <vector>

#include "model.h"
#include "token.h"

namespace insbind {

ParseResult parse(const std::vector<Token>& tokens, const ParseOptions& options);

} // namespace insbind
