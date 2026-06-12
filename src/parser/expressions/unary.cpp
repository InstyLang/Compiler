
#include <parser/parser.hpp>

#include <memory>

AST::NodePtr Parser::parseUnary() {
    const Token& start = current();

    if (check(TokenType::Bang) || check(TokenType::Minus)) {
        std::string op = current().value;
        advance();
        AST::NodePtr operand = parseUnary();
        auto node = std::make_shared<AST::UnaryExpr>();
        node->op = op;
        node->operand = operand;
        fillRange(*node, start);
        return node;
    }

    if (check(TokenType::Amp)) {
        advance();
        AST::NodePtr operand = parseUnary();
        auto node = std::make_shared<AST::AddressOfExpr>();
        node->operand = operand;
        fillRange(*node, start);
        return node;
    }

    if (check(TokenType::Tilde)) {
        advance();
        AST::NodePtr operand = parseUnary();
        auto node = std::make_shared<AST::DereferenceExpr>();
        node->operand = operand;
        fillRange(*node, start);
        return node;
    }

    if (check(TokenType::KwCast)) {
        advance();
        std::string targetType;
        if (match(TokenType::Lt)) {
            targetType = parseTypeName();
            expect(TokenType::Gt, "E1200", "'>' after cast target type");
            match(TokenType::Gt);
        } else {
            error("E1201", "expected '<' after 'cast'", "cast syntax: cast<T>(expr)");
        }
        expect(TokenType::LParen, "E1202", "'(' after cast type");
        match(TokenType::LParen);
        AST::NodePtr inner = parseExpression();
        expect(TokenType::RParen, "E1203", "')' to close cast");
        match(TokenType::RParen);

        auto node = std::make_shared<AST::CastExpr>();
        node->targetType = targetType;
        node->expression = inner;
        fillRange(*node, start);
        return node;
    }

    return parsePostfix(parsePrimary());
}
