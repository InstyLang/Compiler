
#include <parser/parser.hpp>

#include <memory>

AST::NodePtr ecxParseCondition(Parser& parser);

namespace {

AST::NodeList parseArmBody(Parser& parser) {
    if (parser.check(TokenType::LBrace)) {
        return parser.parseBlock();
    }
    AST::NodeList body;
    if (AST::NodePtr stmt = parser.parseStatement()) {
        body.push_back(stmt);
    }
    return body;
}

}

AST::NodePtr Parser::parseSwitch() {
    const Token& start = current();
    advance();

    auto node = std::make_shared<AST::SwitchStatement>();
    node->subject = ecxParseCondition(*this);

    skipNewlines();
    expect(TokenType::LBrace, "E1410", "'{' to begin switch body");
    match(TokenType::LBrace);
    skipNewlines();

    while (!atEnd() && !check(TokenType::RBrace)) {
        AST::SwitchArm arm;

        if (check(TokenType::Identifier) && current().value == "_" &&
            peek().type == TokenType::Arrow) {
            arm.isDefault = true;
            advance();
        } else {
            while (!atEnd() && !check(TokenType::Arrow)) {
                if (AST::NodePtr pat = parseExpression()) {
                    arm.patterns.push_back(pat);
                }
                if (!match(TokenType::Comma)) {
                    break;
                }
                skipNewlines();
            }
        }

        expect(TokenType::Arrow, "E1411", "'->' after switch arm pattern");
        match(TokenType::Arrow);
        skipNewlines();

        arm.body = parseArmBody(*this);
        node->arms.push_back(std::move(arm));

        match(TokenType::Comma);
        skipNewlines();
    }

    expect(TokenType::RBrace, "E1412", "'}' to close switch body");
    match(TokenType::RBrace);

    fillRange(*node, start);
    return node;
}
