
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
    branch.body = parseCompileTimeBlock();
    node->branches.push_back(std::move(branch));

    // Chain of `#else if <cond> { ... }`, optionally closed by `#else { ... }`.
    // A branch with no condition is the else, and nothing may follow it.
    skipNewlines();
    while (check(TokenType::Hash) && peek().type == TokenType::KwElse) {
        advance();  // '#'
        advance();  // 'else'

        AST::CompileTimeBranch next;
        if (check(TokenType::KwIf)) {
            advance();
            next.condition = ecxParseCondition(*this);
        }
        const bool isElse = next.condition == nullptr;
        skipNewlines();
        next.body = parseCompileTimeBlock();
        node->branches.push_back(std::move(next));
        skipNewlines();
        if (isElse) break;
    }

    fillRange(*node, start);
    return node;
}
