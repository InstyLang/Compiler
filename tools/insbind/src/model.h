// insbind: the C declaration model.
//
// The parser's output and the mapper's input. Types live in an arena and
// reference each other by index (cheap copies, trivial equality, and a direct
// port path to Insty slices). Declaration kinds are a variant, anticipating
// the Insty sum-type port one-to-one.
#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "token.h"

namespace insbind {

constexpr std::uint32_t kNoType = ~0u;
constexpr std::uint64_t kNoCount = ~0ull;

enum class CTypeKind : std::uint8_t {
    Void,
    Bool,
    Int,        // bits: 8/16/32/64; isSigned selects s*/u*
    Float,      // bits: 32/64 (long double: 80 SysV, 64 Win64 -- mapper rejects 80)
    Pointer,    // target -> pointee
    Array,      // target -> element; count (kNoCount = incomplete)
    Function,   // target -> return type; params/paramNames/variadic
    Record,     // target -> index into ParseResult::records
    Enum,       // target -> index into ParseResult::enums
    TypedefRef, // a use of a typedef name; name identifies it (mapper resolves)
};

struct CType {
    CTypeKind kind = CTypeKind::Void;
    std::uint32_t target = kNoType;
    std::uint8_t bits = 0;
    bool isSigned = false;
    std::uint64_t count = kNoCount;
    std::vector<std::uint32_t> params;
    std::vector<std::string> paramNames;
    bool variadic = false;
    bool pointeeIsConst = false; // Pointer only: is the pointee const-qualified
    std::string name; // TypedefRef; also the tag on Record/Enum for reference types
};

struct CField {
    std::string name;        // empty for anonymous members and padding bitfields
    std::uint32_t type = kNoType;
    bool bitfield = false;
    std::uint8_t bits = 0;
    bool anonymous = false;  // C11 anonymous struct/union member
    std::uint32_t alignAs = 0; // field-level _Alignas (0 = natural)
};

struct CRecordDef {
    std::string name;        // tag, or synthesized for anonymous definitions
    bool isUnion = false;
    bool incomplete = true;  // forward-declared, no definition seen
    std::uint32_t alignAs = 0; // from _Alignas, 0 = natural
    std::vector<CField> fields;
};

struct CEnumDef {
    std::string name;        // tag, or synthesized for anonymous definitions
    bool incomplete = true;
    std::vector<std::pair<std::string, std::int64_t>> values;
};

struct CTypedefDecl {
    std::string name;
    std::uint32_t type = kNoType;
};

struct CFunctionDecl {
    std::string name;
    std::uint32_t type = kNoType; // a Function type
    bool variadic = false;
    bool noReturn = false;
    bool staticInline = false;  // body skipped; not linkable, mapper warns
};

struct CGlobalDecl {
    std::string name;
    std::uint32_t type = kNoType;
    bool isExtern = false;
};

using CDecl = std::variant<CTypedefDecl, CRecordDef, CEnumDef, CFunctionDecl,
                           CGlobalDecl>;

struct ParseOptions {
    // The target C data model: how wide `long` and `long double` are.
    // LLP64 for x86_64_windows, LP64 for x86_64_linux.
    unsigned longBits = 32;
    unsigned longDoubleBits = 64;
    // The preprocessor's file table, for file-attributed diagnostics.
    const std::vector<std::string>* files = nullptr;
};

struct ParseResult {
    std::vector<CType> types;        // the arena
    std::vector<CRecordDef> records; // every record definition/forward ref
    std::vector<CEnumDef> enums;
    std::vector<CDecl> decls;        // top-level, in source order
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

ParseResult parse(const std::vector<Token>& tokens, const ParseOptions& options);

// Stable textual rendering for golden tests and debugging; diagnostics trail
// as `! <error>` / `!warn <warning>` lines.
std::string dumpModel(const ParseResult& result);

} // namespace insbind
