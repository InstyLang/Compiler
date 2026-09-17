#include "lexer.h"

#include <array>
#include <string_view>

namespace insbind {
namespace {

struct KeywordEntry {
    const char* spelling;
    TokenKind kind;
};

// Sorted by spelling (ASCII: '_' precedes lowercase) for binary search.
constexpr std::array kKeywords = std::to_array<KeywordEntry>({
    {"_Alignas", TokenKind::kw__Alignas},
    {"_Alignof", TokenKind::kw__Alignof},
    {"_Atomic", TokenKind::kw__Atomic},
    {"_Bool", TokenKind::kw__Bool},
    {"_Complex", TokenKind::kw__Complex},
    {"_Generic", TokenKind::kw__Generic},
    {"_Imaginary", TokenKind::kw__Imaginary},
    {"_Noreturn", TokenKind::kw__Noreturn},
    {"_Static_assert", TokenKind::kw__Static_assert},
    {"_Thread_local", TokenKind::kw__Thread_local},
    {"auto", TokenKind::kw_auto},
    {"break", TokenKind::kw_break},
    {"case", TokenKind::kw_case},
    {"char", TokenKind::kw_char},
    {"const", TokenKind::kw_const},
    {"continue", TokenKind::kw_continue},
    {"default", TokenKind::kw_default},
    {"do", TokenKind::kw_do},
    {"double", TokenKind::kw_double},
    {"else", TokenKind::kw_else},
    {"enum", TokenKind::kw_enum},
    {"extern", TokenKind::kw_extern},
    {"float", TokenKind::kw_float},
    {"for", TokenKind::kw_for},
    {"goto", TokenKind::kw_goto},
    {"if", TokenKind::kw_if},
    {"inline", TokenKind::kw_inline},
    {"int", TokenKind::kw_int},
    {"long", TokenKind::kw_long},
    {"register", TokenKind::kw_register},
    {"restrict", TokenKind::kw_restrict},
    {"return", TokenKind::kw_return},
    {"short", TokenKind::kw_short},
    {"signed", TokenKind::kw_signed},
    {"sizeof", TokenKind::kw_sizeof},
    {"static", TokenKind::kw_static},
    {"struct", TokenKind::kw_struct},
    {"switch", TokenKind::kw_switch},
    {"typedef", TokenKind::kw_typedef},
    {"union", TokenKind::kw_union},
    {"unsigned", TokenKind::kw_unsigned},
    {"void", TokenKind::kw_void},
    {"volatile", TokenKind::kw_volatile},
    {"while", TokenKind::kw_while},
});

TokenKind keywordKind(std::string_view text) {
    std::size_t lo = 0, hi = kKeywords.size();
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        const int cmp = text.compare(kKeywords[mid].spelling);
        if (cmp == 0) return kKeywords[mid].kind;
        if (cmp < 0) hi = mid;
        else lo = mid + 1;
    }
    return TokenKind::Identifier;
}

struct PunctEntry {
    const char* spelling;
    TokenKind kind;
};

// Longest spellings first so maximal munch is a plain linear scan.
constexpr std::array kPuncts = std::to_array<PunctEntry>({
    {"%:%:", TokenKind::HashHash},
    {"<<=", TokenKind::ShlAssign}, {">>=", TokenKind::ShrAssign},
    {"...", TokenKind::Ellipsis},
    {"->", TokenKind::Arrow}, {"++", TokenKind::PlusPlus},
    {"--", TokenKind::MinusMinus}, {"<<", TokenKind::Shl},
    {">>", TokenKind::Shr}, {"<=", TokenKind::Le}, {">=", TokenKind::Ge},
    {"==", TokenKind::EqEq}, {"!=", TokenKind::NotEq},
    {"&&", TokenKind::AmpAmp}, {"||", TokenKind::PipePipe},
    {"*=", TokenKind::StarAssign}, {"/=", TokenKind::SlashAssign},
    {"%=", TokenKind::PercentAssign}, {"+=", TokenKind::PlusAssign},
    {"-=", TokenKind::MinusAssign}, {"&=", TokenKind::AmpAssign},
    {"^=", TokenKind::CaretAssign}, {"|=", TokenKind::PipeAssign},
    {"##", TokenKind::HashHash},
    {"<:", TokenKind::LBracket}, {":>", TokenKind::RBracket},
    {"<%", TokenKind::LBrace}, {"%>", TokenKind::RBrace},
    {"%:", TokenKind::Hash},
    {"[", TokenKind::LBracket}, {"]", TokenKind::RBracket},
    {"(", TokenKind::LParen}, {")", TokenKind::RParen},
    {"{", TokenKind::LBrace}, {"}", TokenKind::RBrace},
    {".", TokenKind::Dot}, {"&", TokenKind::Amp}, {"*", TokenKind::Star},
    {"+", TokenKind::Plus}, {"-", TokenKind::Minus}, {"~", TokenKind::Tilde},
    {"!", TokenKind::Bang}, {"/", TokenKind::Slash}, {"%", TokenKind::Percent},
    {"<", TokenKind::Lt}, {">", TokenKind::Gt}, {"^", TokenKind::Caret},
    {"|", TokenKind::Pipe}, {"?", TokenKind::Question},
    {":", TokenKind::Colon}, {";", TokenKind::Semicolon},
    {"=", TokenKind::Assign}, {",", TokenKind::Comma},
    {"#", TokenKind::Hash},
});

bool isIdentStart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool isIdentCont(char c) {
    return isIdentStart(c) || (c >= '0' && c <= '9');
}
bool isDigit(char c) { return c >= '0' && c <= '9'; }

class Lexer {
public:
    explicit Lexer(const std::string& src) : src_(src) {}

