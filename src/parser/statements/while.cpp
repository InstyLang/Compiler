
#include <parser/parser.hpp>

#include <memory>

AST::NodePtr ecxParseCondition(Parser& parser);

AST::NodePtr Parser::parseWhile() {
    const Token& start = current();
    advance();

    auto node = std::make_shared<AST::WhileLoop>();
    node->condition = ecxParseCondition(*this);
    skipNewlines();
    node->body = parseBlock();

    fillRange(*node, start);
    return node;
}
