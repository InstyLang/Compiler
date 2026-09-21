
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
            else if (tt == TokenType::Shr) depth -= 2;
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

AST::NodePtr Parser::parseTypeAliasDeclaration() {
    const Token& start = current();
    expect(TokenType::KwType, "E1110", "'type' keyword");
    match(TokenType::KwType);

    auto node = std::make_shared<AST::TypeAliasDeclaration>();
    if (check(TokenType::Identifier)) {
        node->name = current().value;
        advance();
    } else {
        error("E1111", "expected type alias name", "e.g. type MyInt = i64");
        return nullptr;
    }

    expect(TokenType::Assign, "E1112", "'=' after type alias name");
    match(TokenType::Assign);

    node->targetType = parseTypeName();
    fillRange(*node, start, previous());
    return node;
}

AST::NodePtr Parser::parseDestructureStatement() {
    const Token& start = current();
    expect(TokenType::LParen, "E1113", "'(' to open destructuring bindings");
    match(TokenType::LParen);

    auto node = std::make_shared<AST::DestructureStatement>();
    bool first = true;
    while (!atEnd() && !check(TokenType::RParen)) {
        if (!first) {
            expect(TokenType::Comma, "E1114", "',' between destructuring bindings");
            match(TokenType::Comma);
        }
        first = false;
        skipNewlines();
        AST::DestructureBinding b;
        if (check(TokenType::Identifier)) {
            std::string t1 = current().value;
            advance();
            if (check(TokenType::Identifier)) {
                b.typeHint = t1;
                b.name = current().value;
                advance();
            } else {
                b.name = t1;
            }
        } else {
            b.typeHint = parseTypeName();
            if (check(TokenType::Identifier)) {
                b.name = current().value;
                advance();
            }
        }
        node->bindings.push_back(std::move(b));
    }
    expect(TokenType::RParen, "E1115", "')' to close destructuring bindings");
    match(TokenType::RParen);

    expect(TokenType::Assign, "E1116", "'=' after destructuring bindings");
    match(TokenType::Assign);

    node->value = parseExpression();
    fillRange(*node, start, previous());
    return node;
}