    std::vector<Token> run() {
        std::vector<Token> out;
        for (;;) {
            Token t = next();
            const bool done = (t.kind == TokenKind::End);
            out.push_back(std::move(t));
            if (done) break;
        }
        return out;
    }

private:
    const std::string& src_;
    std::size_t pos_ = 0;
    std::uint32_t line_ = 1, col_ = 1;

    bool eof() const { return pos_ >= src_.size(); }
    char at(std::size_t off) const {
        return pos_ + off < src_.size() ? src_[pos_ + off] : '\0';
    }
    char cur() const { return at(0); }

    void advance() {
        if (eof()) return;
        if (src_[pos_] == '\n') { ++line_; col_ = 1; }
        else { ++col_; }
        ++pos_;
    }
    void advance(std::size_t n) { while (n--) advance(); }

    Token make(TokenKind kind, std::size_t start, std::uint32_t line,
               std::uint32_t col, bool lineStart, bool space) const {
        Token t;
        t.kind = kind;
        t.spelling = src_.substr(start, pos_ - start);
        t.line = line;
        t.col = col;
        t.atLineStart = lineStart;
        t.leadingSpace = space;
        return t;
    }

    Token error(std::string msg, std::uint32_t line, std::uint32_t col,
                bool lineStart, bool space) {
        Token t;
        t.kind = TokenKind::Error;
        t.spelling = std::move(msg);
        t.line = line;
        t.col = col;
        t.atLineStart = lineStart;
        t.leadingSpace = space;
        return t;
    }

    // Skips whitespace and comments. Returns an error message if a block
    // comment is unterminated (the caller then emits an Error token).
    std::string skipTrivia(bool& lineStart, bool& space) {
        lineStart = false;
        space = false;
        for (;;) {
            const char c = cur();
            if (c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f') {
                space = true;
                advance();
            } else if (c == '\n') {
                space = true;
                lineStart = true;
                advance();
            } else if (c == '/' && at(1) == '/') {
                space = true;
                while (!eof() && cur() != '\n') advance();
            } else if (c == '/' && at(1) == '*') {
                space = true;
                advance(2);
                bool closed = false;
                while (!eof()) {
                    if (cur() == '*' && at(1) == '/') {
                        advance(2);
                        closed = true;
                        break;
                    }
                    if (cur() == '\n') lineStart = true;
                    advance();
                }
                if (!closed) return "unterminated block comment";
            } else {
                return {};
            }
        }
    }

    Token next() {
        bool lineStart, space;
        const std::string triviaError = skipTrivia(lineStart, space);
        const std::uint32_t line = line_, col = col_;
        if (eof()) {
            if (!triviaError.empty())
                return error(triviaError, line, col, lineStart, space);
            return make(TokenKind::End, pos_, line, col, lineStart, space);
        }
        if (!triviaError.empty())
            return error(triviaError, line, col, lineStart, space);

        const std::size_t start = pos_;
        const char c = cur();

        // Prefixed literals: L/u/U + ' or ", and u8 + ' or ". Check before
        // identifier lexing so `u8"x"` is one literal, not `u8` + `"x"`.
        if ((c == 'L' || c == 'u' || c == 'U') && (at(1) == '"' || at(1) == '\''))
            return lexCharOrString(start, line, col, lineStart, space, 1);
        if (c == 'u' && at(1) == '8' && (at(2) == '"' || at(2) == '\''))
            return lexCharOrString(start, line, col, lineStart, space, 2);

        if (isIdentStart(c)) {
            while (isIdentCont(cur())) advance();
            const std::string_view text(src_.data() + start, pos_ - start);
            return make(keywordKind(text), start, line, col, lineStart, space);
        }

        if (isDigit(c) || (c == '.' && isDigit(at(1))))
            return lexNumber(start, line, col, lineStart, space);

        if (c == '"' || c == '\'')
            return lexCharOrString(start, line, col, lineStart, space, 0);

        for (const PunctEntry& p : kPuncts) {
            const std::size_t len = std::string_view(p.spelling).size();
            if (src_.compare(pos_, len, p.spelling) == 0) {
                advance(len);
                return make(p.kind, start, line, col, lineStart, space);
            }
        }

        advance();
        return error(std::string("unexpected byte '") + c + "'", line, col,
                     lineStart, space);
    }

