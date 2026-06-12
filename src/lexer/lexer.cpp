
#include <lexer/lexer.hpp>

#include <utilities/errors.hpp>

#include <cctype>

namespace {

bool isIdentStart(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

bool isIdentContinue(char c) {
    return isIdentStart(c) || (c >= '0' && c <= '9');
}

bool isDecimalDigit(char c) {
    return c >= '0' && c <= '9';
}

bool isHexDigit(char c) {
    return isDecimalDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

void reportInvalid(const std::string& code, const std::string& message,
                   int line, int column, int length, int offset,
                   const std::string& hint) {
    if (ErrorReporting::globalErrorReporter) {
        ErrorReporting::globalErrorReporter->error(
            code, message,
            ErrorReporting::SourceLocation{line, column, length, offset},
            hint);
    }
}

}

bool Lexer::atEnd() const {
    return pos_ >= src_->size();
}

char Lexer::peek(size_t ahead) const {
    size_t idx = pos_ + ahead;
    if (idx >= src_->size()) return '\0';
    return (*src_)[idx];
}

char Lexer::advance() {
    char c = (*src_)[pos_];
    ++pos_;
    ++column_;
    return c;
}

void Lexer::newline() {
    ++line_;
    column_ = 1;
}

Token Lexer::makeToken(TokenType type, std::string value, int startOffset,
                       int startLine, int startColumn) {
    Token t;
    t.type = type;
    t.value = std::move(value);
    t.start = startOffset;
    t.end = static_cast<int>(pos_);
    t.line = startLine;
    t.column = startColumn;
    return t;
}

std::vector<Token> Lexer::tokenize(const std::string& source) {
    src_ = &source;
    pos_ = 0;
    line_ = 1;
    column_ = 1;

    std::vector<Token> out;

    while (!atEnd()) {
        char c = peek();

        if (c == ' ' || c == '\t' || c == '\r') {
            advance();
            continue;
        }

        if (c == '\n') {
            int startOffset = static_cast<int>(pos_);
            int startLine = line_;
            int startColumn = column_;
            advance();
            newline();

            bool hasReal = false;
            for (const Token& t : out) {
                if (t.type != TokenType::Newline) {
                    hasReal = true;
                    break;
                }
            }
            bool prevNewline = !out.empty() && out.back().type == TokenType::Newline;
            if (hasReal && !prevNewline) {
                out.push_back(makeToken(TokenType::Newline, "\n", startOffset,
                                        startLine, startColumn));
            }
            continue;
        }

        if (c == '/' && peek(1) == '/') {
            while (!atEnd() && peek() != '\n') {
                advance();
            }
            continue;
        }

        if (isIdentStart(c)) {
            lexIdentifierOrKeyword(out);
            continue;
        }

        if (isDecimalDigit(c)) {
            lexNumber(out);
            continue;
        }

        if (c == '"') {
            lexString(out);
            continue;
        }

        if (c == '\'') {
            lexChar(out);
            continue;
        }

        lexOperator(out);
    }

    out.push_back(makeToken(TokenType::EndOfFile, "", static_cast<int>(pos_),
                            line_, column_));

    src_ = nullptr;
    return out;
}

void Lexer::lexIdentifierOrKeyword(std::vector<Token>& out) {
    int startOffset = static_cast<int>(pos_);
    int startLine = line_;
    int startColumn = column_;

    std::string text;
    while (!atEnd() && isIdentContinue(peek())) {
        text.push_back(advance());
    }

    TokenType type = keywordTokenType(text);
    out.push_back(makeToken(type, std::move(text), startOffset, startLine, startColumn));
}

void Lexer::lexNumber(std::vector<Token>& out) {
    int startOffset = static_cast<int>(pos_);
    int startLine = line_;
    int startColumn = column_;

    std::string text;

    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
        text.push_back(advance());
        text.push_back(advance());
        while (!atEnd() && (isHexDigit(peek()) || peek() == '_')) {
            text.push_back(advance());
        }
        out.push_back(makeToken(TokenType::IntegerLiteral, std::move(text),
                                startOffset, startLine, startColumn));
        return;
    }

    while (!atEnd() && (isDecimalDigit(peek()) || peek() == '_')) {
        text.push_back(advance());
    }

    if (peek() == '.' && isDecimalDigit(peek(1))) {
        text.push_back(advance());
        while (!atEnd() && (isDecimalDigit(peek()) || peek() == '_')) {
            text.push_back(advance());
        }
        out.push_back(makeToken(TokenType::FloatLiteral, std::move(text),
                                startOffset, startLine, startColumn));
        return;
    }

    out.push_back(makeToken(TokenType::IntegerLiteral, std::move(text),
                            startOffset, startLine, startColumn));
}

void Lexer::lexString(std::vector<Token>& out) {
    int startOffset = static_cast<int>(pos_);
    int startLine = line_;
    int startColumn = column_;

    advance();

    std::string decoded;
    bool terminated = false;

    while (!atEnd()) {
        char c = peek();

        if (c == '"') {
            advance();
            terminated = true;
            break;
        }

        if (c == '\n') {
            break;
        }

        if (c == '\\') {
            advance();
            if (atEnd()) break;
            char esc = peek();
            switch (esc) {
                case 'n': decoded.push_back('\n'); advance(); break;
                case 't': decoded.push_back('\t'); advance(); break;
                case 'r': decoded.push_back('\r'); advance(); break;
                case '\\': decoded.push_back('\\'); advance(); break;
                case '"': decoded.push_back('"'); advance(); break;
                case '0': decoded.push_back('\0'); advance(); break;
                case 'x': {
                    advance();
                    if (isHexDigit(peek()) && isHexDigit(peek(1))) {
                        int hi = hexValue(advance());
                        int lo = hexValue(advance());
                        decoded.push_back(static_cast<char>((hi << 4) | lo));
                    } else {
                        decoded.push_back('\\');
                        decoded.push_back('x');
                    }
                    break;
                }
                default:
                    decoded.push_back('\\');
                    decoded.push_back(esc);
                    advance();
                    break;
            }
            continue;
        }

        decoded.push_back(advance());
    }

    if (!terminated) {
        reportInvalid("E0002", "unterminated string literal", startLine,
                      startColumn, static_cast<int>(pos_) - startOffset,
                      startOffset, "add a closing '\"'");
    }

    out.push_back(makeToken(TokenType::StringLiteral, std::move(decoded),
                            startOffset, startLine, startColumn));
}

void Lexer::lexChar(std::vector<Token>& out) {
    int startOffset = static_cast<int>(pos_);
    int startLine = line_;
    int startColumn = column_;

    advance();

    std::string decoded;
    bool terminated = false;

    if (!atEnd() && peek() != '\'' && peek() != '\n') {
        char c = peek();
        if (c == '\\') {
            advance();
            if (!atEnd()) {
                char esc = peek();
                switch (esc) {
                    case 'n': decoded.push_back('\n'); advance(); break;
                    case 't': decoded.push_back('\t'); advance(); break;
                    case 'r': decoded.push_back('\r'); advance(); break;
                    case '\\': decoded.push_back('\\'); advance(); break;
                    case '\'': decoded.push_back('\''); advance(); break;
                    case '"': decoded.push_back('"'); advance(); break;
                    case '0': decoded.push_back('\0'); advance(); break;
                    case 'x': {
                        advance();
                        if (isHexDigit(peek()) && isHexDigit(peek(1))) {
                            int hi = hexValue(advance());
                            int lo = hexValue(advance());
                            decoded.push_back(static_cast<char>((hi << 4) | lo));
                        } else {
                            decoded.push_back('\\');
                            decoded.push_back('x');
                        }
                        break;
                    }
                    default:
                        decoded.push_back('\\');
                        decoded.push_back(esc);
                        advance();
                        break;
                }
            }
        } else {
            decoded.push_back(advance());
        }
    }

    if (!atEnd() && peek() == '\'') {
        advance();
        terminated = true;
    }

    if (!terminated) {
        reportInvalid("E0003", "unterminated character literal", startLine,
                      startColumn, static_cast<int>(pos_) - startOffset,
                      startOffset, "add a closing '\\''");
    }

    out.push_back(makeToken(TokenType::CharLiteral, std::move(decoded),
                            startOffset, startLine, startColumn));
}

void Lexer::lexOperator(std::vector<Token>& out) {
    int startOffset = static_cast<int>(pos_);
    int startLine = line_;
    int startColumn = column_;

    char c = peek();
    char n = peek(1);

    TokenType twoType = TokenType::Invalid;
    bool two = true;
    if (c == '-' && n == '>') twoType = TokenType::Arrow;
    else if (c == '=' && n == '=') twoType = TokenType::EqEq;
    else if (c == '!' && n == '=') twoType = TokenType::NotEq;
    else if (c == '<' && n == '=') twoType = TokenType::Le;
    else if (c == '>' && n == '=') twoType = TokenType::Ge;
    else if (c == '&' && n == '&') twoType = TokenType::AmpAmp;
    else if (c == '|' && n == '|') twoType = TokenType::PipePipe;
    else if (c == '<' && n == '<') twoType = TokenType::Shl;
    else if (c == '>' && n == '>') twoType = TokenType::Shr;
    else if (c == ':' && n == ':') twoType = TokenType::ColonColon;
    else two = false;

    if (two) {
        std::string text;
        text.push_back(advance());
        text.push_back(advance());
        out.push_back(makeToken(twoType, std::move(text), startOffset,
                                startLine, startColumn));
        return;
    }

    TokenType oneType = TokenType::Invalid;
    switch (c) {
        case '(': oneType = TokenType::LParen; break;
        case ')': oneType = TokenType::RParen; break;
        case '{': oneType = TokenType::LBrace; break;
        case '}': oneType = TokenType::RBrace; break;
        case '[': oneType = TokenType::LBracket; break;
        case ']': oneType = TokenType::RBracket; break;
        case ',': oneType = TokenType::Comma; break;
        case '.': oneType = TokenType::Dot; break;
        case ':': oneType = TokenType::Colon; break;
        case ';': oneType = TokenType::Semicolon; break;
        case '=': oneType = TokenType::Assign; break;
        case '+': oneType = TokenType::Plus; break;
        case '-': oneType = TokenType::Minus; break;
        case '*': oneType = TokenType::Star; break;
        case '/': oneType = TokenType::Slash; break;
        case '%': oneType = TokenType::Percent; break;
        case '&': oneType = TokenType::Amp; break;
        case '|': oneType = TokenType::Pipe; break;
        case '^': oneType = TokenType::Caret; break;
        case '~': oneType = TokenType::Tilde; break;
        case '!': oneType = TokenType::Bang; break;
        case '<': oneType = TokenType::Lt; break;
        case '>': oneType = TokenType::Gt; break;
        case '@': oneType = TokenType::At; break;
        case '#': oneType = TokenType::Hash; break;
        case '$': oneType = TokenType::Dollar; break;
        default: oneType = TokenType::Invalid; break;
    }

    if (oneType == TokenType::Invalid) {
        std::string text(1, advance());
        std::string msg = "unexpected character '";
        msg += text;
        msg += "'";
        reportInvalid("E0001", msg, startLine, startColumn, 1, startOffset, "");
        out.push_back(makeToken(TokenType::Invalid, std::move(text), startOffset,
                                startLine, startColumn));
        return;
    }

    std::string text(1, advance());
    out.push_back(makeToken(oneType, std::move(text), startOffset, startLine,
                            startColumn));
}
