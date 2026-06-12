
#include <parser/parser.hpp>

#include <memory>

AST::NodePtr ecxBuildCall(Parser& parser, AST::NodePtr callee) {
    auto node = std::make_shared<AST::FunctionCallExpr>();
    node->callee = callee;

    if (parser.check(TokenType::Lt)) {
        parser.advance();
        while (!parser.atEnd() && !parser.check(TokenType::Gt)) {
            node->genericArgs.push_back(parser.parseTypeName());
            if (!parser.match(TokenType::Comma)) {
                break;
            }
        }
        parser.expect(TokenType::Gt, "E1310", "'>' to close call generics");
        parser.match(TokenType::Gt);
    }

    node->arguments = parser.parseArguments();

    if (callee) {
        node->range.startOffset = callee->range.startOffset;
        node->range.startLine = callee->range.startLine;
        node->range.startColumn = callee->range.startColumn;
    }
    const Token& last = parser.previous();
    node->range.endOffset = last.end;
    node->range.endLine = last.line;
    node->range.endColumn = last.column;
    return node;
}
