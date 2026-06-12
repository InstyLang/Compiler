
#include <parser/parser.hpp>

#include <memory>

void ecxSpanBinary(AST::ExprAST& node, const AST::NodePtr& lhs,
                   const AST::NodePtr& rhs) {
    if (lhs) {
        node.range.startOffset = lhs->range.startOffset;
        node.range.startLine = lhs->range.startLine;
        node.range.startColumn = lhs->range.startColumn;
    }
    if (rhs) {
        node.range.endOffset = rhs->range.endOffset;
        node.range.endLine = rhs->range.endLine;
        node.range.endColumn = rhs->range.endColumn;
    }
}

AST::NodePtr Parser::parseComparison() {
    AST::NodePtr left = parseShift();

    while (check(TokenType::Lt) || check(TokenType::Gt) ||
           check(TokenType::Le) || check(TokenType::Ge)) {
        std::string op = current().value;
        advance();
        AST::NodePtr right = parseShift();

        auto node = std::make_shared<AST::BinaryOperationExpr>();
        node->op = op;
        node->lhs = left;
        node->rhs = right;
        ecxSpanBinary(*node, left, right);
        left = node;
    }

    return left;
}
