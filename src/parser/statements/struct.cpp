
#include <parser/parser.hpp>

#include <memory>

AST::NodePtr Parser::parseStructDeclaration(std::vector<AST::Attribute> attributes) {
    const Token& start = previous();

    auto node = std::make_shared<AST::StructDeclaration>();
    node->attributes = std::move(attributes);

    if (check(TokenType::LBracket)) {
        auto more = parseAttributes();
        for (auto& a : more) node->attributes.push_back(std::move(a));
    }

    if (check(TokenType::Identifier)) {
        node->name = current().value;
        advance();
    } else {
        error("E1430", "expected struct name", "structs are `struct Name { ... }`");
    }

    node->genericParams = parseGenericParams();

    expect(TokenType::LBrace, "E1431", "'{' to begin struct body");
    match(TokenType::LBrace);

    skipNewlines();
    while (!atEnd() && !check(TokenType::RBrace)) {
        AST::StructField field;
        field.type = parseTypeName();
        if (check(TokenType::Identifier)) {
            field.name = current().value;
            advance();
        } else {
            error("E1432", "expected field name", "fields are `Type name`");
        }
        node->fields.push_back(std::move(field));

        match(TokenType::Comma);
        skipNewlines();
    }

    expect(TokenType::RBrace, "E1433", "'}' to close struct body");
    match(TokenType::RBrace);

    Symbol sym(node->name, "struct", true, false, start.line, start.column,
               static_cast<int>(node->name.size()), "", "");
    scopes_.declare(sym);

    fillRange(*node, start);
    return node;
}
