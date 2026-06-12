
#include <parser/parser.hpp>

#include <functional>
#include <memory>
#include <string>

namespace {
void parseBraceFields(Parser& parser,
                      const std::function<void(const std::string&, AST::NodePtr)>& sink) {
    parser.expect(TokenType::LBrace, "E1350", "'{' to begin literal body");
    parser.match(TokenType::LBrace);

    parser.skipNewlines();
    while (!parser.atEnd() && !parser.check(TokenType::RBrace)) {
        std::string key;
        if (parser.check(TokenType::Identifier)) {
            key = parser.current().value;
            parser.advance();
        } else {
            parser.error("E1351", "expected field name in literal",
                         "fields are `name: value`");
            break;
        }
        parser.expect(TokenType::Colon, "E1352", "':' after field name");
        parser.match(TokenType::Colon);

        AST::NodePtr value = parser.parseExpression();
        sink(key, value);

        parser.skipNewlines();
        if (!parser.match(TokenType::Comma)) {
            break;
        }
        parser.skipNewlines();
    }

    parser.expect(TokenType::RBrace, "E1353", "'}' to close literal body");
    parser.match(TokenType::RBrace);
}
}

AST::NodePtr ecxParseStructInstantiation(Parser& parser, const Token& nameTok) {
    parser.advance();

    auto node = std::make_shared<AST::StructInstantiation>();
    node->typeName = nameTok.value;

    parseBraceFields(parser, [&](const std::string& key, AST::NodePtr value) {
        AST::FieldValue fv;
        fv.name = key;
        fv.value = value;
        node->fieldValues.push_back(std::move(fv));
    });

    const Token& last = parser.previous();
    node->range.startOffset = nameTok.start;
    node->range.startLine = nameTok.line;
    node->range.startColumn = nameTok.column;
    node->range.endOffset = last.end;
    node->range.endLine = last.line;
    node->range.endColumn = last.column;
    return node;
}

AST::NodePtr ecxParseObjectLiteral(Parser& parser) {
    const Token& start = parser.current();
    auto node = std::make_shared<AST::ObjectLiteral>();

    parseBraceFields(parser, [&](const std::string& key, AST::NodePtr value) {
        auto prop = std::make_shared<AST::ObjectProperty>();
        prop->key = key;
        prop->value = value;
        if (value) {
            prop->range = value->range;
        }
        node->properties.push_back(prop);
    });

    const Token& last = parser.previous();
    node->range.startOffset = start.start;
    node->range.startLine = start.line;
    node->range.startColumn = start.column;
    node->range.endOffset = last.end;
    node->range.endLine = last.line;
    node->range.endColumn = last.column;
    return node;
}
