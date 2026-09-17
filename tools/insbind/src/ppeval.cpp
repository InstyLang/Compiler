#include "ppeval.h"

#include <cctype>
#include <cstdint>

namespace insbind {
namespace {

Token literal(TokenKind kind, const std::string& spelling, const Token& at) {
    Token t = at;
    t.kind = kind;
    t.spelling = spelling;
    return t;
}

// A preprocessor arithmetic value: intmax_t unless suffixed/derived unsigned.
struct Val {
    std::uint64_t bits = 0;
    bool uns = false;
    std::int64_t s() const { return static_cast<std::int64_t>(bits); }
};

Val fromInt(std::int64_t v) {
    return {static_cast<std::uint64_t>(v), false};
}

// Decodes an integer literal spelling (base prefix, u/U/l/L suffixes).
bool decodeInt(const std::string& s, Val& out) {
    std::size_t i = 0;
    unsigned base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        i = 2;
    } else if (s.size() > 1 && s[0] == '0') {
        base = 8;
        i = 1;
    }
    std::uint64_t value = 0;
    bool any = false;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        unsigned d;
        if (c >= '0' && c <= '9') d = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<unsigned>(c - 'a') + 10;
        else if (c >= 'A' && c <= 'F') d = static_cast<unsigned>(c - 'A') + 10;
        else break; // suffix
        if (d >= base) return false;
        value = value * base + d;
        any = true;
    }
    if (!any && base != 8) return false;
    bool uns = false;
    for (; i < s.size(); ++i)
        if (s[i] == 'u' || s[i] == 'U') uns = true;
    out = {value, uns};
    return true;
}

// Decodes a character constant; multi-character constants pack like the big
// compilers: 'AB' == ('A'<<8)|'B'.
bool decodeChar(const std::string& s, std::int64_t& out) {
    std::size_t i = s.find('\'');
    if (i == std::string::npos) return false;
    ++i;
    std::int64_t value = 0;
    bool any = false;
    while (i < s.size() && s[i] != '\'') {
        std::int64_t c = s[i];
        if (s[i] == '\\' && i + 1 < s.size()) {
            ++i;
            switch (s[i]) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case '0': case '1': case '2': case '3':
                case '4': case '5': case '6': case '7': {
                    std::int64_t o = 0;
                    int n = 0;
                    while (n < 3 && i < s.size() && s[i] >= '0' && s[i] <= '7') {
                        o = o * 8 + (s[i] - '0');
                        ++i;
                        ++n;
                    }
                    c = o;
                    --i; // loop advances past
                    break;
                }
                case 'x': {
                    std::int64_t h = 0;
                    ++i;
                    while (i < s.size() && isxdigit(static_cast<unsigned char>(s[i]))) {
                        const char d = s[i];
                        h = h * 16 + (d <= '9' ? d - '0'
                                      : d <= 'F' ? d - 'A' + 10
                                                 : d - 'a' + 10);
                        ++i;
                    }
                    c = h;
                    --i;
                    break;
                }
                default: c = s[i]; break; // \\ \' \" \? \a \b \f \v approximated
            }
        }
        value = (value << 8) | (c & 0xFF);
        any = true;
        ++i;
    }
    if (!any) return false;
    out = value;
    return true;
}

class Parser {
public:
    Parser(const std::vector<Token>& tokens, IfEvalContext& ctx)
        : tokens_(tokens), ctx_(ctx) {}

    bool eval() {
        Val v = conditional();
        if (pos_ != tokens_.size())
            ctx_.errors.push_back(ctx_.location + ": trailing tokens in #if");
        return v.bits != 0;
    }

private:
    const std::vector<Token>& tokens_;
    IfEvalContext& ctx_;
    std::size_t pos_ = 0;

    bool at(TokenKind k) const {
        return pos_ < tokens_.size() && tokens_[pos_].kind == k;
    }
    bool eat(TokenKind k) {
        if (at(k)) { ++pos_; return true; }
        return false;
    }
    Val fail(const std::string& msg) {
        ctx_.errors.push_back(ctx_.location + ": " + msg);
        return fromInt(0);
    }

    Val conditional() {
        Val c = logicalOr();
        if (eat(TokenKind::Question)) {
            Val t = conditional();
            if (!eat(TokenKind::Colon))
                return fail("expected ':' in conditional expression");
            Val f = conditional();
            return c.bits ? t : f;
        }
        return c;
    }

    using BinFn = Val (*)(Val, Val, bool&);
    Val leftAssoc(Val (Parser::*next)(),
                  std::initializer_list<std::pair<TokenKind, BinFn>> ops) {
        Val lhs = (this->*next)();
        for (;;) {
            bool matched = false;
            for (const auto& [k, fn] : ops) {
                if (at(k)) {
                    ++pos_;
                    Val rhs = (this->*next)();
                    bool err = false;
                    lhs = fn(lhs, rhs, err);
                    if (err) return fail("division by zero in #if");
                    matched = true;
                    break;
                }
            }
            if (!matched) return lhs;
        }
    }

