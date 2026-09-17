#include "parse.h"

#include <unordered_map>

#include "constexpr.h"

namespace insbind {
namespace {

constexpr std::uint32_t kPlaceholder = ~0u - 1; // nested-declarator marker type

class Parser {
public:
    Parser(const std::vector<Token>& tokens, const ParseOptions& options)
        : tokens_(tokens), opts_(options) {}

    ParseResult run() {
        while (!atEnd()) {
            externalDecl();
        }
        return std::move(result_);
    }

private:
    const std::vector<Token>& tokens_;
    ParseOptions opts_;
    std::size_t pos_ = 0;
    ParseResult result_;
    std::unordered_map<std::string, std::uint32_t> typedefs_;
    std::unordered_map<std::string, std::uint32_t> recordTags_;
    std::unordered_map<std::string, std::uint32_t> enumTags_;
    std::unordered_map<std::string, std::int64_t> enumValues_;
    unsigned anonCounter_ = 0;

    // ------------------------------------------------------------ utilities

    bool atEnd() const {
        return pos_ >= tokens_.size() || tokens_[pos_].kind == TokenKind::End;
    }
    const Token& peek(std::size_t off = 0) const {
        static const Token end;
        return pos_ + off < tokens_.size() ? tokens_[pos_ + off] : end;
    }
    bool at(TokenKind k) const { return peek().kind == k; }
    bool eat(TokenKind k) {
        if (at(k)) { ++pos_; return true; }
        return false;
    }
    bool expect(TokenKind k, const char* what) {
        if (eat(k)) return true;
        error(std::string("expected ") + what + ", found '" + peek().spelling +
              "'");
        return false;
    }
    void error(const std::string& msg) {
        result_.errors.push_back(std::to_string(peek().line) + ": " + msg);
    }
    void warn(const std::string& msg) {
        result_.warnings.push_back(std::to_string(peek().line) + ": " + msg);
    }

    std::uint32_t addType(CType t) {
        result_.types.push_back(std::move(t));
        return static_cast<std::uint32_t>(result_.types.size() - 1);
    }
    std::uint32_t intType(unsigned bits, bool isSigned) {
        CType t;
        t.kind = CTypeKind::Int;
        t.bits = static_cast<std::uint8_t>(bits);
        t.isSigned = isSigned;
        return addType(t);
    }
    std::uint32_t floatType(unsigned bits) {
        CType t;
        t.kind = CTypeKind::Float;
        t.bits = static_cast<std::uint8_t>(bits);
        return addType(t);
    }
    std::uint32_t simpleType(CTypeKind kind) {
        CType t;
        t.kind = kind;
        return addType(t);
    }
    std::uint32_t pointerTo(std::uint32_t target) {
        CType t;
        t.kind = CTypeKind::Pointer;
        t.target = target;
        return addType(t);
    }
    std::uint32_t typedefRef(const std::string& name) {
        CType t;
        t.kind = CTypeKind::TypedefRef;
        t.name = name;
        const auto it = typedefs_.find(name);
        if (it != typedefs_.end()) t.target = it->second;
        return addType(t);
    }

    bool isTypeName(const std::string& s) const {
        return typedefs_.find(s) != typedefs_.end();
    }

    // -------------------------------------------------- attribute skipping

    // Skips one GNU/MSVC attribute-ish construct if present. Returns true if
    // something was consumed. `inline`-like words feed the out flag.
    bool skipAttribute(bool& sawInline) {
        const std::string& s = peek().spelling;
        if (peek().kind != TokenKind::Identifier) return false;
        if (s == "__attribute__" || s == "__declspec" || s == "__asm" ||
            s == "__asm__") {
            ++pos_;
            if (at(TokenKind::LParen)) skipBalancedParens();
            return true;
        }
        if (s == "__cdecl" || s == "__stdcall" || s == "__fastcall" ||
            s == "__thiscall" || s == "__vectorcall" || s == "_cdecl" ||
            s == "_stdcall" || s == "__extension__") {
            ++pos_; // Win64 has one convention; nothing to record
            return true;
        }
        if (s == "__inline" || s == "__forceinline") {
            sawInline = true;
            ++pos_;
            return true;
        }
        return false;
    }

