
#include <parser/parser.hpp>

#include <memory>

AST::NodePtr Parser::parseFunctionDeclaration(std::vector<AST::Attribute> attributes) {
    const Token& start = current();
    expect(TokenType::KwFun, "E1410", "'fun' to begin a function");
    match(TokenType::KwFun);

    auto node = std::make_shared<AST::FunctionDeclaration>();
    node->attributes = std::move(attributes);

    if (check(TokenType::LBracket)) {
        auto more = parseAttributes();
        for (auto& a : more) node->attributes.push_back(std::move(a));
    }

    if (check(TokenType::Identifier)) {
        node->name = current().value;
        advance();
    } else {
        error("E1411", "expected function name", "functions are `fun name(...)`");
    }

    node->genericParams = parseGenericParams();
    node->parameters = parseParameterList();

    if (check(TokenType::Arrow)) {
        advance();
        node->returnType = parseTypeName();
    } else {
        node->returnType = "void";
    }

    skipNewlines();
    if (check(TokenType::LBrace)) {
        node->hasBody = true;
        node->body = parseBlock();
    } else {
        node->hasBody = false;
    }

    fillRange(*node, start);
    return node;
}
