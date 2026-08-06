
#include <iostream>
#include <memory>
#include <string>

#include <extra/ast.hpp>
#include <extra/type_system.hpp>
#include <lexer/lexer.hpp>
#include <parser/parser.hpp>
#include <sema/sema.hpp>
#include <utilities/errors.hpp>

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failures;                                                 \
            std::cerr << "FAIL: " << __FILE__ << ":" << __LINE__          \
                      << ": " #cond "\n";                                 \
        }                                                                 \
    } while (0)

std::shared_ptr<AST::ProgramRoot> parse(const std::string& source) {
    ErrorReporting::initErrorReporter(source, "<test>");
    Parser parser;
    std::string mutableSource = source;
    auto ast = parser.produceAST(mutableSource);
    return ast;
}

bool parseClean(const std::string& source) {
    auto ast = parse(source);
    bool clean = ast && (!ErrorReporting::globalErrorReporter ||
                         !ErrorReporting::globalErrorReporter->hasError());
    ErrorReporting::cleanupErrorReporter();
    return clean;
}


void testLexer() {
    Lexer lexer;

    auto identTokens = lexer.tokenize("foo bar123 _x");
    int idents = 0;
    for (const auto& t : identTokens) {
        if (t.type == TokenType::Identifier) ++idents;
    }
    CHECK(idents == 3);

    auto kw = lexer.tokenize("fun if else while return");
    CHECK(kw[0].type == TokenType::KwFun);
    CHECK(kw[1].type == TokenType::KwIf);
    CHECK(kw[2].type == TokenType::KwElse);
    CHECK(kw[3].type == TokenType::KwWhile);
    CHECK(kw[4].type == TokenType::KwReturn);

    auto kw2 = lexer.tokenize("switch when");
    CHECK(kw2[0].type == TokenType::KwSwitch);
    CHECK(kw2[1].type == TokenType::KwWhen);

    auto ints = lexer.tokenize("512 0x3F8 0");
    CHECK(ints[0].type == TokenType::IntegerLiteral && ints[0].value == "512");
    CHECK(ints[1].type == TokenType::IntegerLiteral);
    CHECK(ints[1].value.find("3F8") != std::string::npos ||
          ints[1].value.find("0x") != std::string::npos);

    auto str = lexer.tokenize("\"Hello\\n\"");
    CHECK(str[0].type == TokenType::StringLiteral);
    CHECK(str[0].value == "Hello\n");

    auto comment = lexer.tokenize("foo // a comment\nbar");
    int afterComment = 0;
    for (const auto& t : comment) {
        if (t.type == TokenType::Identifier) ++afterComment;
    }
    CHECK(afterComment == 2);

    auto ops = lexer.tokenize("-> == != <= >= && || << >> ~ & @ #");
    CHECK(ops[0].type == TokenType::Arrow);
    CHECK(ops[1].type == TokenType::EqEq);
    CHECK(ops[2].type == TokenType::NotEq);
    CHECK(ops[7].type == TokenType::Shl);
    CHECK(ops[8].type == TokenType::Shr);
    CHECK(ops[9].type == TokenType::Tilde);
    CHECK(ops[11].type == TokenType::At);
    CHECK(ops[12].type == TokenType::Hash);

    auto attr = lexer.tokenize("[name(x)]");
    CHECK(attr[0].type == TokenType::LBracket);

    auto scope = lexer.tokenize("std::io");
    CHECK(scope[0].type == TokenType::Identifier && scope[0].value == "std");
    CHECK(scope[1].type == TokenType::ColonColon);
    CHECK(scope[2].type == TokenType::Identifier && scope[2].value == "io");

    ErrorReporting::initErrorReporter("\x01", "<test>");
    auto bad = lexer.tokenize("\x01");
    CHECK(!bad.empty());
    ErrorReporting::cleanupErrorReporter();
}


