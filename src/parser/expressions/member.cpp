
#include <parser/parser.hpp>

#include <memory>

namespace {
AST::NodePtr buildAccess(Parser& parser, AST::NodePtr object, bool scope) {
    parser.advance();

    auto node = std::make_shared<AST::MemberAccessExpr>();
    node->object = object;
    node->computed = false;
    node->isScope = scope;

    auto prop = std::make_shared<AST::IdentifierExpr>();
    const Token& nameTok = parser.current();
    if (parser.check(TokenType::Identifier) || parser.check(TokenType::KwThis)) {
        prop->name = nameTok.value;
        parser.advance();
    } else if (scope) {
        parser.error("E1321", "expected name after '::'",
                     "scope access is `module::name`");
    } else {
        parser.error("E1320", "expected member name after '.'",
                     "member access is `object.field`");
    }
    prop->range.startOffset = nameTok.start;
    prop->range.startLine = nameTok.line;
    prop->range.startColumn = nameTok.column;
    prop->range.endOffset = nameTok.end;
    prop->range.endLine = nameTok.line;
    prop->range.endColumn = nameTok.column;
    node->property = prop;

    if (object) {
        node->range.startOffset = object->range.startOffset;
        node->range.startLine = object->range.startLine;
        node->range.startColumn = object->range.startColumn;
    }
    node->range.endOffset = nameTok.end;
    node->range.endLine = nameTok.line;
    node->range.endColumn = nameTok.column;
    return node;
}
}

AST::NodePtr ecxBuildMemberAccess(Parser& parser, AST::NodePtr object) {
    return buildAccess(parser, std::move(object), false);
}

AST::NodePtr ecxBuildScopeAccess(Parser& parser, AST::NodePtr object) {
    return buildAccess(parser, std::move(object), true);
}
