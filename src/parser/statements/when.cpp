
#include <parser/parser.hpp>

#include <memory>

AST::NodePtr ecxParseCondition(Parser& parser);

AST::NodePtr Parser::parseWhen() {
    const Token& start = current();
    advance();

    auto node = std::make_shared<AST::WhenStatement>();
    node->condition = ecxParseCondition(*this);
    skipNewlines();
    node->consequent = parseBlock();

    fillRange(*node, start);
    return node;
}
