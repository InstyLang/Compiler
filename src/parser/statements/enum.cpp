
#include <parser/parser.hpp>

#include <memory>
#include <string>

namespace ecxlit {
long long parseIntegerValue(const std::string& raw);
}

AST::NodePtr Parser::parseEnumDeclaration() {
    const Token& start = current();
    expect(TokenType::KwEnum, "E1440", "'enum'");
    match(TokenType::KwEnum);

    auto node = std::make_shared<AST::EnumDeclaration>();
    node->underlyingType = "i32";

    if (check(TokenType::LBracket)) {
        parseAttributes();
    }

    if (check(TokenType::Identifier)) {
        node->name = current().value;
        advance();
    } else {
        error("E1441", "expected enum name", "enums are `enum Name { ... }`");
    }

    if (check(TokenType::Colon)) {
        advance();
        node->underlyingType = parseTypeName();
    }

    expect(TokenType::LBrace, "E1442", "'{' to begin enum body");
    match(TokenType::LBrace);

    long long nextImplicit = 0;
    skipNewlines();
    while (!atEnd() && !check(TokenType::RBrace)) {
        AST::EnumVariant variant;
        if (check(TokenType::Identifier)) {
            variant.name = current().value;
            advance();
        } else {
            error("E1443", "expected enum variant name", "variants are identifiers");
            break;
        }

        if (check(TokenType::Assign)) {
            advance();
            variant.hasExplicitValue = true;
            if (check(TokenType::IntegerLiteral)) {
                variant.value = ecxlit::parseIntegerValue(current().value);
                advance();
            } else {
                error("E1444", "expected integer value for enum variant", "");
            }
            nextImplicit = variant.value + 1;
        } else {
            variant.value = nextImplicit++;
        }

        node->variants.push_back(std::move(variant));

        match(TokenType::Comma);
        skipNewlines();
    }

    expect(TokenType::RBrace, "E1445", "'}' to close enum body");
    match(TokenType::RBrace);

    Symbol sym(node->name, "enum", true, false, start.line, start.column,
               static_cast<int>(node->name.size()), "", "");
    scopes_.declare(sym);

    fillRange(*node, start);
    return node;
}
