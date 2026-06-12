
#include <parser/parser.hpp>

#include <memory>

AST::NodePtr Parser::parseLoop() {
    const Token& start = current();
    advance();

    auto node = std::make_shared<AST::InfiniteLoop>();
    skipNewlines();
    node->body = parseBlock();

    fillRange(*node, start);
    return node;
}
