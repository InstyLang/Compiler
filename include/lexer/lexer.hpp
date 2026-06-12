#pragma once


#include <cstddef>
#include <string>
#include <vector>

enum class TokenType {
    Identifier,
    IntegerLiteral,
    FloatLiteral,
    StringLiteral,
    CharLiteral,

    KwModule,
    KwImport,
    KwAs,
    KwFun,
    KwStruct,
    KwClass,
    KwEnum,
    KwConstructor,
    KwDestructor,
    KwOperator,
    KwConst,
    KwLet,
    KwIf,
    KwElse,
    KwWhile,
    KwFor,
    KwLoop,
    KwWhen,
    KwSwitch,
    KwReturn,
    KwBreak,
    KwSkip,
    KwNew,
    KwDelete,
    KwCast,
    KwUnsafe,
    KwVolatile,
    KwSection,
    KwThis,
    KwTrue,
    KwFalse,

    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket,
    Comma,
    Dot,
    Colon,
    ColonColon,
    Semicolon,
    Arrow,
    Assign,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Amp,
    Pipe,
    Caret,
    Tilde,
    Bang,
    Lt,
    Gt,
    Le,
    Ge,
    EqEq,
    NotEq,
    AmpAmp,
    PipePipe,
    Shl,
    Shr,
    At,
    Hash,
    Dollar,

    Newline,
    EndOfFile,
    Invalid
};

struct Token {
    TokenType type = TokenType::Invalid;
    std::string value;
    int start = 0;
    int end = 0;
    int line = 1;
    int column = 1;
};

std::string tokenTypeName(TokenType type);
std::string stringifyToken(const Token& token);

class Lexer {
public:
    Lexer() = default;

    std::vector<Token> tokenize(const std::string& source);

private:
    const std::string* src_ = nullptr;
    size_t pos_ = 0;
    int line_ = 1;
    int column_ = 1;

    bool atEnd() const;
    char peek(size_t ahead = 0) const;
    char advance();
    void newline();

    Token makeToken(TokenType type, std::string value, int startOffset, int startLine, int startColumn);

    void lexNumber(std::vector<Token>& out);
    void lexIdentifierOrKeyword(std::vector<Token>& out);
    void lexString(std::vector<Token>& out);
    void lexChar(std::vector<Token>& out);
    void lexOperator(std::vector<Token>& out);
};

TokenType keywordTokenType(const std::string& word);
bool isPrimitiveTypeName(const std::string& word);
