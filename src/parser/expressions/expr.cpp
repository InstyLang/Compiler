
#include <parser/parser.hpp>

AST::NodePtr Parser::parseExpression() {
    return parseAssignment();
}
