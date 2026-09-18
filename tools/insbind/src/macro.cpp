#include "macro.h"

#include "lexer.h"

namespace insbind {

// ---------------------------------------------------------------------------
// MacroTable
// ---------------------------------------------------------------------------

namespace {

bool sameTokens(const std::vector<Token>& a, const std::vector<Token>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].kind != b[i].kind || a[i].spelling != b[i].spelling)
            return false;
    return true;
}

} // namespace

std::string MacroTable::define(const std::string& name, Macro macro) {
    const auto it = map_.find(name);
    std::string warning;
    if (it != map_.end()) {
        const Macro& old = it->second;
        if (old.functionLike != macro.functionLike ||
            old.variadic != macro.variadic || old.params != macro.params ||
            !sameTokens(old.replacement, macro.replacement)) {
            warning = "macro '" + name + "' redefined";
        }
    }
    map_[name] = std::move(macro);
    return warning;
}

void MacroTable::undef(const std::string& name) { map_.erase(name); }

const Macro* MacroTable::find(const std::string& name) const {
    const auto it = map_.find(name);
    return it == map_.end() ? nullptr : &it->second;
}

bool MacroTable::isDefined(const std::string& name) const {
    return map_.find(name) != map_.end();
}

// ---------------------------------------------------------------------------
// Expander
// ---------------------------------------------------------------------------

namespace {

bool inHideset(const std::vector<std::string>& hideset, const std::string& name) {
    for (const auto& h : hideset)
        if (h == name) return true;
    return false;
}

Token literal(TokenKind kind, const std::string& spelling, const Token& at) {
    Token t = at;
    t.kind = kind;
    t.spelling = spelling;
    return t;
}

// Stringize (6.10.3.2): the unexpanded argument spelling, whitespace runs
// collapsed to single spaces, with " and \ escaped.
std::string stringize(const std::vector<Token>& arg) {
    std::string out = "\"";
    bool first = true;
    for (const Token& t : arg) {
        if (!first) out += ' ';
        first = false;
        for (const char c : t.spelling) {
            if (c == '"' || c == '\\') out += '\\';
            out += c;
        }
    }
    out += '"';
    return out;
}

} // namespace

Expander::Expander(MacroTable& table, std::string_view fileName,
                   std::vector<std::string>& errors)
    : table_(table), fileName_(fileName), errors_(errors) {}

void Expander::diagnose(std::uint32_t line, const std::string& msg) {
    errors_.push_back(fileName_ + ":" + std::to_string(line) + ": " + msg);
}

std::vector<Token> Expander::expandLine(const std::vector<Token>& tokens) {
    std::vector<std::string> hideset;
    return expandTokens(tokens, hideset);
}

Expander::Expansion Expander::expandLineEx(const std::vector<Token>& tokens) {
    needsMore_ = false;
    std::vector<std::string> hideset;
    Expansion ex;
    ex.tokens = expandTokens(tokens, hideset);
    ex.needsMore = needsMore_;
    return ex;
}

std::vector<Token> Expander::expandTokens(const std::vector<Token>& tokens,
                                          std::vector<std::string>& hideset) {
    std::vector<Token> out;
    std::size_t i = 0;
    while (i < tokens.size()) {
        const Token& t = tokens[i];
        if (t.kind != TokenKind::Identifier ||
            inHideset(hideset, t.spelling)) {
            out.push_back(t);
            ++i;
            continue;
        }

        // Builtins.
        if (t.spelling == "__FILE__") {
            out.push_back(literal(TokenKind::StringLiteral,
                                  "\"" + fileName_ + "\"", t));
            ++i;
            continue;
        }
        if (t.spelling == "__LINE__") {
            out.push_back(literal(TokenKind::IntLiteral,
                                  std::to_string(t.line), t));
            ++i;
            continue;
        }

        const Macro* m = table_.find(t.spelling);
        if (!m) {
            out.push_back(t);
            ++i;
            continue;
        }

        if (!m->functionLike) {
            hideset.push_back(t.spelling);
            std::vector<Token> expanded = expandTokens(m->replacement, hideset);
            hideset.pop_back();
            out.insert(out.end(), expanded.begin(), expanded.end());
            ++i;
            continue;
        }

        // Function-like: only a call when '(' follows.
        if (i + 1 >= tokens.size() || tokens[i + 1].kind != TokenKind::LParen) {
            out.push_back(t);
            ++i;
            continue;
        }

        std::vector<std::vector<Token>> args;
        std::size_t after = 0;
        if (!gatherArgs(*m, tokens, i + 1, args, after, t.line)) {
            out.push_back(t); // malformed call: leave the identifier, move on
            ++i;
            continue;
        }
        hideset.push_back(t.spelling);
        std::vector<Token> sub = substitute(*m, args, hideset, t.line);
        std::vector<Token> expanded = expandTokens(sub, hideset);
        hideset.pop_back();
        out.insert(out.end(), expanded.begin(), expanded.end());
        i = after;
    }
    return out;
}