    // pp-number (6.4.8): digits/letters/periods, with e/E/p/P exponents that
    // may consume a following sign. Classified int vs float by content; the
    // value itself is decoded later by the constant-expression evaluator.
    Token lexNumber(std::size_t start, std::uint32_t line, std::uint32_t col,
                    bool lineStart, bool space) {
        advance(); // first digit, or '.' of a leading fraction
        char prev = src_[start];
        for (;;) {
            const char c = cur();
            if (isIdentCont(c) || c == '.') {
                prev = c;
                advance();
            } else if ((c == '+' || c == '-') &&
                       (prev == 'e' || prev == 'E' || prev == 'p' || prev == 'P')) {
                prev = c;
                advance();
            } else {
                break;
            }
        }
        const std::string_view text(src_.data() + start, pos_ - start);
        bool isFloat = text.find('.') != std::string_view::npos;
        if (!isFloat) {
            const bool hex = text.size() > 2 && text[0] == '0' &&
                             (text[1] == 'x' || text[1] == 'X');
            if (hex) {
                isFloat = text.find_first_of("pP") != std::string_view::npos;
            } else {
                isFloat = text.find_first_of("eEfF") != std::string_view::npos;
            }
        }
        return make(isFloat ? TokenKind::FloatLiteral : TokenKind::IntLiteral,
                    start, line, col, lineStart, space);
    }

    Token lexCharOrString(std::size_t start, std::uint32_t line,
                          std::uint32_t col, bool lineStart, bool space,
                          std::size_t prefixLen) {
        advance(prefixLen);
        const char quote = cur();
        advance(); // opening quote
        for (;;) {
            if (eof() || cur() == '\n') {
                return error(quote == '"' ? "unterminated string literal"
                                          : "unterminated character constant",
                             line, col, lineStart, space);
            }
            if (cur() == '\\') {
                advance();
                if (!eof()) advance();
                continue;
            }
            if (cur() == quote) {
                advance();
                return make(quote == '"' ? TokenKind::StringLiteral
                                         : TokenKind::CharLiteral,
                            start, line, col, lineStart, space);
            }
            advance();
        }
    }
};

} // namespace

std::vector<Token> lex(const std::string& source) {
    return Lexer(source).run();
}

