
#include <parser/parser.hpp>

#include <memory>

void ecxSpanBinary(AST::ExprAST& node, const AST::NodePtr& lhs,
                   const AST::NodePtr& rhs);

AST::NodePtr Parser::parseAdditive() {
    AST::NodePtr left = parseMultiplicative();

    while (check(TokenType::Plus) || check(TokenType::Minus) ||
           check(TokenType::Pipe) || check(TokenType::Caret) ||
           check(TokenType::Amp)) {
        std::string op = current().value;
        advance();
        AST::NodePtr right = parseMultiplicative();

        auto node = std::make_shared<AST::BinaryOperationExpr>();
        node->op = op;
        node->lhs = left;
        node->rhs = right;
        ecxSpanBinary(*node, left, right);
        left = node;
    }

    return left;
}
