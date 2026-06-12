
#include <parser/parser.hpp>
#include <lexer/lexer.hpp>

#include <memory>
#include <string>

bool ecxLooksLikeVariableDecl(Parser& parser);

namespace {
void parseMethodTail(Parser& parser, AST::Method& method) {
    method.parameters = parser.parseParameterList();
    if (parser.check(TokenType::Arrow)) {
        parser.advance();
        method.returnType = parser.parseTypeName();
    } else {
        method.returnType = "void";
    }
    parser.skipNewlines();
    if (parser.check(TokenType::LBrace)) {
        method.body = parser.parseBlock();
    }
}

std::string parseOperatorSymbol(Parser& parser) {
    if (parser.check(TokenType::LBracket)) {
        parser.advance();
        parser.match(TokenType::RBracket);
        return "[]";
    }
    std::string sym = parser.current().value;
    parser.advance();
    return sym;
}
}

AST::NodePtr Parser::parseClassDeclaration(std::vector<AST::Attribute> attributes) {
    const Token& start = previous();

    auto node = std::make_shared<AST::ClassDeclaration>();
    node->attributes = std::move(attributes);

    if (check(TokenType::LBracket)) {
        auto more = parseAttributes();
        for (auto& a : more) node->attributes.push_back(std::move(a));
    }

    if (check(TokenType::Identifier)) {
        node->name = current().value;
        advance();
    } else {
        error("E1450", "expected class name", "classes are `class Name { ... }`");
    }

    node->genericParams = parseGenericParams();

    expect(TokenType::LBrace, "E1451", "'{' to begin class body");
    match(TokenType::LBrace);

    skipNewlines();
    while (!atEnd() && !check(TokenType::RBrace)) {
        size_t before = index_;

        std::vector<AST::Attribute> memberAttrs;
        if (check(TokenType::LBracket)) {
            memberAttrs = parseAttributes();
            skipNewlines();
        }

        if (check(TokenType::KwConstructor)) {
            advance();
            AST::Method m;
            m.name = "constructor";
            m.isConstructor = true;
            m.attributes = std::move(memberAttrs);
            parseMethodTail(*this, m);
            node->methods.push_back(std::move(m));
        } else if (check(TokenType::KwDestructor)) {
            advance();
            AST::Method m;
            m.name = "destructor";
            m.isDestructor = true;
            m.attributes = std::move(memberAttrs);
            parseMethodTail(*this, m);
            node->methods.push_back(std::move(m));
        } else if (check(TokenType::KwOperator)) {
            advance();
            AST::Method m;
            m.isOperator = true;
            m.operatorSymbol = parseOperatorSymbol(*this);
            m.name = "operator" + m.operatorSymbol;
            m.attributes = std::move(memberAttrs);
            parseMethodTail(*this, m);
            node->methods.push_back(std::move(m));
        } else if (check(TokenType::KwFun)) {
            advance();
            AST::Method m;
            if (check(TokenType::Identifier)) {
                m.name = current().value;
                advance();
            } else {
                error("E1452", "expected method name", "methods are `fun name(...)`");
            }
            m.attributes = std::move(memberAttrs);
            parseMethodTail(*this, m);
            node->methods.push_back(std::move(m));
        } else if (ecxLooksLikeVariableDecl(*this)) {
            AST::StructField field;
            field.type = parseTypeName();
            if (check(TokenType::Identifier)) {
                field.name = current().value;
                advance();
            } else {
                error("E1453", "expected field name", "fields are `Type name`");
            }
            node->fields.push_back(std::move(field));
        } else {
            error("E1454", "unexpected token in class body",
                  "expected a field, method, constructor, destructor, or operator");
            synchronize();
        }

        if (index_ == before && !atEnd()) {
            advance();
        }
        match(TokenType::Comma);
        skipNewlines();
    }

    expect(TokenType::RBrace, "E1455", "'}' to close class body");
    match(TokenType::RBrace);

    Symbol sym(node->name, "class", true, false, start.line, start.column,
               static_cast<int>(node->name.size()), "", "");
    scopes_.declare(sym);

    fillRange(*node, start);
    return node;
}
