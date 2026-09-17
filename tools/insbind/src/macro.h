// insbind: macro table and expansion engine (C11 6.10.3).
//
// Object-like, function-like and variadic macros; argument prescan; `#`
// stringize; `##` token paste (re-lexed, must yield exactly one token);
// hideset ("blue paint") recursion prevention; __FILE__/__LINE__ builtins.
// The engine is deliberately independent of the directive driver so it can be
// tested (and later ported) on its own.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "token.h"

namespace insbind {

struct Macro {
    bool functionLike = false;
    bool variadic = false;
    std::vector<std::string> params;   // named parameters (__VA_ARGS__ excluded)
    std::vector<Token> replacement;    // replacement list tokens
};

class MacroTable {
public:
    // Defines or replaces `name`. Returns a warning string on a non-identical
    // redefinition (benign redefinition with identical tokens returns "").
    std::string define(const std::string& name, Macro macro);
    void undef(const std::string& name);
    const Macro* find(const std::string& name) const;
    bool isDefined(const std::string& name) const;

private:
    std::unordered_map<std::string, Macro> map_;
};

// Expands macros in token lines on behalf of the preprocessor driver.
// Diagnostics are appended to `errors` as "file:line: message" strings.
class Expander {
public:
    Expander(MacroTable& table, std::string_view fileName,
             std::vector<std::string>& errors);

    // Expands one logical line. Pure identifiers that name no macro pass
    // through untouched; a function-like macro not followed by '(' is an
    // ordinary identifier.
    std::vector<Token> expandLine(const std::vector<Token>& tokens);

private:
    MacroTable& table_;
    std::string fileName_;
    std::vector<std::string>& errors_;

    std::vector<Token> expandTokens(const std::vector<Token>& tokens,
                                    std::vector<std::string>& hideset);
    // Collects arguments of a function-like invocation starting at the '('
    // token tokens[lparen]. Returns the argument lists and the index just
    // past the closing ')'.
    bool gatherArgs(const Macro& macro, const std::vector<Token>& tokens,
                    std::size_t lparen, std::vector<std::vector<Token>>& args,
                    std::size_t& after, std::uint32_t errLine);
    std::vector<Token> substitute(const Macro& macro,
                                  const std::vector<std::vector<Token>>& args,
                                  std::vector<std::string>& hideset,
                                  std::uint32_t errLine);
    void diagnose(std::uint32_t line, const std::string& msg);
};

} // namespace insbind