bool Expander::gatherArgs(const Macro& macro, const std::vector<Token>& tokens,
                          std::size_t lparen,
                          std::vector<std::vector<Token>>& args,
                          std::size_t& after, std::uint32_t errLine) {
    args.clear();
    std::vector<Token> current;
    int depth = 0;
    bool sawAny = false;
    std::size_t i = lparen;
    for (; i < tokens.size(); ++i) {
        const Token& t = tokens[i];
        if (t.kind == TokenKind::LParen) {
            ++depth;
            if (depth > 1) {
                current.push_back(t);
                sawAny = true;
            }
        } else if (t.kind == TokenKind::RParen) {
            --depth;
            if (depth == 0) {
                ++i;
                break;
            }
            current.push_back(t);
            sawAny = true;
        } else if (t.kind == TokenKind::Comma && depth == 1) {
            args.push_back(current);
            current.clear();
            sawAny = false;
        } else {
            current.push_back(t);
            sawAny = true;
        }
    }
    if (depth != 0) {
        // The token stream ends inside the invocation: it may continue on the
        // next physical line. Report up (no diagnostic yet) so the driver can
        // append more tokens and retry; only EOF makes it a hard error.
        needsMore_ = true;
        return false;
    }
    // F() supplies zero arguments to a zero-parameter macro, one (empty)
    // argument otherwise.
    if (!current.empty() || sawAny || !args.empty()) args.push_back(current);
    const std::size_t required = macro.params.size();
    if (macro.variadic ? args.size() < required : args.size() != required) {
        diagnose(errLine, "macro argument count mismatch (expected " +
                              std::to_string(required) +
                              (macro.variadic ? "+)" : ")"));
        return false;
    }
    after = i;
    return true;
}

std::vector<Token> Expander::substitute(
    const Macro& macro, const std::vector<std::vector<Token>>& args,
    std::vector<std::string>& hideset, std::uint32_t errLine) {
    auto paramIndex = [&](const Token& t) -> long long {
        if (t.kind != TokenKind::Identifier) return -1;
        if (macro.variadic && t.spelling == "__VA_ARGS__")
            return static_cast<long long>(macro.params.size());
        for (std::size_t p = 0; p < macro.params.size(); ++p)
            if (macro.params[p] == t.spelling) return static_cast<long long>(p);
        return -1;
    };
    auto argTokens = [&](long long p) -> std::vector<Token> {
        if (p < static_cast<long long>(macro.params.size())) {
            return args[static_cast<std::size_t>(p)];
        }
        // __VA_ARGS__: remaining arguments interleaved with commas (every
        // argument position keeps its comma, empty or not).
        std::vector<Token> out;
        for (std::size_t a = macro.params.size(); a < args.size(); ++a) {
            if (a > macro.params.size()) {
                Token comma = args[a].empty() ? Token{} : args[a].front();
                comma.kind = TokenKind::Comma;
                comma.spelling = ",";
                out.push_back(comma);
            }
            out.insert(out.end(), args[a].begin(), args[a].end());
        }
        return out;
    };
    auto adjacentToPaste = [&](std::size_t i) {
        const auto& rep = macro.replacement;
        return (i > 0 && rep[i - 1].kind == TokenKind::HashHash) ||
               (i + 1 < rep.size() && rep[i + 1].kind == TokenKind::HashHash);
    };

    // Phase 1: substitute. '#' stringizes the unexpanded argument; parameters
    // adjacent to '##' stay unexpanded; all others are prescanned.
    std::vector<Token> result;
    const auto& rep = macro.replacement;
    for (std::size_t i = 0; i < rep.size(); ++i) {
        const Token& t = rep[i];
        if (t.kind == TokenKind::Hash && i + 1 < rep.size()) {
            const long long p = paramIndex(rep[i + 1]);
            if (p >= 0) {
                result.push_back(literal(TokenKind::StringLiteral,
                                         stringize(argTokens(p)), t));
                ++i;
                continue;
            }
        }
        const long long p = paramIndex(t);
        if (p >= 0) {
            std::vector<Token> arg = argTokens(p);
            if (!adjacentToPaste(i)) {
                std::vector<std::string> argHideset = hideset;
                arg = expandTokens(arg, argHideset);
            }
            result.insert(result.end(), arg.begin(), arg.end());
            continue;
        }
        result.push_back(t);
    }

    // Phase 2: fold '##' (an empty side acts as a placemarker and vanishes).
    std::vector<Token> folded;
    for (std::size_t i = 0; i < result.size(); ++i) {
        if (result[i].kind != TokenKind::HashHash) {
            folded.push_back(result[i]);
            continue;
        }
        const bool haveLhs = !folded.empty();
        const bool haveRhs = i + 1 < result.size();
        if (haveLhs && haveRhs) {
            const std::string spelling =
                folded.back().spelling + result[i + 1].spelling;
            std::vector<Token> reparsed = lex(spelling);
            std::size_t count = 0;
            for (const Token& t : reparsed)
                if (t.kind != TokenKind::End) ++count;
            if (count != 1 || reparsed.front().kind == TokenKind::Error) {
                diagnose(errLine, "token paste '" + spelling +
                                      "' is not a valid token");
                ++i; // drop the paste
                continue;
            }
            folded.back() = reparsed.front();
            ++i; // consumed the rhs
        }
        // else: placemarker side -- drop the '##'.
    }
    return folded;
}

} // namespace insbind