    void skipBalancedParens() {
        int depth = 0;
        do {
            if (at(TokenKind::LParen)) ++depth;
            else if (at(TokenKind::RParen)) --depth;
            else if (atEnd()) return;
            ++pos_;
        } while (depth > 0);
    }

    void skipBalancedBraces() {
        int depth = 0;
        do {
            if (at(TokenKind::LBrace)) ++depth;
            else if (at(TokenKind::RBrace)) --depth;
            else if (atEnd()) return;
            ++pos_;
        } while (depth > 0);
    }

    // ------------------------------------------------------------- specifiers

    struct Specifiers {
        std::uint32_t base = kNoType;   // arena index of the base type
        bool isTypedef = false;
        bool isExtern = false;
        bool isStatic = false;
        bool isInline = false;
        bool noReturn = false;
        bool isConst = false;           // base carries a const qualifier
        std::uint32_t alignAs = 0;
        bool ok = false;
    };

    // declarationSpecifiers: storage class + type specifiers + qualifiers +
    // function specifiers + _Alignas, in any order, at least one type-ish word.
    enum class SpecBase { None, Void, Bool, Char, Short, Int, Float, Double, Given };

    // Assembles the keyword-combination base type at a specifier-list return
    // point (sp.base is already set for struct/union/enum/typedef bases).
    // `unsigned`/`signed`/`long` alone all imply int.
    Specifiers finishSpecifiers(Specifiers sp, SpecBase base,
                                unsigned longCount, bool unsignedSeen,
                                bool signedSeen) {
        const bool impliesInt = unsignedSeen || signedSeen || longCount > 0;
        if (sp.base == kNoType) {
            switch (base) {
                case SpecBase::None:
                    if (impliesInt) {
                        unsigned bits = 32;
                        if (longCount >= 2) bits = 64;
                        else if (longCount == 1) bits = opts_.longBits;
                        sp.base = intType(bits, !unsignedSeen);
                    }
                    break;
                case SpecBase::Void: sp.base = simpleType(CTypeKind::Void); break;
                case SpecBase::Bool: sp.base = simpleType(CTypeKind::Bool); break;
                case SpecBase::Char: sp.base = intType(8, !unsignedSeen); break;
                case SpecBase::Short: sp.base = intType(16, !unsignedSeen); break;
                case SpecBase::Int: {
                    unsigned bits = 32;
                    if (longCount >= 2) bits = 64;
                    else if (longCount == 1) bits = opts_.longBits;
                    sp.base = intType(bits, !unsignedSeen);
                    break;
                }
                case SpecBase::Float: sp.base = floatType(32); break;
                case SpecBase::Double:
                    sp.base = floatType(longCount >= 1 ? opts_.longDoubleBits
                                                       : 64);
                    break;
                case SpecBase::Given: break; // unreachable
            }
        }
        sp.ok = base != SpecBase::None || impliesInt;
        return sp;
    }

