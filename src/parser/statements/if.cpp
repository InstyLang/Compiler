
#include <parser/parser.hpp>

#include <memory>

AST::NodePtr ecxParseCondition(Parser& parser);

AST::NodePtr Parser::parseIf() {
    const Token& start = current();
    advance();

    auto node = std::make_shared<AST::IfStatement>();
    node->condition = ecxParseCondition(*this);

    skipNewlines();
    node->consequent = parseBlock();

    skipNewlines();
    if (check(TokenType::KwElse)) {
        advance();
        skipNewlines();
        if (check(TokenType::KwIf)) {
            node->alternate.push_back(parseIf());
        } else {
            node->alternate = parseBlock();
        }
    }

    fillRange(*node, start);
    return node;
}
