
#include <parser/parser.hpp>

#include <memory>

void ecxSetAllowStructLiteral(bool allow);

AST::NodePtr ecxParseCondition(Parser& parser) {
    ecxSetAllowStructLiteral(false);
    AST::NodePtr cond = parser.parseExpression();
    ecxSetAllowStructLiteral(true);
    return cond;
}

AST::NodePtr Parser::parseReturn() {
    const Token& start = current();
    advance();

    auto node = std::make_shared<AST::ReturnStatement>();

    if (!check(TokenType::Newline) && !check(TokenType::Semicolon) &&
        !check(TokenType::RBrace) && !atEnd()) {
        node->returnValue = parseExpression();
    }

    fillRange(*node, start);
    return node;
}

AST::NodePtr Parser::parseUnsafeBlock() {
    const Token& start = current();
    advance();

    auto node = std::make_shared<AST::UnsafeBlock>();
    skipNewlines();
    node->body = parseBlock();

    fillRange(*node, start);
    return node;
}