    Specifiers parseSpecifiers() {
        Specifiers sp;
        SpecBase base = SpecBase::None;
        unsigned longCount = 0;
        bool unsignedSeen = false, signedSeen = false;

        for (;;) {
            const Token& t = peek();
            bool sawInlineTmp = false;
            if (skipAttribute(sawInlineTmp)) {
                sp.isInline = sp.isInline || sawInlineTmp;
                continue;
            }
            switch (t.kind) {
                case TokenKind::kw_typedef: sp.isTypedef = true; break;
                case TokenKind::kw_extern: {
                    // extern "C" is C++ linkage -- out of scope.
                    if (peek(1).kind == TokenKind::StringLiteral) {
                        error("C++ linkage (extern \"C\") is not supported");
                        ++pos_;
                        skipBalancedBraces();
                        sp.ok = false;
                        return sp;
                    }
                    sp.isExtern = true;
                    break;
                }
                case TokenKind::kw_static: sp.isStatic = true; break;
                case TokenKind::kw_inline: sp.isInline = true; break;
                case TokenKind::kw__Noreturn: sp.noReturn = true; break;
                case TokenKind::kw_register: case TokenKind::kw_auto:
                    break; // meaningless for bindings
                case TokenKind::kw_const:
                    sp.isConst = true;
                    break; // the FFI has no const model (kept only for text/u8*)
                case TokenKind::kw_volatile:
                case TokenKind::kw_restrict:
                    break; // the FFI has no const model
                case TokenKind::kw__Atomic:
                    warn("_Atomic dropped (no Insty atomic type mapping yet)");
                    break;
                case TokenKind::kw__Thread_local:
                    warn("_Thread_local dropped");
                    break;
                case TokenKind::kw__Alignas: {
                    ++pos_;
                    if (expect(TokenKind::LParen, "'('")) {
                        std::int64_t v = 0;
                        std::string err;
                        std::size_t save = pos_;
                        if (evalConstExpr(tokens_, &pos_, &v, &err, &enumValues_)) {
                            sp.alignAs = static_cast<std::uint32_t>(v);
                            expect(TokenKind::RParen, "')'");
                        } else {
                            pos_ = save;
                            warn("unsupported _Alignas operand (type form?)");
                            skipBalancedParens();
                        }
                    }
                    continue; // fully consumed; do not ++pos_ past the next token
                }
                case TokenKind::kw_void: base = SpecBase::Void; break;
                case TokenKind::kw_char: base = SpecBase::Char; break;
                case TokenKind::kw_short:
                    if (base == SpecBase::None) base = SpecBase::Short;
                    break;
                case TokenKind::kw_int:
                    if (base == SpecBase::None) base = SpecBase::Int;
                    break;
                case TokenKind::kw_long:
                    ++longCount;
                    if (base == SpecBase::None) base = SpecBase::Int;
                    break;
                case TokenKind::kw_float: base = SpecBase::Float; break;
                case TokenKind::kw_double:
                    if (base == SpecBase::None) base = SpecBase::Double;
                    break;
                case TokenKind::kw_signed: signedSeen = true; break;
                case TokenKind::kw_unsigned: unsignedSeen = true; break;
                case TokenKind::kw__Bool: base = SpecBase::Bool; break;
                case TokenKind::kw_struct: case TokenKind::kw_union: {
                    const bool isUnion = t.kind == TokenKind::kw_union;
                    ++pos_;
                    sp.base = recordSpecifier(isUnion);
                    base = SpecBase::Given;
                    continue; // recordSpecifier consumed the whole specifier
                }
                case TokenKind::kw_enum: {
                    ++pos_;
                    sp.base = enumSpecifier();
                    base = SpecBase::Given;
                    continue; // enumSpecifier consumed the whole specifier
                }
                case TokenKind::Identifier:
                    // MSVC sized-integer keywords (__int8..__int64) are type
                    // specifiers, ubiquitous in Windows-world headers.
                    if (base == SpecBase::None &&
                        (t.spelling == "__int8" || t.spelling == "__int16" ||
                         t.spelling == "__int32" || t.spelling == "__int64")) {
                        const unsigned bits =
                            t.spelling == "__int8"    ? 8
                            : t.spelling == "__int16" ? 16
                            : t.spelling == "__int32" ? 32
                                                      : 64;
                        sp.base = intType(bits, !unsignedSeen);
                        base = SpecBase::Given;
                        break; // ++pos_ below consumes it
                    }
                    if (base == SpecBase::None && !signedSeen && !unsignedSeen &&
                        longCount == 0 && isTypeName(t.spelling)) {
                        sp.base = typedefRef(t.spelling);
                        base = SpecBase::Given;
                        break; // ++pos_ below consumes the typedef name
                    }
                    // Not a type name: specifiers are done.
                    return finishSpecifiers(sp, base, longCount, unsignedSeen,
                                            signedSeen);
                default:
                    return finishSpecifiers(sp, base, longCount, unsignedSeen,
                                            signedSeen);
            }
            ++pos_;
        }
    }