void testParser() {
    CHECK(parseClean("module main\nfun main() -> i32 {\n  return 0\n}\n"));

    auto ast = parse("module foo\nimport io\nimport bar as b\n");
    CHECK(ast && ast->moduleName == "foo");
    CHECK(ast);
    ErrorReporting::cleanupErrorReporter();

    auto scoped = parse("import std::io\n");
    CHECK(scoped && !scoped->imports.empty() &&
          scoped->imports.front() == "std::io");
    ErrorReporting::cleanupErrorReporter();

    auto selective = parse("import std::math.{ max, min }\n");
    CHECK(selective && !selective->imports.empty() &&
          selective->imports.front() == "std::math");
    {
        std::shared_ptr<AST::ImportStatement> imp;
        if (selective) {
            for (const auto& n : selective->body) {
                if (auto i = AST::ast_cast<AST::ImportStatement>(n)) {
                    imp = i;
                    break;
                }
            }
        }
        CHECK(imp && imp->importedSymbols.size() == 2 &&
              imp->importedSymbols[0] == "max" &&
              imp->importedSymbols[1] == "min");
    }
    ErrorReporting::cleanupErrorReporter();

    CHECK(parseClean("fun add(i64 a, i64 b) -> i64 {\n  return a + b\n}\n"));
    CHECK(parseClean("fun f() -> void {\n  i32 x = 1 + 2 * 3\n}\n"));

    CHECK(parseClean("enum E {\n  A(i64),\n  B(i64, i64),\n  C\n}\n"
                     "fun f(E e) -> void {\n"
                     "  switch e {\n"
                     "    A(v) => return\n"
                     "    B(x, y) => { return }\n"
                     "    _ => return\n"
                     "  }\n"
                     "}\n"));

    {
        auto sw = parse("enum E {\n  A(i64, i64),\n  B\n}\n"
                        "fun f(E e) -> void {\n"
                        "  switch e {\n"
                        "    A(x, y) => return\n"
                        "    _ => return\n"
                        "  }\n"
                        "}\n");
        std::shared_ptr<AST::SwitchStatement> snode;
        if (sw) {
            for (const auto& top : sw->body) {
                if (auto fn = AST::ast_cast<AST::FunctionDeclaration>(top)) {
                    for (const auto& s : fn->body) {
                        if (auto sst = AST::ast_cast<AST::SwitchStatement>(s)) {
                            snode = sst;
                            break;
                        }
                    }
                }
            }
        }
        CHECK(snode && snode->arms.size() == 2);
        CHECK(snode && snode->arms[0].variant == "A" &&
              snode->arms[0].bindings.size() == 2 &&
              !snode->arms[0].isDefault);
        CHECK(snode && snode->arms[1].isDefault &&
              snode->arms[1].variant.empty());
        ErrorReporting::cleanupErrorReporter();
    }
    CHECK(parseClean("fun f() -> void {\n  if 1 < 2 {\n    return\n  } else {\n    return\n  }\n}\n"));
    CHECK(parseClean("fun f() -> void {\n  while true {\n    break\n  }\n}\n"));
    CHECK(parseClean("fun f() -> void {\n  loop {\n    break\n  }\n}\n"));

    {
        // `export` marks a declaration as visible to importers; without it,
        // top-level declarations are private to their module.
        auto ex = parse("module m\n"
                        "export fun pub() -> void {\n  return\n}\n"
                        "fun priv() -> void {\n  return\n}\n"
                        "export struct S { i32 a }\n");
        std::shared_ptr<AST::FunctionDeclaration> pub;
        std::shared_ptr<AST::FunctionDeclaration> priv;
        std::shared_ptr<AST::StructDeclaration> st;
        if (ex) {
            for (const auto& top : ex->body) {
                if (auto fn = AST::ast_cast<AST::FunctionDeclaration>(top)) {
                    if (fn->name == "pub") pub = fn;
                    if (fn->name == "priv") priv = fn;
                } else if (auto s = AST::ast_cast<AST::StructDeclaration>(top)) {
                    st = s;
                }
            }
        }
        CHECK(pub && pub->isExported);
        CHECK(priv && !priv->isExported);
        CHECK(st && st->isExported);
        ErrorReporting::cleanupErrorReporter();
    }

    auto broken = parse("fun f( -> {{{");
    CHECK(broken != nullptr);
    ErrorReporting::cleanupErrorReporter();

    auto vd = parse("fun f() -> void {\n  u8 x = 5\n}\n");
    CHECK(vd != nullptr);
    ErrorReporting::cleanupErrorReporter();
}


