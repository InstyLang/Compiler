
#include <parser/parser.hpp>

#include <memory>

AST::NodePtr ecxParseCondition(Parser& parser);

AST::NodePtr Parser::parseCompileTimeIf() {
    const Token& start = current();
    expect(TokenType::Hash, "E1460", "'#' to begin a compile-time directive");
    match(TokenType::Hash);

    auto node = std::make_shared<AST::CompileTimeIfExpr>();

    if (check(TokenType::KwIf)) {
        advance();
    } else {
        error("E1461", "expected 'if' after '#'", "compile-time conditional is `#if`");
    }

    AST::CompileTimeBranch branch;
    branch.condition = ecxParseCondition(*this);
    skipNewlines();
    branch.body = parseBlock();
    node->branches.push_back(std::move(branch));

    skipNewlines();
    if (check(TokenType::Hash) && peek().type == TokenType::KwElse) {
        advance();
        advance();
        AST::CompileTimeBranch elseBranch;
        elseBranch.condition = nullptr;
        skipNewlines();
        elseBranch.body = parseBlock();
        node->branches.push_back(std::move(elseBranch));
    }

    fillRange(*node, start);
    return node;
}