    // ----------------------------------------------------- struct/union/enum

    // After 'struct'/'union'. Handles definition, reference, forward decl.
    std::uint32_t recordSpecifier(bool isUnion) {
        std::string tag;
        if (at(TokenKind::Identifier)) tag = peek().spelling, ++pos_;
        bool dummyInline = false;
        while (skipAttribute(dummyInline)) {}

        std::uint32_t defIdx = kNoType;
        if (!tag.empty()) {
            const auto it = recordTags_.find(tag);
            if (it != recordTags_.end()) defIdx = it->second;
        }

        if (at(TokenKind::LBrace)) { // definition
            ++pos_;
            if (defIdx == kNoType) {
                defIdx = static_cast<std::uint32_t>(result_.records.size());
                result_.records.push_back(CRecordDef{});
                result_.records[defIdx].isUnion = isUnion;
                result_.records[defIdx].name =
                    tag.empty() ? synthesizeAnon() : tag;
                if (!tag.empty()) recordTags_[tag] = defIdx;
            } else if (!result_.records[defIdx].incomplete) {
                error("redefinition of record '" + tag + "'");
            }
            // Parse into a LOCAL body: nested record definitions grow
            // result_.records, which would dangle a reference held across
            // parseFieldList. Acquiring the entry again afterwards is safe.
            CRecordDef body;
            body.isUnion = isUnion;
            body.name = result_.records[defIdx].name;
            parseFieldList(body);
            CRecordDef& def = result_.records[defIdx];
            def = std::move(body);
            def.incomplete = false;
            result_.decls.push_back(def);
        } else if (defIdx == kNoType) {
            // Forward reference: create an incomplete entry.
            defIdx = static_cast<std::uint32_t>(result_.records.size());
            result_.records.push_back(CRecordDef{});
            result_.records[defIdx].isUnion = isUnion;
            result_.records[defIdx].name = tag;
            recordTags_[tag] = defIdx;
        }

        CType t;
        t.kind = CTypeKind::Record;
        t.target = defIdx;
        t.name = result_.records[defIdx].name;
        return addType(t);
    }

    void parseFieldList(CRecordDef& def) {
        while (!at(TokenKind::RBrace) && !atEnd()) {
            if (eat(TokenKind::Semicolon)) continue; // stray ';'
            Specifiers sp = parseSpecifiers();
            if (!sp.ok) {
                error("malformed field");
                synchronize();
                continue;
            }
            if (sp.alignAs > def.alignAs) def.alignAs = sp.alignAs;
            for (;;) {
                CField field;
                if (at(TokenKind::Semicolon)) {
                    // C11 anonymous member: a record type standing alone.
                    field.type = sp.base;
                    field.anonymous = true;
                    def.fields.push_back(std::move(field));
                    ++pos_; // ';'
                    break;
                }
                if (at(TokenKind::Colon)) {
                    field.type = sp.base; // unnamed bitfield (padding)
                } else {
                    std::string name;
                    field.type = parseDeclarator(sp.base, name, sp.isConst);
                    field.name = std::move(name);
                }
                field.alignAs = sp.alignAs;
                if (eat(TokenKind::Colon)) {
                    std::int64_t width = 0;
                    std::string err;
                    if (evalConstExpr(tokens_, &pos_, &width, &err, &enumValues_)) {
                        field.bitfield = true;
                        field.bits = static_cast<std::uint8_t>(width);
                    } else {
                        error("bad bitfield width: " + err);
                    }
                }
                def.fields.push_back(std::move(field));
                if (eat(TokenKind::Comma)) continue;
                expect(TokenKind::Semicolon, "';'");
                break;
            }
        }
        expect(TokenKind::RBrace, "'}'");
    }

