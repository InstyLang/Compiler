
#include <parser/parser.hpp>
#include <lexer/lexer.hpp>

#include <memory>
#include <string>

namespace ecxlit {
std::shared_ptr<AST::IntegerLiteral> makeInteger(const std::string& raw);
std::shared_ptr<AST::FloatLiteral> makeFloat(const std::string& raw);
std::shared_ptr<AST::BoolLiteral> makeBool(bool value);
std::shared_ptr<AST::StringLiteral> makeString(const std::string& decoded);
}

AST::NodePtr ecxParseArrayLiteral(Parser& parser);
AST::NodePtr ecxParseStructInstantiation(Parser& parser, const Token& nameTok);

namespace {
bool g_allowStructLiteral = true;
}
void ecxSetAllowStructLiteral(bool allow) { g_allowStructLiteral = allow; }
bool ecxAllowStructLiteral() { return g_allowStructLiteral; }


AST::NodePtr Parser::parsePrimary() {
    const Token& start = current();

    switch (start.type) {
        case TokenType::IntegerLiteral: {
            advance();
            auto node = ecxlit::makeInteger(start.value);
            fillRange(*node, start, start);
            return node;
        }
        case TokenType::FloatLiteral: {
            advance();
            auto node = ecxlit::makeFloat(start.value);
            fillRange(*node, start, start);
            return node;
        }
        case TokenType::StringLiteral: {
            advance();
            auto node = ecxlit::makeString(start.value);
            fillRange(*node, start, start);
            return node;
        }
        case TokenType::CharLiteral: {
            advance();
            auto node = std::make_shared<AST::IntegerLiteral>();
            node->raw = start.value;
            node->value = start.value.empty()
                              ? 0
                              : static_cast<long long>(
                                    static_cast<unsigned char>(start.value[0]));
            fillRange(*node, start, start);
            return node;
        }
        case TokenType::KwTrue: {
            advance();
            auto node = ecxlit::makeBool(true);
            fillRange(*node, start, start);
            return node;
        }
        case TokenType::KwFalse: {
            advance();
            auto node = ecxlit::makeBool(false);
            fillRange(*node, start, start);
            return node;
        }
        case TokenType::KwThis: {
            advance();
            auto node = std::make_shared<AST::IdentifierExpr>();
            node->name = "this";
            fillRange(*node, start, start);
            return node;
        }
        case TokenType::Identifier: {
            if (peek().type == TokenType::LBrace && ecxAllowStructLiteral()) {
                return ecxParseStructInstantiation(*this, start);
            }
            advance();
            auto node = std::make_shared<AST::IdentifierExpr>();
            node->name = start.value;
            fillRange(*node, start, start);
            return node;
        }
        case TokenType::LParen: {
            advance();
            AST::NodePtr inner = parseExpression();
            expect(TokenType::RParen, "E1300", "')' to close grouping");
            match(TokenType::RParen);
            return inner;
        }
        case TokenType::LBracket: {
            return ecxParseArrayLiteral(*this);
        }
        case TokenType::At: {
            advance();
            auto node = std::make_shared<AST::BuiltinCallExpr>();
            if (check(TokenType::Identifier)) {
                node->name = current().value;
                advance();
            } else {
                error("E1301", "expected builtin name after '@'",
                      "e.g. @syscall(...)");
            }
            if (check(TokenType::Lt)) {
                advance();
                while (!atEnd() && !check(TokenType::Gt)) {
                    node->genericArgs.push_back(parseTypeName());
                    if (!match(TokenType::Comma)) break;
                }
                expect(TokenType::Gt, "E1302", "'>' to close builtin generics");
                match(TokenType::Gt);
            }
            node->arguments = parseArguments();
            fillRange(*node, start);
            return node;
        }
        case TokenType::KwNew: {
            advance();
            auto node = std::make_shared<AST::NewExpression>();
            node->typeName = parseTypeName();
            if (check(TokenType::LBracket)) {
                advance();
                node->arraySize = parseExpression();
                expect(TokenType::RBracket, "E1303", "']' to close new[]");
                match(TokenType::RBracket);
            }
            if (check(TokenType::LParen)) {
                node->arguments = parseArguments();
            }
            fillRange(*node, start);
            return node;
        }
        case TokenType::KwDelete: {
            advance();
            auto node = std::make_shared<AST::DeleteExpression>();
            node->operand = parseUnary();
            fillRange(*node, start);
            return node;
        }
        default:
            break;
    }

    if (start.type == TokenType::Identifier && start.value == "asm") {
        advance();
        auto node = std::make_shared<AST::InlineAsmExpr>();
        if (match(TokenType::Lt)) {
            node->returnType = parseTypeName();
            expect(TokenType::Gt, "E1304", "'>' after asm return type");
            match(TokenType::Gt);
        }
        expect(TokenType::LParen, "E1305", "'(' after asm");
        match(TokenType::LParen);
        if (check(TokenType::StringLiteral)) {
            node->templateString = current().value;
            advance();
        }
        if (match(TokenType::Comma)) {
            if (check(TokenType::StringLiteral)) {
                node->constraints = current().value;
                advance();
            }
            while (match(TokenType::Comma)) {
                node->inputs.push_back(parseExpression());
            }
        }
        expect(TokenType::RParen, "E1306", "')' to close asm");
        match(TokenType::RParen);
        fillRange(*node, start);
        return node;
    }

    error("E1307", "unexpected token in expression",
          "expected a value, name, or '('");
    auto placeholder = std::make_shared<AST::IdentifierExpr>();
    placeholder->name = "";
    fillRange(*placeholder, start, start);
    if (!atEnd()) advance();
    return placeholder;
}