    static Val logic(Val a, Val b, bool r) { (void)a; (void)b; return fromInt(r); }

    Val logicalOr() {
        Val lhs = logicalAnd();
        while (at(TokenKind::PipePipe)) {
            ++pos_;
            Val rhs = logicalAnd();
            lhs = logic(lhs, rhs, (lhs.bits != 0) || (rhs.bits != 0));
        }
        return lhs;
    }
    Val logicalAnd() {
        Val lhs = bitOr();
        while (at(TokenKind::AmpAmp)) {
            ++pos_;
            Val rhs = bitOr();
            lhs = logic(lhs, rhs, (lhs.bits != 0) && (rhs.bits != 0));
        }
        return lhs;
    }
    Val bitOr() {
        return leftAssoc(&Parser::bitXor,
                         {{TokenKind::Pipe, [](Val a, Val b, bool&) {
                               return Val{a.bits | b.bits, a.uns || b.uns};
                           }}});
    }
    Val bitXor() {
        return leftAssoc(&Parser::bitAnd,
                         {{TokenKind::Caret, [](Val a, Val b, bool&) {
                               return Val{a.bits ^ b.bits, a.uns || b.uns};
                           }}});
    }
    Val bitAnd() {
        return leftAssoc(&Parser::equality,
                         {{TokenKind::Amp, [](Val a, Val b, bool&) {
                               return Val{a.bits & b.bits, a.uns || b.uns};
                           }}});
    }
    Val equality() {
        return leftAssoc(&Parser::relational,
                         {{TokenKind::EqEq,
                           [](Val a, Val b, bool&) {
                               bool r = (a.uns || b.uns) ? a.bits == b.bits
                                                         : a.s() == b.s();
                               return fromInt(r);
                           }},
                          {TokenKind::NotEq, [](Val a, Val b, bool&) {
                               bool r = (a.uns || b.uns) ? a.bits != b.bits
                                                         : a.s() != b.s();
                               return fromInt(r);
                           }}});
    }
    Val relational() {
        return leftAssoc(
            &Parser::shift,
            {{TokenKind::Lt,
              [](Val a, Val b, bool&) {
                  return fromInt((a.uns || b.uns) ? a.bits < b.bits
                                                  : a.s() < b.s());
              }},
             {TokenKind::Gt,
              [](Val a, Val b, bool&) {
                  return fromInt((a.uns || b.uns) ? a.bits > b.bits
                                                  : a.s() > b.s());
              }},
             {TokenKind::Le,
              [](Val a, Val b, bool&) {
                  return fromInt((a.uns || b.uns) ? a.bits <= b.bits
                                                  : a.s() <= b.s());
              }},
             {TokenKind::Ge, [](Val a, Val b, bool&) {
                  return fromInt((a.uns || b.uns) ? a.bits >= b.bits
                                                  : a.s() >= b.s());
              }}});
    }
    Val shift() {
        return leftAssoc(&Parser::additive,
                         {{TokenKind::Shl,
                           [](Val a, Val b, bool&) {
                               return Val{a.bits << (b.bits & 63), a.uns};
                           }},
                          {TokenKind::Shr, [](Val a, Val b, bool&) {
                               return Val{a.uns ? a.bits >> (b.bits & 63)
                                                : static_cast<std::uint64_t>(
                                                      a.s() >> (b.bits & 63)),
                                          a.uns};
                           }}});
    }
    Val additive() {
        return leftAssoc(&Parser::multiplicative,
                         {{TokenKind::Plus,
                           [](Val a, Val b, bool&) {
                               return Val{a.bits + b.bits, a.uns || b.uns};
                           }},
                          {TokenKind::Minus, [](Val a, Val b, bool&) {
                               return Val{a.bits - b.bits, a.uns || b.uns};
                           }}});
    }
    Val multiplicative() {
        return leftAssoc(
            &Parser::unary,
            {{TokenKind::Star,
              [](Val a, Val b, bool&) {
                  return Val{a.bits * b.bits, a.uns || b.uns};
              }},
             {TokenKind::Slash,
              [](Val a, Val b, bool& err) {
                  if (b.bits == 0) { err = true; return Val{}; }
                  return Val{(a.uns || b.uns)
                                 ? a.bits / b.bits
                                 : static_cast<std::uint64_t>(a.s() / b.s()),
                             a.uns || b.uns};
              }},
             {TokenKind::Percent, [](Val a, Val b, bool& err) {
                  if (b.bits == 0) { err = true; return Val{}; }
                  return Val{(a.uns || b.uns)
                                 ? a.bits % b.bits
                                 : static_cast<std::uint64_t>(a.s() % b.s()),
                             a.uns || b.uns};
              }}});
    }
    Val unary() {
        if (eat(TokenKind::Bang)) return fromInt(unary().bits == 0);
        if (eat(TokenKind::Tilde)) {
            Val v = unary();
            return Val{~v.bits, v.uns};
        }
        if (eat(TokenKind::Plus)) return unary();
        if (eat(TokenKind::Minus)) {
            Val v = unary();
            return Val{0 - v.bits, v.uns};
        }
        return primary();
    }
    Val primary() {
        if (eat(TokenKind::LParen)) {
            Val v = conditional();
            if (!eat(TokenKind::RParen))
                return fail("expected ')' in #if");
            return v;
        }
        if (pos_ >= tokens_.size()) return fail("unexpected end of #if");
        const Token& t = tokens_[pos_++];
        if (t.kind == TokenKind::IntLiteral) {
            Val v;
            if (!decodeInt(t.spelling, v))
                return fail("malformed integer literal '" + t.spelling + "'");
            return v;
        }
        if (t.kind == TokenKind::CharLiteral) {
            std::int64_t v;
            if (!decodeChar(t.spelling, v))
                return fail("malformed character constant '" + t.spelling + "'");
            return fromInt(v);
        }
        return fail("unexpected '" + t.spelling + "' in #if");
    }
};

