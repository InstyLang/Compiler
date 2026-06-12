
#include <parser/parser.hpp>
#include <lexer/lexer.hpp>

namespace {
size_t scanType(Parser& parser, size_t i, bool& ok) {
    ok = false;
    size_t k = i;

    if (parser.peek(k).type == TokenType::KwVolatile) {
        ++k;
    }

    TokenType t = parser.peek(k).type;
    bool isName = (t == TokenType::Identifier) ||
                  isPrimitiveTypeName(parser.peek(k).value);
    if (!isName) {
        return i;
    }
    ++k;
    ok = true;

    while (parser.peek(k).type == TokenType::Dot &&
           parser.peek(k + 1).type == TokenType::Identifier) {
        k += 2;
    }

    if (parser.peek(k).type == TokenType::Lt) {
        int depth = 0;
        do {
            TokenType tt = parser.peek(k).type;
            if (tt == TokenType::Lt) ++depth;
            else if (tt == TokenType::Gt) --depth;
            else if (tt == TokenType::EndOfFile) break;
            ++k;
        } while (depth > 0);
    }

    while (parser.peek(k).type == TokenType::Star) {
        ++k;
    }

    if (parser.peek(k).type == TokenType::LBracket) {
        if (parser.peek(k + 1).type == TokenType::RBracket) {
            k += 2;
        } else if (parser.peek(k + 1).type == TokenType::IntegerLiteral &&
                   parser.peek(k + 2).type == TokenType::RBracket) {
            k += 3;
        }
    }

    return k;
}
}

bool ecxLooksLikeVariableDecl(Parser& parser) {
    if (parser.check(TokenType::KwConst) || parser.check(TokenType::KwLet)) {
        return true;
    }

    TokenType t = parser.current().type;
    bool startsType = (t == TokenType::Identifier) ||
                      (t == TokenType::KwVolatile) ||
                      isPrimitiveTypeName(parser.current().value);
    if (!startsType) {
        return false;
    }

    bool ok = false;
    size_t after = scanType(parser, 0, ok);
    if (!ok) {
        return false;
    }
    return parser.peek(after).type == TokenType::Identifier;
}
