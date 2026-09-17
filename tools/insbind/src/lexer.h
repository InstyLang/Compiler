// insbind: C lexer (translation phase 3).
//
// Produces the token stream the preprocessor and parser consume. Deliberately
// small: no universal-character-names, no '$' identifiers, no raw strings --
// system headers are out of scope (insbind ships its own freestanding headers).
#pragma once

#include <string>
#include <vector>

#include "token.h"

namespace insbind {

// Lexes one source text into tokens, always terminated by a final End token.
// Whitespace and comments are discarded (recorded only in Token flags); '\r'
// is trivia so golden files are stable across CRLF/LF checkouts. Lex errors
// become Error tokens and scanning resumes, so callers see every error.
std::vector<Token> lex(const std::string& source);

// Renders tokens as golden-test text: one `line:col kind spelling` line per
// token, LF newlines, End token omitted. Error tokens print their message.
std::string dumpTokens(const std::vector<Token>& tokens);

} // namespace insbind
