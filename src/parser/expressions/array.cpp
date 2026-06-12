
#include <parser/parser.hpp>

#include <memory>

AST::NodePtr ecxBuildIndex(Parser& parser, AST::NodePtr object) {
    parser.advance();

    auto node = std::make_shared<AST::MemberAccessExpr>();
    node->object = object;
    node->computed = true;
    node->property = parser.parseExpression();

    parser.expect(TokenType::RBracket, "E1330", "']' to close index");
    parser.match(TokenType::RBracket);

    const Token& last = parser.previous();
    if (object) {
        node->range.startOffset = object->range.startOffset;
        node->range.startLine = object->range.startLine;
        node->range.startColumn = object->range.startColumn;
    }
    node->range.endOffset = last.end;
    node->range.endLine = last.line;
    node->range.endColumn = last.column;
    return node;
}

AST::NodePtr ecxParseArrayLiteral(Parser& parser) {
    const Token& start = parser.current();
    parser.advance();

    auto node = std::make_shared<AST::ArrayLiteral>();
    parser.skipNewlines();
    while (!parser.atEnd() && !parser.check(TokenType::RBracket)) {
        node->elements.push_back(parser.parseExpression());
        parser.skipNewlines();
        if (!parser.match(TokenType::Comma)) {
            break;
        }
        parser.skipNewlines();
    }
    parser.expect(TokenType::RBracket, "E1331", "']' to close array literal");
    parser.match(TokenType::RBracket);

    const Token& last = parser.previous();
    node->range.startOffset = start.start;
    node->range.startLine = start.line;
    node->range.startColumn = start.column;
    node->range.endOffset = last.end;
    node->range.endLine = last.line;
    node->range.endColumn = last.column;
    return node;
}
