
#include <parser/parser.hpp>

#include <utilities/errors.hpp>

#include <memory>

namespace {

void recordImport(AST::ProgramRoot& root, const AST::NodePtr& node) {
    if (auto imp = AST::ast_cast<AST::ImportStatement>(node)) {
        root.imports.push_back(imp->moduleName);
    }
}

}

std::shared_ptr<AST::ProgramRoot> Parser::produceAST(std::string& sourceCode) {
    Lexer lexer;
    std::vector<Token> tokens = lexer.tokenize(sourceCode);
    return produceASTFromTokens(std::move(tokens));
}

std::shared_ptr<AST::ProgramRoot>
Parser::produceASTFromTokens(std::vector<Token> tokens) {
    tokens_ = std::move(tokens);
    index_ = 0;
    panicMode_ = false;
    scopes_.reset();

    auto root = std::make_shared<AST::ProgramRoot>();

    if (tokens_.empty()) {
        return root;
    }

    const Token& first = tokens_.front();
    fillRange(*root, first, tokens_.back());

    skipNewlines();
    if (check(TokenType::KwModule)) {
        advance();
        if (check(TokenType::Identifier)) {
            std::string name = current().value;
            advance();
            while (check(TokenType::Dot) && peek().type == TokenType::Identifier) {
                advance();
                name += ".";
                name += current().value;
                advance();
            }
            root->moduleName = name;
        } else {
            error("E1100", "expected module name after 'module'",
                  "module names are identifiers, e.g. `module io`");
        }
        consumeStatementEnd();
    }

    predeclareTopLevel();

    while (!atEnd()) {
        skipNewlines();
        if (atEnd()) {
            break;
        }

        size_t before = index_;
        AST::NodePtr node = parseTopLevel();

        if (node) {
            recordImport(*root, node);
            root->body.push_back(node);
        }

        if (index_ == before && !atEnd()) {
            advance();
        }

        consumeStatementEnd();
    }

    return root;
}

void Parser::predeclareTopLevel() {
    size_t saved = index_;
    bool savedPanic = panicMode_;

    while (!atEnd()) {
        const Token& tok = current();

        if (tok.type == TokenType::LBracket) {
            int depth = 0;
            do {
                if (check(TokenType::LBracket)) ++depth;
                else if (check(TokenType::RBracket)) --depth;
                advance();
            } while (!atEnd() && depth > 0);
            continue;
        }

        TokenType t = tok.type;
        if (t == TokenType::KwFun || t == TokenType::KwStruct ||
            t == TokenType::KwClass || t == TokenType::KwEnum) {
            advance();
            if (check(TokenType::LBracket)) {
                int depth = 0;
                do {
                    if (check(TokenType::LBracket)) ++depth;
                    else if (check(TokenType::RBracket)) --depth;
                    advance();
                } while (!atEnd() && depth > 0);
            }
            if (check(TokenType::Identifier)) {
                const Token& nameTok = current();
                std::string kind = (t == TokenType::KwFun)      ? "function"
                                    : (t == TokenType::KwStruct) ? "struct"
                                    : (t == TokenType::KwClass)  ? "class"
                                                                 : "enum";
                Symbol sym(nameTok.value, kind,  true,
                            false, nameTok.line, nameTok.column,
                           static_cast<int>(nameTok.value.size()), "", "");
                scopes_.declare(sym);
            }
        }
        advance();
    }

    index_ = saved;
    panicMode_ = savedPanic;
}

void Parser::fillRange(AST::ExprAST& node, const Token& startToken) const {
    fillRange(node, startToken, previous());
}

void Parser::fillRange(AST::ExprAST& node, const Token& startToken,
                       const Token& endToken) const {
    node.range.startOffset = startToken.start;
    node.range.startLine = startToken.line;
    node.range.startColumn = startToken.column;
    node.range.endOffset = endToken.end;
    node.range.endLine = endToken.line;
    node.range.endColumn = endToken.column;
    if (node.range.endOffset < node.range.startOffset) {
        node.range.endOffset = node.range.startOffset;
    }
}

void Parser::error(const std::string& code, const std::string& message,
                   const std::string& hint) {
    errorAt(current(), code, message, hint);
}

void Parser::errorAt(const Token& token, const std::string& code,
                     const std::string& message, const std::string& hint) {
    if (panicMode_) {
        return;
    }
    panicMode_ = true;

    if (ErrorReporting::globalErrorReporter) {
        int length = token.end - token.start;
        if (length <= 0) {
            length = static_cast<int>(token.value.size());
            if (length <= 0) length = 1;
        }
        ErrorReporting::globalErrorReporter->error(
            code, message,
            ErrorReporting::SourceLocation{token.line, token.column, length,
                                           token.start},
            hint);
    }
}