bool semaClean(const std::string& source) {
    auto ast = parse(source);
    if (!ast) {
        ErrorReporting::cleanupErrorReporter();
        return false;
    }
    Types::TypeContext types;
    Sema::Analyzer analyzer(types, ErrorReporting::globalErrorReporter.get());
    Sema::SemaResult result = analyzer.analyze(ast, {});
    bool clean = result.ok && (!ErrorReporting::globalErrorReporter ||
                               !ErrorReporting::globalErrorReporter->hasError());
    ErrorReporting::cleanupErrorReporter();
    return clean;
}

void testSema() {
    CHECK(semaClean("module main\nfun main() -> i32 {\n  i32 x = 1\n  return x\n}\n"));
    CHECK(semaClean("fun add(i64 a, i64 b) -> i64 {\n  return a + b\n}\n"));

    CHECK(!semaClean("fun f() -> void {\n  wibble x = 1\n}\n"));
    CHECK(!semaClean("fun f() -> i32 {\n  return missing\n}\n"));
    CHECK(!semaClean("fun f() -> i32 {\n  i32 x = 1\n}\n"));
    CHECK(!semaClean("fun f() -> void {\n  i64 p = 0\n  i8* q = cast<i8*>(p)\n}\n"));
    CHECK(semaClean("fun f() -> void {\n  i64 p = 0\n  unsafe {\n    i8* q = cast<i8*>(p)\n  }\n}\n"));
    CHECK(semaClean("fun f() -> i64 {\n  i32 x = 5\n  i64 y = cast<i64>(x)\n  return y\n}\n"));
    CHECK(!semaClean("fun f() -> void {\n  i32 x = 1\n  i32 x = 2\n}\n"));
    CHECK(semaClean("fun f() -> void {\n  i32 x = 1\n  if 1 < 2 {\n    i32 x = 2\n  }\n}\n"));
    CHECK(!semaClean("fun g(i64 a) -> i64 {\n  return a\n}\nfun f() -> i64 {\n  return g()\n}\n"));
    CHECK(semaClean("fun g(i64 a) -> i64 {\n  return a\n}\nfun f() -> i64 {\n  return g(7)\n}\n"));
    CHECK(!semaClean("fun f(u8* p) -> u8* {\n  return @realloc(p, 32)\n}\n"));
    CHECK(semaClean("fun f(u8* p) -> u8* {\n  unsafe {\n    return @realloc(p, 32)\n  }\n}\n"));
    CHECK(semaClean("fun f(u8* p) -> u8* {\n  unsafe {\n    return @realloc(p, 32, 16)\n  }\n}\n"));
    CHECK(semaClean("fun f(u8* p) -> u8* {\n  unsafe {\n    return @realloc(p, 16, 32, 16)\n  }\n}\n"));
    CHECK(!semaClean("fun f(u8* p) -> u8* {\n  unsafe {\n    return @realloc(p, 1, 2, 3, 4)\n  }\n}\n"));

    // Slices (`T[]`): `.len` is i64, `.ptr` is T*, and a fixed array / another
    // slice initializes a slice-typed variable.
    CHECK(semaClean("fun f() -> i64 {\n  i32[3] a\n  i32[] s = a\n  return s.len\n}\n"));
    CHECK(semaClean("fun f() -> i32 {\n  i32[] s = [1, 2, 3]\n  return s[0]\n}\n"));
    CHECK(semaClean("fun sum(i32[] xs) -> i32 {\n  return xs[0]\n}\n"
                    "fun f() -> i32 {\n  i32[2] a\n  i32[] s = a\n  return sum(s)\n}\n"));
    // A raw pointer is not a slice (no length), so it cannot initialize `T[]`.
    CHECK(!semaClean("fun f(i32* p) -> void {\n  i32[] s = p\n}\n"));
    // `.len` is i64; narrowing to i32 without a cast is rejected.
    CHECK(!semaClean("fun f() -> i32 {\n  i32[] s = [1, 2]\n  i32 n = s.len\n  return n\n}\n"));
    // Slices expose only `.ptr` / `.len`.
    CHECK(!semaClean("fun f() -> i32 {\n  i32[] s = [1]\n  return s.cap\n}\n"));
    // `.ptr` has pointer type; dereferencing/indexing it is an unsafe op.
    CHECK(semaClean("fun f() -> i32 {\n  i32[] s = [1, 2]\n  i32* p = s.ptr\n"
                    "  unsafe {\n    return p[1]\n  }\n}\n"));

    // Sub-slicing `s[a..b]` yields a slice; open bounds are allowed.
    CHECK(semaClean("fun f() -> i64 {\n  i32[] s = [1,2,3,4]\n  i32[] t = s[1..3]\n"
                    "  return t.len\n}\n"));
    CHECK(semaClean("fun f() -> i64 {\n  i32[] s = [1,2,3,4]\n  return s[2..].len\n}\n"));
    CHECK(semaClean("fun f() -> i64 {\n  i32[] s = [1,2,3,4]\n  return s[..2].len\n}\n"));
    CHECK(semaClean("fun f() -> i64 {\n  i32[] s = [1,2,3,4]\n  return s[..].len\n}\n"));
    // Slicing a fixed array is fine; slicing text yields u8[].
    CHECK(semaClean("fun f() -> i64 {\n  i32[4] a\n  i32[] s = a[1..3]\n  return s.len\n}\n"));
    CHECK(semaClean("fun f() -> u8 {\n  text s = \"hello\"\n  u8[] b = s[1..3]\n  return b[0]\n}\n"));
    // A slice bound must be an integer.
    CHECK(!semaClean("fun f() -> void {\n  i32[] s = [1,2,3]\n  i32[] t = s[true..2]\n}\n"));
    // Slicing a raw pointer needs an explicit end and an unsafe context.
    CHECK(!semaClean("fun f(i32* p) -> void {\n  i32[] s = p[0..2]\n}\n"));
    CHECK(semaClean("fun f(i32* p) -> void {\n  unsafe {\n    i32[] s = p[0..2]\n  }\n}\n"));
    CHECK(!semaClean("fun f(i32* p) -> void {\n  unsafe {\n    i32[] s = p[2..]\n  }\n}\n"));

    // for-in: range and iterable forms bind the loop variable in a fresh scope.
    CHECK(semaClean("fun f() -> i32 {\n  i32 t = 0\n  for i in 0..5 { t = t + i }\n  return t\n}\n"));
    CHECK(semaClean("fun f() -> i32 {\n  i32 t = 0\n  i32[] s = [1, 2, 3]\n"
                    "  for x in s { t = t + x }\n  return t\n}\n"));
    CHECK(semaClean("fun f() -> i32 {\n  i32 t = 0\n  i32[3] a\n"
                    "  for x in a { t = t + x }\n  return t\n}\n"));
    CHECK(semaClean("fun f() -> i32 {\n  i32 t = 0\n  for c in \"hi\" { t = t + cast<i32>(c) }\n"
                    "  return t\n}\n"));
    CHECK(semaClean("fun f() -> i32 {\n  i32 t = 0\n  i32[] s = [1,2,3,4]\n"
                    "  for x in s[1..3] { t = t + x }\n  return t\n}\n"));
    // The loop variable is scoped to the loop body.
    CHECK(!semaClean("fun f() -> i32 {\n  for i in 0..3 { }\n  return i\n}\n"));
    // A non-iterable source is rejected.
    CHECK(!semaClean("fun f() -> void {\n  i32 n = 3\n  for x in n { }\n}\n"));
    // Range bounds must be integers.
    CHECK(!semaClean("fun f() -> void {\n  for i in 0..true { }\n}\n"));
    // break / skip are valid inside a for body.
    CHECK(semaClean("fun f() -> i32 {\n  i32 t = 0\n  for i in 0..5 { if i == 2 { skip }\n"
                    "    if i == 4 { break }\n    t = t + 1 }\n  return t\n}\n"));
    // `in` remains usable as an ordinary identifier outside the for header.
    CHECK(semaClean("fun f() -> i32 {\n  i32 in = 7\n  return in\n}\n"));

    // Generic monomorphization must substitute the type parameter inside compound
    // spellings (T[], T*), not just a bare T -- this is what lets a generic
    // container declare `T[]` locals and `new T[n]` storage.
    CHECK(semaClean(
        "class Vec<T> {\n"
        "  T[] items\n"
        "  constructor() { this.items = new T[4] }\n"
        "  fun grow() -> void {\n"
        "    T[] bigger = new T[8]\n"
        "    i64 i = 0\n"
        "    while i < 4 { bigger[i] = this.items[i]; i = i + 1 }\n"
        "    this.items = bigger\n"
        "  }\n"
        "  fun first() -> T { return this.items[0] }\n"
        "}\n"
        "fun main() -> i32 {\n"
        "  Vec<i32> v = Vec<i32>()\n"
        "  return v.first()\n"
        "}\n"));
    // A generic function with a `T*` local monomorphizes too.
    CHECK(semaClean(
        "fun idptr<T>(T value) -> T {\n"
        "  T local = value\n"
        "  T* p = &local\n"
        "  unsafe { return ~p }\n"
        "}\n"
        "fun main() -> i32 { return idptr<i32>(7) }\n"));

    // Tagged-union enums + switch: construction, payload binding, exhaustiveness.
    const char* exprAst =
        "enum Expr {\n"
        "  Lit(i64),\n"
        "  Add(Expr*, Expr*),\n"
        "  Nil\n"
        "}\n";
    CHECK(semaClean(std::string(exprAst) +
        "fun f(Expr e) -> i64 {\n"
        "  switch e {\n"
        "    Lit(v) => return v\n"
        "    Add(l, r) => return 1\n"
        "    Nil => return 0\n"
        "  }\n"
        "  return 0\n"
        "}\n"
        "fun main() -> i32 {\n"
        "  Expr a = Expr.Lit(5)\n"
        "  Expr n = Expr.Nil\n"
        "  return cast<i32>(f(a))\n"
        "}\n"));
    // Non-exhaustive switch is rejected (missing Nil).
    CHECK(!semaClean(std::string(exprAst) +
        "fun f(Expr e) -> i64 {\n"
        "  switch e { Lit(v) => return v, Add(l, r) => return 1 }\n"
        "  return 0\n"
        "}\n"));
    // A `_` arm makes it exhaustive.
    CHECK(semaClean(std::string(exprAst) +
        "fun f(Expr e) -> i64 {\n"
        "  switch e { Lit(v) => return v, _ => return 0 }\n"
        "  return 0\n"
        "}\n"));
    // Unknown variant in a switch arm is rejected.
    CHECK(!semaClean(std::string(exprAst) +
        "fun f(Expr e) -> i64 {\n"
        "  switch e { Lit(v) => return v, Bogus => return 0, _ => return 1 }\n"
        "  return 0\n"
        "}\n"));
    // Wrong binding arity is rejected (Add carries two fields).
    CHECK(!semaClean(std::string(exprAst) +
        "fun f(Expr e) -> i64 {\n"
        "  switch e { Lit(v) => return v, Add(x) => return 1, Nil => return 0 }\n"
        "  return 0\n"
        "}\n"));
    // Constructing a payload variant with the wrong argument count is rejected.
    CHECK(!semaClean(std::string(exprAst) +
        "fun main() -> i32 { Expr a = Expr.Lit(1, 2)\n  return 0 }\n"));
    // switch on a non-sum type is rejected.
    CHECK(!semaClean(
        "fun f(i32 x) -> i32 { switch x { _ => return 0 }\n  return 0 }\n"));
    // A by-value sum-type payload (unsupported in the MVP) is rejected.
    CHECK(!semaClean(
        "struct P { i32 x }\n"
        "enum Bad { Has(P) }\n"
        "fun main() -> i32 { return 0 }\n"));
}

}

int main() {
    testLexer();
    testParser();
    testSema();

    std::cout << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    if (g_failures > 0) {
        std::cerr << g_failures << " checks FAILED\n";
        return 1;
    }
    std::cout << "all unit tests passed\n";
    return 0;
}

