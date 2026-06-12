
#include <parser/parser.hpp>

#include <memory>

AST::NodePtr Parser::parseAssignment() {
    AST::NodePtr left = parseLogical();

    if (check(TokenType::Assign)) {
        advance();
        AST::NodePtr value = parseAssignment();

        auto node = std::make_shared<AST::AssignmentExpr>();
        node->target = left;
        node->value = value;
        node->range.startOffset = left ? left->range.startOffset : -1;
        node->range.startLine = left ? left->range.startLine : 0;
        node->range.startColumn = left ? left->range.startColumn : 0;
        node->range.endOffset = value ? value->range.endOffset : node->range.startOffset;
        node->range.endLine = value ? value->range.endLine : node->range.startLine;
        node->range.endColumn = value ? value->range.endColumn : node->range.startColumn;
        return node;
    }

    return left;
}
