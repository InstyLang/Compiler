
#include <parser/parser.hpp>

#include <lexer/lexer.hpp>

namespace {
const Token& eofToken() {
    static const Token kEof = [] {
        Token t;
        t.type = TokenType::EndOfFile;
        return t;
    }();
    return kEof;
}
}

const Token& Parser::current() const {
    if (index_ >= tokens_.size()) {
        return eofToken();
    }
    return tokens_[index_];
}

const Token& Parser::peek(size_t ahead) const {
    size_t idx = index_ + ahead;
    if (idx >= tokens_.size()) {
        return eofToken();
    }
    return tokens_[idx];
}

const Token& Parser::previous() const {
    if (index_ == 0 || tokens_.empty()) {
        return eofToken();
    }
    size_t idx = index_ - 1;
    if (idx >= tokens_.size()) {
        return tokens_.back();
    }
    return tokens_[idx];
}

bool Parser::atEnd() const {
    return index_ >= tokens_.size() || current().type == TokenType::EndOfFile;
}

const Token& Parser::advance() {
    const Token& tok = current();
    if (!atEnd()) {
        ++index_;
    }
    return tok;
}

bool Parser::check(TokenType type) const {
    return current().type == type;
}

bool Parser::match(TokenType type) {
    if (check(type)) {
        advance();
        return true;
    }
    return false;
}

const Token& Parser::expect(TokenType type, const std::string& code,
                            const std::string& what) {
    if (check(type)) {
        return current();
    }
    error(code, "expected " + what, "");
    return current();
}

void Parser::skipNewlines() {
    while (check(TokenType::Newline)) {
        advance();
    }
}

void Parser::consumeStatementEnd() {
    match(TokenType::Semicolon);
    while (check(TokenType::Newline)) {
        advance();
    }
}

void Parser::synchronize() {
    panicMode_ = false;

    while (!atEnd()) {
        if (check(TokenType::Newline) || check(TokenType::Semicolon)) {
            advance();
            return;
        }
        if (check(TokenType::RBrace)) {
            return;
        }
        switch (current().type) {
            case TokenType::KwFun:
            case TokenType::KwStruct:
            case TokenType::KwClass:
            case TokenType::KwEnum:
            case TokenType::KwImport:
            case TokenType::KwSection:
            case TokenType::KwIf:
            case TokenType::KwWhile:
            case TokenType::KwLoop:
            case TokenType::KwWhen:
            case TokenType::KwSwitch:
            case TokenType::KwReturn:
            case TokenType::KwUnsafe:
                return;
            default:
                break;
        }
        advance();
    }
}


std::string Parser::parseTypeName() {
    std::string spelling;

    if (check(TokenType::KwVolatile)) {
        advance();
        spelling += "volatile ";
    }

    if (check(TokenType::Identifier) || isPrimitiveTypeName(current().value)) {
        spelling += current().value;
        advance();
    } else if (check(TokenType::KwThis)) {
        spelling += "this";
        advance();
    } else {
        error("E1101", "expected a type name", "e.g. i32, bool, or a struct name");
        return spelling;
    }

    while (check(TokenType::Dot) && peek().type == TokenType::Identifier) {
        advance();
        spelling += ".";
        spelling += current().value;
        advance();
    }

    if (check(TokenType::Lt)) {
        spelling += "<";
        advance();
        bool firstArg = true;
        while (!atEnd() && !check(TokenType::Gt)) {
            if (!firstArg) {
                if (!match(TokenType::Comma)) {
                    break;
                }
                spelling += ", ";
            }
            firstArg = false;
            spelling += parseTypeName();
        }
        expect(TokenType::Gt, "E1102", "'>' to close generic arguments");
        match(TokenType::Gt);
        spelling += ">";
    }

    while (check(TokenType::Star)) {
        advance();
        spelling += "*";
    }

    if (check(TokenType::LBracket)) {
        advance();
        spelling += "[";
        if (check(TokenType::IntegerLiteral)) {
            spelling += current().value;
            advance();
        }
        expect(TokenType::RBracket, "E1103", "']' to close array type");
        match(TokenType::RBracket);
        spelling += "]";
    }

    return spelling;
}

std::vector<std::string> Parser::parseGenericParams() {
    std::vector<std::string> params;
    if (!check(TokenType::Lt)) {
        return params;
    }
    advance();
    while (!atEnd() && !check(TokenType::Gt)) {
        if (check(TokenType::Identifier)) {
            params.push_back(current().value);
            advance();
        } else {
            break;
        }
        if (!match(TokenType::Comma)) {
            break;
        }
    }
    expect(TokenType::Gt, "E1104", "'>' to close generic parameters");
    match(TokenType::Gt);
    return params;
}

std::vector<AST::Parameter> Parser::parseParameterList() {
    std::vector<AST::Parameter> params;
    expect(TokenType::LParen, "E1105", "'(' to begin parameter list");
    match(TokenType::LParen);

    skipNewlines();
    while (!atEnd() && !check(TokenType::RParen)) {
        AST::Parameter p;
        if (check(TokenType::KwVolatile)) {
            p.isVolatile = true;
        }
        p.type = parseTypeName();
        if (check(TokenType::Identifier)) {
            p.name = current().value;
            advance();
        } else {
            error("E1106", "expected parameter name", "parameters are `Type name`");
        }
        params.push_back(std::move(p));

        skipNewlines();
        if (!match(TokenType::Comma)) {
            break;
        }
        skipNewlines();
    }

    expect(TokenType::RParen, "E1107", "')' to close parameter list");
    match(TokenType::RParen);
    return params;
}

std::vector<AST::Attribute> Parser::parseAttributes() {
    std::vector<AST::Attribute> attrs;
    if (!check(TokenType::LBracket)) {
        return attrs;
    }
    advance();

    skipNewlines();
    while (!atEnd() && !check(TokenType::RBracket)) {
        AST::Attribute attr;
        if (check(TokenType::Identifier) || current().type == TokenType::KwSection ||
            current().type == TokenType::KwUnsafe) {
            attr.name = current().value;
            advance();
        } else {
            error("E1108", "expected attribute name", "e.g. name(main), mangle(off)");
            break;
        }

        if (match(TokenType::LParen)) {
            if (!check(TokenType::RParen)) {
                attr.value = current().value;
                advance();
                while (!atEnd() && !check(TokenType::RParen)) {
                    attr.value += current().value;
                    advance();
                }
            }
            expect(TokenType::RParen, "E1109", "')' to close attribute value");
            match(TokenType::RParen);
        }

        attrs.push_back(std::move(attr));

        skipNewlines();
        if (!match(TokenType::Comma)) {
            break;
        }
        skipNewlines();
    }

    expect(TokenType::RBracket, "E1110", "']' to close attribute list");
    match(TokenType::RBracket);
    return attrs;
}
