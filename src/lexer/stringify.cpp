
#include <lexer/lexer.hpp>

#include <string>
#include <unordered_map>
#include <unordered_set>

std::string tokenTypeName(TokenType type) {
    switch (type) {
        case TokenType::Identifier:      return "Identifier";
        case TokenType::IntegerLiteral:  return "IntegerLiteral";
        case TokenType::FloatLiteral:    return "FloatLiteral";
        case TokenType::StringLiteral:   return "StringLiteral";
        case TokenType::CharLiteral:     return "CharLiteral";

        case TokenType::KwModule:        return "KwModule";
        case TokenType::KwImport:        return "KwImport";
        case TokenType::KwAs:            return "KwAs";
        case TokenType::KwFun:           return "KwFun";
        case TokenType::KwExtern:        return "KwExtern";
        case TokenType::KwStruct:        return "KwStruct";
        case TokenType::KwClass:         return "KwClass";
        case TokenType::KwEnum:          return "KwEnum";
        case TokenType::KwConstructor:   return "KwConstructor";
        case TokenType::KwDestructor:    return "KwDestructor";
        case TokenType::KwOperator:      return "KwOperator";
        case TokenType::KwConst:         return "KwConst";
        case TokenType::KwLet:           return "KwLet";
        case TokenType::KwIf:            return "KwIf";
        case TokenType::KwElse:          return "KwElse";
        case TokenType::KwWhile:         return "KwWhile";
        case TokenType::KwFor:           return "KwFor";
        case TokenType::KwLoop:          return "KwLoop";
        case TokenType::KwWhen:          return "KwWhen";
        case TokenType::KwSwitch:        return "KwSwitch";
        case TokenType::KwReturn:        return "KwReturn";
        case TokenType::KwBreak:         return "KwBreak";
        case TokenType::KwSkip:          return "KwSkip";
        case TokenType::KwNew:           return "KwNew";
        case TokenType::KwDelete:        return "KwDelete";
        case TokenType::KwCast:          return "KwCast";
        case TokenType::KwUnsafe:        return "KwUnsafe";
        case TokenType::KwVolatile:      return "KwVolatile";
        case TokenType::KwSection:       return "KwSection";
        case TokenType::KwThis:          return "KwThis";
        case TokenType::KwTrue:          return "KwTrue";
        case TokenType::KwFalse:         return "KwFalse";

        case TokenType::LParen:          return "LParen";
        case TokenType::RParen:          return "RParen";
        case TokenType::LBrace:          return "LBrace";
        case TokenType::RBrace:          return "RBrace";
        case TokenType::LBracket:        return "LBracket";
        case TokenType::RBracket:        return "RBracket";
        case TokenType::Comma:           return "Comma";
        case TokenType::Dot:             return "Dot";
        case TokenType::Colon:           return "Colon";
        case TokenType::ColonColon:      return "ColonColon";
        case TokenType::Semicolon:       return "Semicolon";
        case TokenType::Arrow:           return "Arrow";
        case TokenType::Assign:          return "Assign";
        case TokenType::Plus:            return "Plus";
        case TokenType::Minus:           return "Minus";
        case TokenType::Star:            return "Star";
        case TokenType::Slash:           return "Slash";
        case TokenType::Percent:         return "Percent";
        case TokenType::Amp:             return "Amp";
        case TokenType::Pipe:            return "Pipe";
        case TokenType::Caret:           return "Caret";
        case TokenType::Tilde:           return "Tilde";
        case TokenType::Bang:            return "Bang";
        case TokenType::Lt:              return "Lt";
        case TokenType::Gt:              return "Gt";
        case TokenType::Le:              return "Le";
        case TokenType::Ge:              return "Ge";
        case TokenType::EqEq:            return "EqEq";
        case TokenType::NotEq:           return "NotEq";
        case TokenType::AmpAmp:          return "AmpAmp";
        case TokenType::PipePipe:        return "PipePipe";
        case TokenType::Shl:             return "Shl";
        case TokenType::Shr:             return "Shr";
        case TokenType::At:              return "At";
        case TokenType::Hash:            return "Hash";
        case TokenType::Dollar:          return "Dollar";

        case TokenType::Newline:         return "Newline";
        case TokenType::EndOfFile:       return "EndOfFile";
        case TokenType::Invalid:         return "Invalid";
    }
    return "Unknown";
}

std::string stringifyToken(const Token& token) {
    std::string display;
    if (token.type == TokenType::Newline) {
        display = "\\n";
    } else {
        for (char c : token.value) {
            switch (c) {
                case '\n': display += "\\n"; break;
                case '\t': display += "\\t"; break;
                case '\r': display += "\\r"; break;
                case '\0': display += "\\0"; break;
                default:   display.push_back(c); break;
            }
        }
    }

    std::string out = tokenTypeName(token.type);
    out += "('";
    out += display;
    out += "' ";
    out += std::to_string(token.line);
    out += ":";
    out += std::to_string(token.column);
    out += " [";
    out += std::to_string(token.start);
    out += "..";
    out += std::to_string(token.end);
    out += "))";
    return out;
}

TokenType keywordTokenType(const std::string& word) {
    static const std::unordered_map<std::string, TokenType> kKeywords = {
        {"module",      TokenType::KwModule},
        {"import",      TokenType::KwImport},
        {"as",          TokenType::KwAs},
        {"fun",         TokenType::KwFun},
        {"extern",      TokenType::KwExtern},
        {"struct",      TokenType::KwStruct},
        {"class",       TokenType::KwClass},
        {"enum",        TokenType::KwEnum},
        {"constructor", TokenType::KwConstructor},
        {"destructor",  TokenType::KwDestructor},
        {"operator",    TokenType::KwOperator},
        {"const",       TokenType::KwConst},
        {"let",         TokenType::KwLet},
        {"if",          TokenType::KwIf},
        {"else",        TokenType::KwElse},
        {"while",       TokenType::KwWhile},
        {"for",         TokenType::KwFor},
        {"loop",        TokenType::KwLoop},
        {"when",        TokenType::KwWhen},
        {"switch",      TokenType::KwSwitch},
        {"return",      TokenType::KwReturn},
        {"break",       TokenType::KwBreak},
        {"skip",        TokenType::KwSkip},
        {"new",         TokenType::KwNew},
        {"delete",      TokenType::KwDelete},
        {"cast",        TokenType::KwCast},
        {"unsafe",      TokenType::KwUnsafe},
        {"volatile",    TokenType::KwVolatile},
        {"section",     TokenType::KwSection},
        {"this",        TokenType::KwThis},
        {"true",        TokenType::KwTrue},
        {"false",       TokenType::KwFalse},
    };

    auto it = kKeywords.find(word);
    if (it != kKeywords.end()) {
        return it->second;
    }
    return TokenType::Identifier;
}

bool isPrimitiveTypeName(const std::string& word) {
    static const std::unordered_set<std::string> kPrimitives = {
        "void", "bool", "text",
        "i8", "i16", "i32", "i64", "i128",
        "u8", "u16", "u32", "u64", "u128",
        "f16", "f32", "f64", "f128",
    };
    return kPrimitives.find(word) != kPrimitives.end();
}