    // After 'enum'.
    std::uint32_t enumSpecifier() {
        std::string tag;
        if (at(TokenKind::Identifier)) tag = peek().spelling, ++pos_;

        std::uint32_t defIdx = kNoType;
        if (!tag.empty()) {
            const auto it = enumTags_.find(tag);
            if (it != enumTags_.end()) defIdx = it->second;
        }

        if (at(TokenKind::LBrace)) {
            ++pos_;
            if (defIdx == kNoType) {
                defIdx = static_cast<std::uint32_t>(result_.enums.size());
                result_.enums.push_back(CEnumDef{});
                result_.enums[defIdx].name =
                    tag.empty() ? synthesizeAnon() : tag;
                if (!tag.empty()) enumTags_[tag] = defIdx;
            }
            CEnumDef& def = result_.enums[defIdx];
            def.incomplete = false;

            std::int64_t nextValue = 0;
            while (!at(TokenKind::RBrace) && !atEnd()) {
                if (!at(TokenKind::Identifier)) {
                    error("expected enumerator name, found '" +
                          peek().spelling + "'");
                    break;
                }
                const std::string valueName = peek().spelling;
                ++pos_;
                if (eat(TokenKind::Assign)) {
                    std::string err;
                    if (!evalConstExpr(tokens_, &pos_, &nextValue, &err, &enumValues_))
                        error("bad enumerator value: " + err);
                }
                def.values.emplace_back(valueName, nextValue);
                enumValues_[valueName] = nextValue;
                ++nextValue;
                eat(TokenKind::Comma); // trailing comma is fine
            }
            expect(TokenKind::RBrace, "'}'");
            result_.decls.push_back(def);
        } else if (defIdx == kNoType) {
            defIdx = static_cast<std::uint32_t>(result_.enums.size());
            result_.enums.push_back(CEnumDef{});
            result_.enums[defIdx].name = tag;
            enumTags_[tag] = defIdx;
        }

        CType t;
        t.kind = CTypeKind::Enum;
        t.target = defIdx;
        t.name = result_.enums[defIdx].name;
        return addType(t);
    }

    std::string synthesizeAnon() {
        return "_anon_" + std::to_string(anonCounter_++);
    }

    // -------------------------------------------------------------- declarator

    // declarator: pointer* direct-declarator. The nested-parentheses case is
    // handled with a placeholder type: the inner declarator is parsed with a
    // marker, the outer suffixes are built, then the marker is replaced --
    // this is what makes `int (*f)(void)` a pointer-to-function rather than a
    // function-returning-pointer. `baseIsConst` is the base type's qualifier
    // (it decides pointeeIsConst on the innermost pointer).
    std::uint32_t parseDeclarator(std::uint32_t base, std::string& name,
                                  bool baseIsConst = false) {
        bool firstWrap = true;
        while (at(TokenKind::Star)) {
            ++pos_;
            bool starConst = false;
            while (at(TokenKind::kw_const) || at(TokenKind::kw_volatile) ||
                   at(TokenKind::kw_restrict) || at(TokenKind::kw__Atomic)) {
                starConst = starConst || at(TokenKind::kw_const);
                ++pos_;
            }
            const bool pointeeConst = firstWrap ? baseIsConst : starConst;
            firstWrap = false;
            const std::uint32_t ptr = pointerTo(base);
            result_.types[ptr].pointeeIsConst = pointeeConst;
            base = ptr;
        }

        if (at(TokenKind::LParen) && startsDeclarator(peek(1))) {
            ++pos_;
            const std::uint32_t inner =
                parseDeclarator(kPlaceholder, name, baseIsConst);
            expect(TokenKind::RParen, "')'");
            std::uint32_t ty = parseTypeSuffix(base);
            return replacePlaceholder(inner, ty);
        }

        if (at(TokenKind::Identifier)) {
            name = peek().spelling;
            ++pos_;
        }
        return parseTypeSuffix(base);
    }

