
#include <parser/parser.hpp>

#include <memory>
#include <string>

bool ecxLooksLikeVariableDecl(Parser& parser);
namespace ecxlit {
long long parseIntegerValue(const std::string& raw);
}

AST::NodePtr Parser::parseVariableOrExpressionStatement() {
    if (!ecxLooksLikeVariableDecl(*this)) {
        return parseExpression();
    }

    const Token& start = current();
    auto node = std::make_shared<AST::VariableDeclarationExpr>();

    if (check(TokenType::KwConst)) {
        node->isConst = true;
        advance();
    } else if (check(TokenType::KwLet)) {
        advance();
    }

    {
        node->typeHint = parseTypeName();
        std::string& s = node->typeHint;
        auto lb = s.rfind('[');
        if (lb != std::string::npos && s.back() == ']') {
            node->isArray = true;
            std::string inner = s.substr(lb + 1, s.size() - lb - 2);
            if (!inner.empty()) {
                node->arraySize =
                    static_cast<int>(ecxlit::parseIntegerValue(inner));
            }
        }
    }

    if (check(TokenType::Identifier)) {
        node->identifier = current().value;
        advance();
    } else {
        error("E1420", "expected variable name", "declarations are `Type name`");
    }

    if (check(TokenType::Assign)) {
        advance();
        node->initialValue = parseExpression();
    }

    match(TokenType::Semicolon);

    Symbol sym(node->identifier, node->typeHint, scopes_.scopeDepth() <= 1,
                false, start.line, start.column,
               static_cast<int>(node->identifier.size()), "", "");
    scopes_.declare(sym);

    fillRange(*node, start);
    return node;
}