// Stage 1: rewrite `defined X`, `defined(X)`, `__has_include(...)` to 1/0
// before anything else is expanded.
std::vector<Token> rewriteSpecials(const std::vector<Token>& in,
                                   IfEvalContext& ctx) {
    std::vector<Token> out;
    auto answer = [&](const Token& at, bool v) {
        return literal(TokenKind::IntLiteral, v ? "1" : "0", at);
    };
    for (std::size_t i = 0; i < in.size(); ++i) {
        const Token& t = in[i];
        if (t.kind != TokenKind::Identifier ||
            (t.spelling != "defined" && t.spelling != "__has_include")) {
            out.push_back(t);
            continue;
        }
        std::size_t j = i + 1;
        bool parens = j < in.size() && in[j].kind == TokenKind::LParen;
        if (parens) ++j;
        if (t.spelling == "defined") {
            if (j < in.size() && in[j].kind == TokenKind::Identifier) {
                out.push_back(answer(t, ctx.macros.isDefined(in[j].spelling)));
                i = j + (parens && j + 1 < in.size() &&
                                 in[j + 1].kind == TokenKind::RParen
                             ? 1
                             : 0);
                if (parens && (j + 1 >= in.size() ||
                               in[j + 1].kind != TokenKind::RParen))
                    ctx.errors.push_back(ctx.location +
                                         ": malformed defined() in #if");
            } else {
                ctx.errors.push_back(ctx.location +
                                     ": malformed defined in #if");
                out.push_back(answer(t, false));
            }
        } else {
            // __has_include: gather tokens to the matching ')' and interpret
            // them as "header" or <header>.
            std::string name;
            bool angled = false;
            bool ok = false;
            if (parens) {
                std::size_t k = j;
                int depth = 1;
                std::string inner;
                for (; k < in.size() && depth > 0; ++k) {
                    if (in[k].kind == TokenKind::LParen) ++depth;
                    else if (in[k].kind == TokenKind::RParen) {
                        if (--depth == 0) break;
                    }
                    if (depth > 0) inner += in[k].spelling;
                }
                if (depth == 0) {
                    if (inner.size() >= 2 && inner.front() == '"' &&
                        inner.back() == '"') {
                        name = inner.substr(1, inner.size() - 2);
                        ok = true;
                    } else if (inner.size() >= 2 && inner.front() == '<' &&
                               inner.back() == '>') {
                        name = inner.substr(1, inner.size() - 2);
                        angled = true;
                        ok = true;
                    }
                    j = k;
                }
            }
            if (ok && ctx.includeExists) {
                out.push_back(answer(t, ctx.includeExists(name, angled)));
                i = j;
            } else {
                ctx.errors.push_back(ctx.location +
                                     ": malformed __has_include in #if");
                out.push_back(answer(t, false));
                if (parens) i = j;
            }
        }
    }
    return out;
}

} // namespace

bool evalIf(const std::vector<Token>& tokens, IfEvalContext& ctx) {
    if (tokens.empty()) {
        ctx.errors.push_back(ctx.location + ": empty #if expression");
        return false;
    }
    // Stage 1: specials. Stage 2: macro expansion. Stage 3: identifiers -> 0.
    std::vector<Token> expanded = ctx.expander.expandLine(rewriteSpecials(tokens, ctx));
    for (Token& t : expanded) {
        if (t.kind == TokenKind::Identifier) {
            t.kind = TokenKind::IntLiteral;
            t.spelling = "0";
        }
    }
    Parser parser(expanded, ctx);
    return parser.eval();
}

} // namespace insbind
