
#include <parser/parser.hpp>
#include <lexer/lexer.hpp>

#include <cstdlib>
#include <memory>
#include <string>

namespace ecxlit {

static std::string stripUnderscores(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        if (c != '_') out.push_back(c);
    }
    return out;
}

long long parseIntegerValue(const std::string& raw) {
    std::string clean = stripUnderscores(raw);
    if (clean.size() > 2 && clean[0] == '0' &&
        (clean[1] == 'x' || clean[1] == 'X')) {
        return static_cast<long long>(
            std::strtoull(clean.c_str() + 2, nullptr, 16));
    }
    return static_cast<long long>(std::strtoull(clean.c_str(), nullptr, 10));
}

// Parses an integer literal into a full 128-bit value, so i128/u128 constants
// that exceed 64 bits are represented exactly. Accepts an optional 0x/0X hex
// prefix; digits are accumulated into an unsigned __int128 (overflow past 128
// bits simply wraps, matching two's-complement literal semantics).
__int128 parseInteger128(const std::string& raw) {
    std::string clean = stripUnderscores(raw);
    unsigned __int128 acc = 0;
    const char* p = clean.c_str();
    int base = 10;
    if (clean.size() > 2 && clean[0] == '0' &&
        (clean[1] == 'x' || clean[1] == 'X')) {
        base = 16;
        p += 2;
    }
    for (; *p; ++p) {
        char c = *p;
        unsigned d;
        if (c >= '0' && c <= '9') d = static_cast<unsigned>(c - '0');
        else if (base == 16 && c >= 'a' && c <= 'f') d = static_cast<unsigned>(c - 'a' + 10);
        else if (base == 16 && c >= 'A' && c <= 'F') d = static_cast<unsigned>(c - 'A' + 10);
        else break;
        acc = acc * static_cast<unsigned __int128>(base) + d;
    }
    return static_cast<__int128>(acc);
}

double parseFloatValue(const std::string& raw) {
    std::string clean = stripUnderscores(raw);
    return std::strtod(clean.c_str(), nullptr);
}

std::shared_ptr<AST::IntegerLiteral> makeInteger(const std::string& raw) {
    auto node = std::make_shared<AST::IntegerLiteral>();
    node->raw = raw;
    node->value = parseInteger128(raw);
    return node;
}

std::shared_ptr<AST::FloatLiteral> makeFloat(const std::string& raw) {
    auto node = std::make_shared<AST::FloatLiteral>();
    node->raw = raw;
    node->value = parseFloatValue(raw);
    return node;
}

std::shared_ptr<AST::BoolLiteral> makeBool(bool value) {
    auto node = std::make_shared<AST::BoolLiteral>();
    node->value = value;
    return node;
}

static AST::NodePtr parseFragment(const std::string& fragment) {
    if (fragment.empty()) {
        return nullptr;
    }
    Lexer lexer;
    std::string src = fragment;
    std::vector<Token> toks = lexer.tokenize(src);
    Parser sub;
    auto root = sub.produceASTFromTokens(std::move(toks));
    if (root && !root->body.empty()) {
        return root->body.front();
    }
    return nullptr;
}

std::shared_ptr<AST::StringLiteral> makeString(const std::string& decoded) {
    auto node = std::make_shared<AST::StringLiteral>();

    auto startsInterp = [](const std::string& s, size_t at) -> bool {
        if (at + 1 >= s.size()) return false;
        char nx = s[at + 1];
        return nx == '{' || nx == '_' ||
               (nx >= 'A' && nx <= 'Z') || (nx >= 'a' && nx <= 'z');
    };

    bool anyInterp = false;
    for (size_t k = 0; k + 1 < decoded.size(); ++k) {
        if (decoded[k] == '$' && startsInterp(decoded, k)) {
            anyInterp = true;
            break;
        }
    }
    if (!anyInterp) {
        node->value = decoded;
        return node;
    }

    node->hasInterpolation = true;
    std::string literal;
    size_t i = 0;
    const size_t n = decoded.size();

    while (i < n) {
        char c = decoded[i];
        if (c == '$' && startsInterp(decoded, i)) {
            node->literalParts.push_back(literal);
            literal.clear();

            ++i;
            std::string fragment;
            if (decoded[i] == '{') {
                ++i;
                int depth = 1;
                while (i < n && depth > 0) {
                    if (decoded[i] == '{') ++depth;
                    else if (decoded[i] == '}') {
                        --depth;
                        if (depth == 0) break;
                    }
                    fragment.push_back(decoded[i]);
                    ++i;
                }
                if (i < n && decoded[i] == '}') ++i;
            } else {
                while (i < n) {
                    char d = decoded[i];
                    bool identChar = (d >= 'A' && d <= 'Z') ||
                                     (d >= 'a' && d <= 'z') ||
                                     (d >= '0' && d <= '9') || d == '_' || d == '.';
                    if (identChar) {
                        fragment.push_back(d);
                        ++i;
                    } else if (d == '(' || d == '[') {
                        char open = d;
                        char close = (d == '(') ? ')' : ']';
                        int depth = 0;
                        do {
                            char e = decoded[i];
                            if (e == open) ++depth;
                            else if (e == close) --depth;
                            fragment.push_back(e);
                            ++i;
                        } while (i < n && depth > 0);
                    } else {
                        break;
                    }
                }
            }
            node->exprParts.push_back(parseFragment(fragment));
            continue;
        }
        literal.push_back(c);
        ++i;
    }
    node->literalParts.push_back(literal);

    return node;
}

}
