
#include <parser/parser.hpp>

#include <memory>

AST::NodePtr ecxBuildCall(Parser& parser, AST::NodePtr callee);
AST::NodePtr ecxBuildMemberAccess(Parser& parser, AST::NodePtr object);
AST::NodePtr ecxBuildScopeAccess(Parser& parser, AST::NodePtr object);
AST::NodePtr ecxBuildIndex(Parser& parser, AST::NodePtr object);

namespace {
bool looksLikeGenericCall(Parser& parser) {
    if (!parser.check(TokenType::Lt)) {
        return false;
    }
    size_t ahead = 1;
    int depth = 1;
    while (true) {
        const Token& t = parser.peek(ahead);
        switch (t.type) {
            case TokenType::Lt:
                ++depth;
                break;
            case TokenType::Gt:
                --depth;
                if (depth == 0) {
                    return parser.peek(ahead + 1).type == TokenType::LParen;
                }
                break;
            case TokenType::Identifier:
            case TokenType::Comma:
            case TokenType::Star:
            case TokenType::KwVolatile:
            case TokenType::LBracket:
            case TokenType::RBracket:
            case TokenType::IntegerLiteral:
                break;
            default:
                return false;
        }
        ++ahead;
        if (ahead > 64) {
            return false;
        }
    }
}
}

AST::NodePtr Parser::parsePostfix(AST::NodePtr base) {
    AST::NodePtr node = base;

    while (!atEnd()) {
        if (check(TokenType::LParen)) {
            node = ecxBuildCall(*this, node);
        } else if (check(TokenType::Lt) && looksLikeGenericCall(*this)) {
            node = ecxBuildCall(*this, node);
        } else if (check(TokenType::Dot)) {
            node = ecxBuildMemberAccess(*this, node);
        } else if (check(TokenType::ColonColon)) {
            node = ecxBuildScopeAccess(*this, node);
        } else if (check(TokenType::LBracket)) {
            node = ecxBuildIndex(*this, node);
        } else {
            break;
        }
    }

    return node;
}
