
#include <parser/parser.hpp>

#include <memory>

AST::NodePtr Parser::parseFunctionDeclaration(std::vector<AST::Attribute> attributes) {
    const Token& start = current();
    expect(TokenType::KwFun, "E1410", "'fun' to begin a function");
    match(TokenType::KwFun);

    auto node = std::make_shared<AST::FunctionDeclaration>();
    node->attributes = std::move(attributes);

    if (check(TokenType::LBracket)) {
        auto more = parseAttributes();
        for (auto& a : more) node->attributes.push_back(std::move(a));
    }

    if (check(TokenType::Identifier)) {
        node->name = current().value;
        advance();
    } else {
        error("E1411", "expected function name", "functions are `fun name(...)`");
    }

    node->genericParams = parseGenericParams();
    node->parameters = parseParameterList();

    if (check(TokenType::Arrow)) {
        advance();
        node->returnType = parseTypeName();
    } else {
        node->returnType = "void";
    }

    skipNewlines();
    if (check(TokenType::LBrace)) {
        node->hasBody = true;
        node->body = parseBlock();
    } else {
        node->hasBody = false;
    }

    fillRange(*node, start);
    return node;
}

// `extern fun name(params) -> ret` declares a function defined elsewhere (an
// external C/ABI symbol). It is parsed exactly like a normal function but must
// be bodyless, and the name is not mangled (handled in sema) so it links
// against the external symbol directly.
AST::NodePtr Parser::parseExternDeclaration(std::vector<AST::Attribute> attributes) {
    expect(TokenType::KwExtern, "E1414", "'extern' to begin an external declaration");
    match(TokenType::KwExtern);

    AST::NodePtr decl = parseFunctionDeclaration(std::move(attributes));
    // parseFunctionDeclaration always returns a FunctionDeclaration node.
    if (decl && decl->nodeType() == AST::NodeType::FunctionDeclaration) {
        auto* fn = static_cast<AST::FunctionDeclaration*>(decl.get());
        fn->isExtern = true;
        if (fn->hasBody) {
            error("E1415", "extern functions cannot have a body",
                  "declare the signature only: `extern fun name(...) -> type`");
        }
    }
    return decl;
}
