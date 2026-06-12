
#include <parser/parser.hpp>

#include <memory>

void ecxSpanBinary(AST::ExprAST& node, const AST::NodePtr& lhs,
                   const AST::NodePtr& rhs);

AST::NodePtr Parser::parseShift() {
    AST::NodePtr left = parseAdditive();

    while (check(TokenType::Shl) || check(TokenType::Shr)) {
        std::string op = current().value;
        advance();
        AST::NodePtr right = parseAdditive();

        auto node = std::make_shared<AST::ShiftOperationExpr>();
        node->op = op;
        node->lhs = left;
        node->rhs = right;
        ecxSpanBinary(*node, left, right);
        left = node;
    }

    return left;
}