const char* tokenKindName(TokenKind kind) {
    switch (kind) {
        case TokenKind::End: return "End";
        case TokenKind::Error: return "Error";
        case TokenKind::Identifier: return "Identifier";
        case TokenKind::IntLiteral: return "IntLiteral";
        case TokenKind::FloatLiteral: return "FloatLiteral";
        case TokenKind::CharLiteral: return "CharLiteral";
        case TokenKind::StringLiteral: return "StringLiteral";
        case TokenKind::kw_auto: return "kw_auto";
        case TokenKind::kw_break: return "kw_break";
        case TokenKind::kw_case: return "kw_case";
        case TokenKind::kw_char: return "kw_char";
        case TokenKind::kw_const: return "kw_const";
        case TokenKind::kw_continue: return "kw_continue";
        case TokenKind::kw_default: return "kw_default";
        case TokenKind::kw_do: return "kw_do";
        case TokenKind::kw_double: return "kw_double";
        case TokenKind::kw_else: return "kw_else";
        case TokenKind::kw_enum: return "kw_enum";
        case TokenKind::kw_extern: return "kw_extern";
        case TokenKind::kw_float: return "kw_float";
        case TokenKind::kw_for: return "kw_for";
        case TokenKind::kw_goto: return "kw_goto";
        case TokenKind::kw_if: return "kw_if";
        case TokenKind::kw_inline: return "kw_inline";
        case TokenKind::kw_int: return "kw_int";
        case TokenKind::kw_long: return "kw_long";
        case TokenKind::kw_register: return "kw_register";
        case TokenKind::kw_restrict: return "kw_restrict";
        case TokenKind::kw_return: return "kw_return";
        case TokenKind::kw_short: return "kw_short";
        case TokenKind::kw_signed: return "kw_signed";
        case TokenKind::kw_sizeof: return "kw_sizeof";
        case TokenKind::kw_static: return "kw_static";
        case TokenKind::kw_struct: return "kw_struct";
        case TokenKind::kw_switch: return "kw_switch";
        case TokenKind::kw_typedef: return "kw_typedef";
        case TokenKind::kw_union: return "kw_union";
        case TokenKind::kw_unsigned: return "kw_unsigned";
        case TokenKind::kw_void: return "kw_void";
        case TokenKind::kw_volatile: return "kw_volatile";
        case TokenKind::kw_while: return "kw_while";
        case TokenKind::kw__Alignas: return "kw__Alignas";
        case TokenKind::kw__Alignof: return "kw__Alignof";
        case TokenKind::kw__Atomic: return "kw__Atomic";
        case TokenKind::kw__Bool: return "kw__Bool";
        case TokenKind::kw__Complex: return "kw__Complex";
        case TokenKind::kw__Generic: return "kw__Generic";
        case TokenKind::kw__Imaginary: return "kw__Imaginary";
        case TokenKind::kw__Noreturn: return "kw__Noreturn";
        case TokenKind::kw__Static_assert: return "kw__Static_assert";
        case TokenKind::kw__Thread_local: return "kw__Thread_local";
        case TokenKind::LBracket: return "LBracket";
        case TokenKind::RBracket: return "RBracket";
        case TokenKind::LParen: return "LParen";
        case TokenKind::RParen: return "RParen";
        case TokenKind::LBrace: return "LBrace";
        case TokenKind::RBrace: return "RBrace";
        case TokenKind::Dot: return "Dot";
        case TokenKind::Arrow: return "Arrow";
        case TokenKind::PlusPlus: return "PlusPlus";
        case TokenKind::MinusMinus: return "MinusMinus";
        case TokenKind::Amp: return "Amp";
        case TokenKind::Star: return "Star";
        case TokenKind::Plus: return "Plus";
        case TokenKind::Minus: return "Minus";
        case TokenKind::Tilde: return "Tilde";
        case TokenKind::Bang: return "Bang";
        case TokenKind::Slash: return "Slash";
        case TokenKind::Percent: return "Percent";
        case TokenKind::Shl: return "Shl";
        case TokenKind::Shr: return "Shr";
        case TokenKind::Lt: return "Lt";
        case TokenKind::Gt: return "Gt";
        case TokenKind::Le: return "Le";
        case TokenKind::Ge: return "Ge";
        case TokenKind::EqEq: return "EqEq";
        case TokenKind::NotEq: return "NotEq";
        case TokenKind::Caret: return "Caret";
        case TokenKind::Pipe: return "Pipe";
        case TokenKind::AmpAmp: return "AmpAmp";
        case TokenKind::PipePipe: return "PipePipe";
        case TokenKind::Question: return "Question";
        case TokenKind::Colon: return "Colon";
        case TokenKind::Semicolon: return "Semicolon";
        case TokenKind::Ellipsis: return "Ellipsis";
        case TokenKind::Assign: return "Assign";
        case TokenKind::StarAssign: return "StarAssign";
        case TokenKind::SlashAssign: return "SlashAssign";
        case TokenKind::PercentAssign: return "PercentAssign";
        case TokenKind::PlusAssign: return "PlusAssign";
        case TokenKind::MinusAssign: return "MinusAssign";
        case TokenKind::ShlAssign: return "ShlAssign";
        case TokenKind::ShrAssign: return "ShrAssign";
        case TokenKind::AmpAssign: return "AmpAssign";
        case TokenKind::CaretAssign: return "CaretAssign";
        case TokenKind::PipeAssign: return "PipeAssign";
        case TokenKind::Comma: return "Comma";
        case TokenKind::Hash: return "Hash";
        case TokenKind::HashHash: return "HashHash";
    }
    return "?";
}

std::string dumpTokens(const std::vector<Token>& tokens) {
    std::string out;
    for (const Token& t : tokens) {
        if (t.kind == TokenKind::End) continue;
        out += std::to_string(t.line);
        out += ':';
        out += std::to_string(t.col);
        out += ' ';
        out += tokenKindName(t.kind);
        out += ' ';
        out += t.spelling;
        out += '\n';
    }
    return out;
}

} // namespace insbind
