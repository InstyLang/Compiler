// insbind: C token model.
//
// One TokenKind per keyword and punctuator (rather than a single
// Keyword/Punctuator kind carrying a spelling) so the parser can switch
// directly on kinds, and so golden-test token dumps read as plain text.
#pragma once

#include <cstdint>
#include <string>

namespace insbind {

enum class TokenKind : std::uint8_t {
    End,          // end of input
    Error,        // lex error; spelling carries the message
    Identifier,
    IntLiteral,   // raw pp-number, classified integer (value decoded later)
    FloatLiteral, // raw pp-number, classified floating
    CharLiteral,  // raw, quotes included in spelling
    StringLiteral,

    // C11 keywords (6.4.1).
    kw_auto, kw_break, kw_case, kw_char, kw_const, kw_continue, kw_default,
    kw_do, kw_double, kw_else, kw_enum, kw_extern, kw_float, kw_for, kw_goto,
    kw_if, kw_inline, kw_int, kw_long, kw_register, kw_restrict, kw_return,
    kw_short, kw_signed, kw_sizeof, kw_static, kw_struct, kw_switch,
    kw_typedef, kw_union, kw_unsigned, kw_void, kw_volatile, kw_while,
    kw__Alignas, kw__Alignof, kw__Atomic, kw__Bool, kw__Complex, kw__Generic,
    kw__Imaginary, kw__Noreturn, kw__Static_assert, kw__Thread_local,

    // Punctuators (6.4.6). Digraph spellings (<: :> <% %> %: %:%:) lex to the
    // same kinds as their primary spellings.
    LBracket, RBracket, LParen, RParen, LBrace, RBrace,
    Dot, Arrow, PlusPlus, MinusMinus, Amp, Star, Plus, Minus, Tilde, Bang,
    Slash, Percent, Shl, Shr, Lt, Gt, Le, Ge, EqEq, NotEq, Caret, Pipe,
    AmpAmp, PipePipe, Question, Colon, Semicolon, Ellipsis, Assign,
    StarAssign, SlashAssign, PercentAssign, PlusAssign, MinusAssign,
    ShlAssign, ShrAssign, AmpAssign, CaretAssign, PipeAssign,
    Comma, Hash, HashHash,
};

struct Token {
    TokenKind kind = TokenKind::End;
    std::string spelling;      // raw source text; Error carries the message
    std::uint32_t line = 0;    // 1-based
    std::uint32_t col = 0;     // 1-based, byte columns
    std::uint32_t file = 0;    // index into the preprocessor's file table (0 pre-preproc)
    bool atLineStart = false;  // first token on its line (the preprocessor needs this)
    bool leadingSpace = false; // preceded by whitespace or comments
};

const char* tokenKindName(TokenKind kind);

} // namespace insbind
