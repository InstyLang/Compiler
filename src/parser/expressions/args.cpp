
#include <parser/parser.hpp>

AST::NodeList Parser::parseArguments() {
    AST::NodeList args;

    expect(TokenType::LParen, "E1340", "'(' to begin arguments");
    match(TokenType::LParen);

    skipNewlines();
    while (!atEnd() && !check(TokenType::RParen)) {
        args.push_back(parseExpression());
        skipNewlines();
        if (!match(TokenType::Comma)) {
            break;
        }
        skipNewlines();
    }

    expect(TokenType::RParen, "E1341", "')' to close arguments");
    match(TokenType::RParen);
    return args;
}
