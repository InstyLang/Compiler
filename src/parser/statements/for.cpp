
#include <parser/parser.hpp>

#include <memory>

void ecxSetAllowStructLiteral(bool allow);

// `for <var> in <iterable> { ... }`
// `for <var> in <start>..<end> { ... }`
//
// `in` is a contextual keyword (a plain identifier spelled "in"), so it stays
// usable as an ordinary name elsewhere. The range form is recognized by a `..`
// following the first expression after `in`; otherwise the expression is an
// iterable (slice / fixed array / text).
AST::NodePtr Parser::parseFor() {
    const Token& start = current();
    advance();  // consume `for`

    auto node = std::make_shared<AST::ForLoop>();

    if (check(TokenType::Identifier)) {
        node->varName = current().value;
        advance();
    } else {
        error("E1450", "expected loop variable name after 'for'",
              "for syntax: `for x in <iterable>` or `for i in a..b`");
    }

    if (check(TokenType::Identifier) && current().value == "in") {
        advance();
    } else {
        error("E1451", "expected 'in' after the loop variable",
              "for syntax: `for x in <iterable>` or `for i in a..b`");
    }

    // Parse the iterable / range bounds with struct-literal syntax disabled, so
    // the `{` that opens the loop body is not mistaken for `Iterable { ... }`.
    ecxSetAllowStructLiteral(false);
    AST::NodePtr first = parseExpression();
    if (check(TokenType::DotDot)) {
        advance();
        node->isRange = true;
        node->rangeStart = first;
        node->rangeEnd = parseExpression();
    } else {
        node->iterable = first;
    }
    ecxSetAllowStructLiteral(true);

    skipNewlines();
    node->body = parseBlock();

    fillRange(*node, start);
    return node;
}
