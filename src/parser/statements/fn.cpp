
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

// `export <declaration>` marks a top-level declaration as visible to modules
// that import this one. Without `export`, declarations are private to their
// module (private-by-default). This wraps the normal top-level declaration
// parsers and flips the `isExported` flag on whatever node is produced. It
// accepts an optional leading `[attrs]` block and the `extern` keyword, e.g.
// `export fun`, `export extern fun`, `export [conv(fastcc)] fun`, `export
// struct`, `export class`, `export enum`, and `export <global var>`.
AST::NodePtr Parser::parseExportDeclaration() {
    expect(TokenType::KwExport, "E1416", "'export' to begin an exported declaration");
    match(TokenType::KwExport);
    skipNewlines();

    std::vector<AST::Attribute> attrs;
    if (check(TokenType::LBracket)) {
        attrs = parseAttributes();
        skipNewlines();
    }

    AST::NodePtr decl;
    switch (current().type) {
        case TokenType::KwFun:
            decl = parseFunctionDeclaration(std::move(attrs));
            break;
        case TokenType::KwExtern:
            decl = parseExternDeclaration(std::move(attrs));
            break;
        case TokenType::KwStruct:
            advance();
            decl = parseStructDeclaration(std::move(attrs));
            break;
        case TokenType::KwClass:
            advance();
            decl = parseClassDeclaration(std::move(attrs));
            break;
        case TokenType::KwEnum:
            decl = parseEnumDeclaration();
            break;
        default:
            if (!attrs.empty()) {
                error("E1400", "attributes must precede a declaration",
                      "place [..] before fun/extern/struct/class/enum");
                return nullptr;
            }
            // Allow `export <global var>` (e.g. `export const i32 X = 1`).
            decl = parseStatement();
            break;
    }

    if (!decl) {
        return nullptr;
    }
    switch (decl->nodeType()) {
        case AST::NodeType::FunctionDeclaration:
            static_cast<AST::FunctionDeclaration*>(decl.get())->isExported = true;
            break;
        case AST::NodeType::StructDeclaration:
            static_cast<AST::StructDeclaration*>(decl.get())->isExported = true;
            break;
        case AST::NodeType::ClassDeclaration:
            static_cast<AST::ClassDeclaration*>(decl.get())->isExported = true;
            break;
        case AST::NodeType::EnumDeclaration:
            static_cast<AST::EnumDeclaration*>(decl.get())->isExported = true;
            break;
        case AST::NodeType::VariableDeclaration:
            static_cast<AST::VariableDeclarationExpr*>(decl.get())->isExported = true;
            break;
        default:
            error("E1417", "`export` must precede a function, type, or global declaration",
                  "e.g. `export fun name(...)` or `export struct Name { ... }`");
            break;
    }
    return decl;
}