    bool startsDeclarator(const Token& t) const {
        if (t.kind == TokenKind::Star || t.kind == TokenKind::LParen)
            return true;
        return t.kind == TokenKind::Identifier && !isTypeName(t.spelling);
    }

    std::uint32_t replacePlaceholder(std::uint32_t idx, std::uint32_t with) {
        if (idx == kPlaceholder) return with;
        CType& t = result_.types[idx];
        if (t.kind == CTypeKind::Pointer || t.kind == CTypeKind::Array ||
            t.kind == CTypeKind::Function) {
            t.target = replacePlaceholder(t.target, with);
        }
        return idx;
    }

    std::uint32_t parseTypeSuffix(std::uint32_t base) {
        for (;;) {
            if (eat(TokenKind::LBracket)) {
                std::uint64_t count = kNoCount;
                if (!at(TokenKind::RBracket)) {
                    std::int64_t v = 0;
                    std::string err;
                    if (evalConstExpr(tokens_, &pos_, &v, &err, &enumValues_)) {
                        count = static_cast<std::uint64_t>(v);
                    } else {
                        error("bad array bound: " + err);
                    }
                }
                expect(TokenKind::RBracket, "']'");
                CType t;
                t.kind = CTypeKind::Array;
                t.target = base;
                t.count = count;
                base = addType(t);
            } else if (eat(TokenKind::LParen)) {
                CType t;
                t.kind = CTypeKind::Function;
                t.target = base;
                parseParams(t);
                base = addType(t);
            } else {
                return base;
            }
        }
    }

    void parseParams(CType& fn) {
        if (eat(TokenKind::RParen)) return;
        // (void) -- no parameters.
        if (at(TokenKind::kw_void) && peek(1).kind == TokenKind::RParen) {
            pos_ += 2;
            return;
        }
        for (;;) {
            if (eat(TokenKind::Ellipsis)) {
                fn.variadic = true;
                expect(TokenKind::RParen, "')'");
                return;
            }
            Specifiers sp = parseSpecifiers();
            // Declaration-level keywords are malformed parameters, not
            // missing types: report and resync to ',' or ')' so a header
            // with one bad parameter does not flood diagnostics.
            if (!sp.ok || at(TokenKind::kw_typedef) || at(TokenKind::kw_extern) ||
                at(TokenKind::kw_static) || at(TokenKind::kw_inline) ||
                at(TokenKind::kw_struct) || at(TokenKind::kw_union) ||
                at(TokenKind::kw_enum)) {
                error("malformed parameter");
                int depth = 0;
                while (!atEnd() &&
                       !(depth == 0 && (at(TokenKind::Comma) ||
                                        at(TokenKind::RParen)))) {
                    if (at(TokenKind::LParen) || at(TokenKind::LBracket))
                        ++depth;
                    else if (at(TokenKind::RParen) || at(TokenKind::RBracket)) {
                        if (depth == 0) break;
                        --depth;
                    }
                    ++pos_;
                }
                if (eat(TokenKind::Comma)) continue;
                eat(TokenKind::RParen);
                return;
            }
            std::string name;
            std::uint32_t ty = parseDeclarator(sp.base, name, sp.isConst);
            // C array/function parameters decay to pointers. Copy the kind
            // and target BEFORE addType: pointerTo grows the arena, which
            // would invalidate a reference into it.
            const CTypeKind pk = result_.types[ty].kind;
            const std::uint32_t ptgt = result_.types[ty].target;
            if (pk == CTypeKind::Array) {
                ty = pointerTo(ptgt);
            } else if (pk == CTypeKind::Function) {
                ty = pointerTo(ty);
            }
            fn.params.push_back(ty);
            fn.paramNames.push_back(std::move(name));
            if (eat(TokenKind::Comma)) continue;
            expect(TokenKind::RParen, "')'");
            return;
        }
    }

