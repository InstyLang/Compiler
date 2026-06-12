
#include <parser/parser.hpp>

#include <string>
#include <vector>

std::string ecxParseDottedName(Parser& parser) {
    if (!parser.check(TokenType::Identifier)) {
        return std::string();
    }
    std::string name = parser.current().value;
    parser.advance();
    while (parser.check(TokenType::Dot) &&
           parser.peek().type == TokenType::Identifier) {
        parser.advance();
        name += ".";
        name += parser.current().value;
        parser.advance();
    }
    return name;
}

std::string ecxParseScopedModulePath(Parser& parser) {
    if (!parser.check(TokenType::Identifier)) {
        return std::string();
    }
    std::string name = parser.current().value;
    parser.advance();
    while (parser.check(TokenType::ColonColon) &&
           parser.peek().type == TokenType::Identifier) {
        parser.advance();
        name += "::";
        name += parser.current().value;
        parser.advance();
    }
    return name;
}

std::string ecxParseNameSegment(Parser& parser) {
    if (!parser.check(TokenType::Identifier)) {
        return std::string();
    }
    std::string name = parser.current().value;
    parser.advance();
    return name;
}
