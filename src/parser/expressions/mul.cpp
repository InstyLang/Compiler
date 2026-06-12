
#include <parser/parser.hpp>

#include <memory>

void ecxSpanBinary(AST::ExprAST& node, const AST::NodePtr& lhs,
                   const AST::NodePtr& rhs);

AST::NodePtr Parser::parseMultiplicative() {
    AST::NodePtr left = parseUnary();

    while (check(TokenType::Star) || check(TokenType::Slash) ||
           check(TokenType::Percent)) {
        std::string op = current().value;
        advance();
        AST::NodePtr right = parseUnary();

        auto node = std::make_shared<AST::BinaryOperationExpr>();
        node->op = op;
        node->lhs = left;
        node->rhs = right;
        ecxSpanBinary(*node, left, right);
        left = node;
    }

    return left;
}
