
#include <parser/parser.hpp>

#include <memory>
#include <string>

std::string ecxParseScopedModulePath(Parser& parser);

AST::NodePtr Parser::parseTopLevel() {
    if (check(TokenType::LBracket)) {
        std::vector<AST::Attribute> attrs = parseAttributes();
        skipNewlines();
        if (check(TokenType::KwFun)) {
            return parseFunctionDeclaration(std::move(attrs));
        }
        if (check(TokenType::KwExtern)) {
            return parseExternDeclaration(std::move(attrs));
        }
        if (check(TokenType::KwStruct)) {
            advance();
            return parseStructDeclaration(std::move(attrs));
        }
        if (check(TokenType::KwClass)) {
            advance();
            return parseClassDeclaration(std::move(attrs));
        }
        if (check(TokenType::KwEnum)) {
            return parseEnumDeclaration();
        }
        error("E1400", "attributes must precede a declaration",
              "place [..] before fun/struct/class");
        return nullptr;
    }

    switch (current().type) {
        case TokenType::KwImport:
            return parseImport();
        case TokenType::KwFun:
            return parseFunctionDeclaration({});
        case TokenType::KwExtern:
            return parseExternDeclaration({});
        case TokenType::KwStruct:
            advance();
            return parseStructDeclaration({});
        case TokenType::KwClass:
            advance();
            return parseClassDeclaration({});
        case TokenType::KwEnum:
            return parseEnumDeclaration();
        case TokenType::KwSection:
            return parseSectionBlock();
        case TokenType::Hash:
            return parseCompileTimeIf();
        default:
            break;
    }

    return parseStatement();
}

AST::NodePtr Parser::parseStatement() {
    switch (current().type) {
        case TokenType::KwIf:
            return parseIf();
        case TokenType::KwWhile:
            return parseWhile();
        case TokenType::KwLoop:
            return parseLoop();
        case TokenType::KwWhen:
            return parseWhen();
        case TokenType::KwSwitch:
            return parseSwitch();
        case TokenType::KwReturn:
            return parseReturn();
        case TokenType::KwUnsafe:
            return parseUnsafeBlock();
        case TokenType::KwFun:
            return parseFunctionDeclaration({});
        case TokenType::Hash:
            return parseCompileTimeIf();
        case TokenType::KwBreak: {
            const Token& start = current();
            advance();
            auto node = std::make_shared<AST::BreakStatement>();
            fillRange(*node, start, start);
            return node;
        }
        case TokenType::KwSkip: {
            const Token& start = current();
            advance();
            auto node = std::make_shared<AST::SkipStatement>();
            fillRange(*node, start, start);
            return node;
        }
        default:
            break;
    }

    return parseVariableOrExpressionStatement();
}

AST::NodeList Parser::parseBlock() {
    AST::NodeList body;

    expect(TokenType::LBrace, "E1401", "'{' to begin block");
    match(TokenType::LBrace);

    skipNewlines();
    while (!atEnd() && !check(TokenType::RBrace)) {
        size_t before = index_;
        AST::NodePtr stmt = parseStatement();
        if (stmt) {
            body.push_back(stmt);
        }
        if (index_ == before && !atEnd()) {
            synchronize();
        }
        consumeStatementEnd();
    }

    expect(TokenType::RBrace, "E1402", "'}' to close block");
    match(TokenType::RBrace);
    return body;
}

AST::NodePtr Parser::parseImport() {
    const Token& start = current();
    advance();

    auto node = std::make_shared<AST::ImportStatement>();
    node->moduleName = ecxParseScopedModulePath(*this);

    if (check(TokenType::Star)) {
        advance();
        node->isWildcard = true;
    }

    if (check(TokenType::Dot) && peek().type == TokenType::LBrace) {
        advance();
        advance();
        skipNewlines();
        while (!atEnd() && !check(TokenType::RBrace)) {
            if (check(TokenType::Identifier)) {
                node->importedSymbols.push_back(current().value);
                advance();
            } else {
                break;
            }
            if (!match(TokenType::Comma)) {
                break;
            }
            skipNewlines();
        }
        skipNewlines();
        expect(TokenType::RBrace, "E1403", "'}' to close selective import");
        match(TokenType::RBrace);
    }

    if (check(TokenType::KwAs)) {
        advance();
        if (check(TokenType::Identifier)) {
            node->alias = current().value;
            advance();
        } else {
            error("E1404", "expected alias name after 'as'", "e.g. import io as i");
        }
    }

    fillRange(*node, start);
    return node;
}

AST::NodePtr Parser::parseSectionBlock() {
    advance();

    if (check(TokenType::Identifier) || check(TokenType::StringLiteral)) {
        advance();
    }

    auto node = std::make_shared<AST::UnsafeBlock>();
    const Token& bodyStart = current();
    if (check(TokenType::LBrace)) {
        node->body = parseBlock();
    }
    fillRange(*node, bodyStart);
    return node;
}
