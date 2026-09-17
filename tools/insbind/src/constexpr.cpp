#include "constexpr.h"

namespace insbind {
namespace {

// Integer literal decode shared in spirit with ppeval's; duplicated here
// signed-only and suffix-tolerant, which is all declaration contexts need.
bool decodeInt(const std::string& s, std::int64_t& out) {
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
        else break; // u/U/l/L suffix
        if (d >= base) return false;
        value = value * base + d;
        any = true;
    }
    if (!any && base != 8) return false;
    out = static_cast<std::int64_t>(value);
    return true;
}

bool decodeChar(const std::string& s, std::int64_t& out) {
    std::size_t i = s.find('\'');
    if (i == std::string::npos) return false;
    ++i;
    std::int64_t value = 0;
    bool any = false;
    while (i < s.size() && s[i] != '\'') {
        std::int64_t c = static_cast<unsigned char>(s[i]);
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
                    --i;
                    break;
                }
                case 'x': {
                    std::int64_t h = 0;
                    ++i;
                    while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') ||
                                            (s[i] >= 'a' && s[i] <= 'f') ||
                                            (s[i] >= 'A' && s[i] <= 'F'))) {
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
                default: c = s[i]; break;
            }
        }
        value = (value << 8) | (c & 0xFF); // multi-char packs like the big compilers
        any = true;
        ++i;
    }
    if (!any) return false;
    out = value;
    return true;
}

class Cursor {
public:
    Cursor(const std::vector<Token>& tokens, std::size_t& pos, std::string& err,
           const std::unordered_map<std::string, std::int64_t>* names)
        : tokens_(tokens), pos_(pos), err_(err), names_(names) {}

    bool eval(std::int64_t& out) {
        out = conditional();
        return err_.empty();
    }

private:
    const std::vector<Token>& tokens_;
    std::size_t& pos_;
    std::string& err_;
    const std::unordered_map<std::string, std::int64_t>* names_;

    bool at(TokenKind k) const {
        return pos_ < tokens_.size() && tokens_[pos_].kind == k;
    }
    bool eat(TokenKind k) {
        if (at(k)) { ++pos_; return true; }
        return false;
    }
    std::int64_t fail(const std::string& msg) {
        if (err_.empty()) err_ = msg; // keep the first (deepest) error
        return 0;
    }

    std::int64_t conditional() {
        std::int64_t c = logicalOr();
        if (at(TokenKind::Question)) {
            ++pos_;
            const std::int64_t t = conditional();
            if (!eat(TokenKind::Colon)) return fail("expected ':'");
            const std::int64_t f = conditional();
            return c ? t : f;
        }
        return c;
    }
    std::int64_t logicalOr() {
        std::int64_t v = logicalAnd();
        while (eat(TokenKind::PipePipe)) v = (v != 0) || (logicalAnd() != 0);
        return v;
    }
    std::int64_t logicalAnd() {
        std::int64_t v = bitOr();
        while (eat(TokenKind::AmpAmp)) v = (v != 0) && (bitOr() != 0);
        return v;
    }
    std::int64_t bitOr() {
        std::int64_t v = bitXor();
        while (eat(TokenKind::Pipe)) v |= bitXor();
        return v;
    }
    std::int64_t bitXor() {
        std::int64_t v = bitAnd();
        while (eat(TokenKind::Caret)) v ^= bitAnd();
        return v;
    }
    std::int64_t bitAnd() {
        std::int64_t v = equality();
        while (eat(TokenKind::Amp)) v &= equality();
        return v;
    }
    std::int64_t equality() {
        std::int64_t v = relational();
        for (;;) {
            if (eat(TokenKind::EqEq)) v = v == relational();
            else if (eat(TokenKind::NotEq)) v = v != relational();
            else return v;
        }
    }
    std::int64_t relational() {
        std::int64_t v = shift();
        for (;;) {
            if (eat(TokenKind::Lt)) v = v < shift();
            else if (eat(TokenKind::Gt)) v = v > shift();
            else if (eat(TokenKind::Le)) v = v <= shift();
            else if (eat(TokenKind::Ge)) v = v >= shift();
            else return v;
        }
    }
    std::int64_t shift() {
        std::int64_t v = additive();
        for (;;) {
            if (eat(TokenKind::Shl)) v = v << (additive() & 63);
            else if (eat(TokenKind::Shr)) v = v >> (additive() & 63);
            else return v;
        }
    }
    std::int64_t additive() {
        std::int64_t v = multiplicative();
        for (;;) {
            if (eat(TokenKind::Plus)) v += multiplicative();
            else if (eat(TokenKind::Minus)) v -= multiplicative();
            else return v;
        }
    }
    std::int64_t multiplicative() {
        std::int64_t v = unary();
        for (;;) {
            if (eat(TokenKind::Star)) v *= unary();
            else if (eat(TokenKind::Slash)) {
                const std::int64_t d = unary();
                if (d == 0) return fail("division by zero");
                v /= d;
            } else if (eat(TokenKind::Percent)) {
                const std::int64_t d = unary();
                if (d == 0) return fail("division by zero");
                v %= d;
            } else {
                return v;
            }
        }
    }
    std::int64_t unary() {
        if (eat(TokenKind::Bang)) return unary() == 0;
        if (eat(TokenKind::Tilde)) return ~unary();
        if (eat(TokenKind::Plus)) return unary();
        if (eat(TokenKind::Minus)) return -unary();
        return primary();
    }
    std::int64_t primary() {
        if (eat(TokenKind::LParen)) {
            const std::int64_t v = conditional();
            if (!eat(TokenKind::RParen)) return fail("expected ')'");
            return v;
        }
        if (pos_ >= tokens_.size()) return fail("unexpected end of expression");
        const Token& t = tokens_[pos_++];
        if (t.kind == TokenKind::IntLiteral) {
            std::int64_t v;
            if (!decodeInt(t.spelling, v))
                return fail("malformed integer '" + t.spelling + "'");
            return v;
        }
        if (t.kind == TokenKind::CharLiteral) {
            std::int64_t v;
            if (!decodeChar(t.spelling, v))
                return fail("malformed character constant '" + t.spelling + "'");
            return v;
        }
        if (t.kind == TokenKind::Identifier && names_) {
            const auto it = names_->find(t.spelling);
            if (it != names_->end()) return it->second;
        }
        return fail("not a constant: '" + t.spelling + "'");
    }
};

} // namespace

bool evalConstExpr(
    const std::vector<Token>& tokens, std::size_t* pos, std::int64_t* out,
    std::string* err,
    const std::unordered_map<std::string, std::int64_t>* names) {
    Cursor cursor(tokens, *pos, *err, names);
    return cursor.eval(*out);
}

} // namespace insbind
