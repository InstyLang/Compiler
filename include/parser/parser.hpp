#pragma once


#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <extra/ast.hpp>
#include <lexer/lexer.hpp>
#include <parser/scope_manager.hpp>

class Parser {
public:
    Parser() = default;

    std::shared_ptr<AST::ProgramRoot> produceAST(std::string& sourceCode);
    std::shared_ptr<AST::ProgramRoot> produceASTFromTokens(std::vector<Token> tokens);

    ScopeManager getScopeManager() const { return scopes_; }

    const Token& current() const;
    const Token& peek(size_t ahead = 1) const;
    const Token& previous() const;
    bool atEnd() const;
    const Token& advance();
    bool check(TokenType type) const;
    bool match(TokenType type);
    const Token& expect(TokenType type, const std::string& code, const std::string& what);
    void skipNewlines();
    void consumeStatementEnd();
    void synchronize();

    void error(const std::string& code, const std::string& message, const std::string& hint = "");
    void errorAt(const Token& token, const std::string& code, const std::string& message,
                 const std::string& hint = "");

    AST::NodePtr parseTopLevel();
    AST::NodePtr parseStatement();
    AST::NodeList parseBlock();
    AST::NodePtr parseFunctionDeclaration(std::vector<AST::Attribute> attributes);
    AST::NodePtr parseExternDeclaration(std::vector<AST::Attribute> attributes);
    AST::NodePtr parseExportDeclaration();
    AST::NodePtr parseStructDeclaration(std::vector<AST::Attribute> attributes);
    AST::NodePtr parseClassDeclaration(std::vector<AST::Attribute> attributes);
    AST::NodePtr parseEnumDeclaration();
    AST::NodePtr parseImport();
    AST::NodePtr parseIf();
    AST::NodePtr parseWhile();
    AST::NodePtr parseLoop();
    AST::NodePtr parseWhen();
    AST::NodePtr parseSwitch();
    AST::NodePtr parseReturn();
    AST::NodePtr parseUnsafeBlock();
    AST::NodePtr parseCompileTimeIf();
    AST::NodePtr parseVariableOrExpressionStatement();
    AST::NodePtr parseSectionBlock();

    std::vector<AST::Attribute> parseAttributes();
    std::vector<AST::Parameter> parseParameterList();
    std::vector<std::string> parseGenericParams();
    std::string parseTypeName();

    AST::NodePtr parseExpression();
    AST::NodePtr parseAssignment();
    AST::NodePtr parseLogical();
    AST::NodePtr parseEquality();
    AST::NodePtr parseComparison();
    AST::NodePtr parseShift();
    AST::NodePtr parseAdditive();
    AST::NodePtr parseMultiplicative();
    AST::NodePtr parseUnary();
    AST::NodePtr parsePostfix(AST::NodePtr base);
    AST::NodePtr parsePrimary();
    AST::NodeList parseArguments();

private:
    std::vector<Token> tokens_;
    size_t index_ = 0;
    ScopeManager scopes_;
    bool panicMode_ = false;

    void predeclareTopLevel();
    void fillRange(AST::ExprAST& node, const Token& startToken) const;
    void fillRange(AST::ExprAST& node, const Token& startToken, const Token& endToken) const;
};
