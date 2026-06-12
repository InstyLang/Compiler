
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

    CHECK(parseClean("fun f() -> void {\n"
                     "  i64 n = 2\n"
                     "  switch n {\n"
                     "    0 -> return\n"
                     "    1, 2, 3 -> { return }\n"
                     "    _ -> return\n"
                     "  }\n"
                     "}\n"));

    {
        auto sw = parse("fun f() -> void {\n"
                        "  i64 n = 0\n"
                        "  switch n {\n"
                        "    1, 2 -> return\n"
                        "    _ -> return\n"
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
        CHECK(snode && snode->arms[0].patterns.size() == 2 &&
              !snode->arms[0].isDefault);
        CHECK(snode && snode->arms[1].isDefault &&
              snode->arms[1].patterns.empty());
        ErrorReporting::cleanupErrorReporter();
    }
    CHECK(parseClean("fun f() -> void {\n  if 1 < 2 {\n    return\n  } else {\n    return\n  }\n}\n"));
    CHECK(parseClean("fun f() -> void {\n  while true {\n    break\n  }\n}\n"));
    CHECK(parseClean("fun f() -> void {\n  loop {\n    break\n  }\n}\n"));

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