    // Skip to just past the next ';' (error recovery).
    void synchronize() {
        while (!atEnd() && !at(TokenKind::Semicolon)) ++pos_;
        eat(TokenKind::Semicolon);
    }

    // ------------------------------------------------------------- top level

    void externalDecl() {
        if (at(TokenKind::kw__Static_assert)) {
            ++pos_;
            if (at(TokenKind::LParen)) skipBalancedParens();
            expect(TokenKind::Semicolon, "';'");
            return;
        }
        if (at(TokenKind::Semicolon)) {
            ++pos_;
            return;
        }

        Specifiers sp = parseSpecifiers();
        if (!sp.ok) {
            error("expected a declaration");
            synchronize();
            return;
        }

        // No declarator: a bare struct/union/enum definition or ';'.
        if (eat(TokenKind::Semicolon)) return;

        bool first = true;
        for (;;) {
            if (!first) expect(TokenKind::Comma, "',' or ';'");
            first = false;

            std::string name;
            std::uint32_t ty = parseDeclarator(sp.base, name, sp.isConst);
            bool dummyInline = sp.isInline;
            while (skipAttribute(dummyInline)) {}

            if (name.empty()) {
                error("declaration without a name");
                synchronize();
                return;
            }

            const CType& type = result_.types[ty];
            if (sp.isTypedef) {
                typedefs_[name] = ty;
                // `typedef struct { ... } Name;` / `typedef enum { ... } Name;`
                // -- give the anonymous definition the typedef's name: in the
                // table, in the already-emitted declaration, and in the type.
                if (type.kind == CTypeKind::Record &&
                    result_.records[type.target].name.rfind("_anon_", 0) == 0) {
                    const std::string old = result_.records[type.target].name;
                    result_.records[type.target].name = name;
                    result_.types[ty].name = name;
                    for (auto& d : result_.decls)
                        if (auto* rd = std::get_if<CRecordDef>(&d))
                            if (rd->name == old) rd->name = name;
                }
                if (type.kind == CTypeKind::Enum &&
                    result_.enums[type.target].name.rfind("_anon_", 0) == 0) {
                    const std::string old = result_.enums[type.target].name;
                    result_.enums[type.target].name = name;
                    result_.types[ty].name = name;
                    for (auto& d : result_.decls)
                        if (auto* ed = std::get_if<CEnumDef>(&d))
                            if (ed->name == old) ed->name = name;
                }
                result_.decls.push_back(CTypedefDecl{name, ty});
            } else if (type.kind == CTypeKind::Function) {
                CFunctionDecl fn;
                fn.name = name;
                fn.type = ty;
                fn.variadic = type.variadic;
                fn.noReturn = sp.noReturn;
                fn.staticInline = sp.isInline && sp.isStatic;
                const bool hasBody = at(TokenKind::LBrace);
                if (hasBody) {
                    if (!fn.staticInline)
                        warn("function body in header skipped: " + name);
                    skipBalancedBraces();
                }
                result_.decls.push_back(std::move(fn));
                if (hasBody) return; // a definition ends the declaration
                // else: fall through so `int f(void), g(void);` works
            } else {
                CGlobalDecl g;
                g.name = name;
                g.type = ty;
                g.isExtern = sp.isExtern;
                result_.decls.push_back(std::move(g));
            }

            if (at(TokenKind::Assign)) {
                warn("initializer ignored for '" + name + "'");
                while (!atEnd() && !at(TokenKind::Comma) &&
                       !at(TokenKind::Semicolon)) {
                    if (at(TokenKind::LBrace)) skipBalancedBraces();
                    else ++pos_;
                }
            }
            if (at(TokenKind::Semicolon)) {
                ++pos_;
                return;
            }
        }
    }
};

} // namespace

ParseResult parse(const std::vector<Token>& tokens,
                  const ParseOptions& options) {
    return Parser(tokens, options).run();
}

} // namespace insbind
