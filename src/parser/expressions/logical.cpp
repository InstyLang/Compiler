
#include <parser/parser.hpp>

#include <memory>

namespace {
void spanRange(AST::ExprAST& node, const AST::NodePtr& lhs, const AST::NodePtr& rhs) {
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
}

AST::NodePtr Parser::parseLogical() {
    AST::NodePtr left = parseEquality();

    while (check(TokenType::AmpAmp) || check(TokenType::PipePipe)) {
        std::string op = current().value;
        advance();
        AST::NodePtr right = parseEquality();

        auto node = std::make_shared<AST::LogicalOperationExpr>();
        node->op = op;
        node->left = left;
        node->right = right;
        spanRange(*node, left, right);
        left = node;
    }

    return left;
}

AST::NodePtr Parser::parseEquality() {
    AST::NodePtr left = parseComparison();

    while (check(TokenType::EqEq) || check(TokenType::NotEq)) {
        std::string op = current().value;
        advance();
        AST::NodePtr right = parseComparison();

        auto node = std::make_shared<AST::EqualityCheckExpr>();
        node->op = op;
        node->left = left;
        node->right = right;
        spanRange(*node, left, right);
        left = node;
    }

    return left;
}
