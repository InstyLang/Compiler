
#include <parser/parser.hpp>

#include <memory>

AST::NodePtr ecxParseCondition(Parser& parser);

namespace {

AST::NodeList parseSwitchArmBody(Parser& parser) {
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

// `switch <subject> { Variant(a, b) => body, Unit => body, _ => body }`.
// Destructures a tagged-union value; each arm binds the named variant's payload
// fields to fresh identifiers scoped to the arm body.
AST::NodePtr Parser::parseSwitch() {
    const Token& start = current();
    advance();  // consume `switch`

    auto node = std::make_shared<AST::SwitchStatement>();
    node->subject = ecxParseCondition(*this);

    skipNewlines();
    expect(TokenType::LBrace, "E1460", "'{' to begin switch body");
    match(TokenType::LBrace);
    skipNewlines();

    while (!atEnd() && !check(TokenType::RBrace)) {
        AST::SwitchArm arm;

        if (check(TokenType::Identifier) && current().value == "_") {
            arm.isDefault = true;
            advance();
        } else if (check(TokenType::Identifier)) {
            arm.variant = current().value;
            advance();
            // Optional payload bindings: `Variant(a, b)`.
            if (check(TokenType::LParen)) {
                advance();
                skipNewlines();
                while (!atEnd() && !check(TokenType::RParen)) {
                    if (check(TokenType::Identifier)) {
                        arm.bindings.push_back(current().value);
                        advance();
                    } else {
                        error("E1461", "expected a binding name in match pattern",
                              "e.g. `Add(l, r) => ...`");
                        break;
                    }
                    skipNewlines();
                    if (!match(TokenType::Comma)) {
                        break;
                    }
                    skipNewlines();
                }
                expect(TokenType::RParen, "E1462", "')' to close switch bindings");
                match(TokenType::RParen);
            }
        } else {
            error("E1463", "expected a variant name or '_' in switch arm",
                  "switch arms look like `Variant(bindings) => body`");
            break;
        }

        expect(TokenType::FatArrow, "E1464", "'=>' after switch pattern");
        match(TokenType::FatArrow);
        skipNewlines();

        arm.body = parseSwitchArmBody(*this);
        node->arms.push_back(std::move(arm));

        match(TokenType::Comma);
        skipNewlines();
    }

    expect(TokenType::RBrace, "E1465", "'}' to close switch body");
    match(TokenType::RBrace);

    fillRange(*node, start);
    return node;
}





