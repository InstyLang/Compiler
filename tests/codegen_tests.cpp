
#include <iostream>
#include <memory>
#include <string>

#include <codegen/codegen.hpp>
#include <extra/type_system.hpp>
#include <lexer/lexer.hpp>
#include <parser/parser.hpp>
#include <sema/sema.hpp>
#include <utilities/config.hpp>
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

std::string compileToIR(const std::string& source, bool& verified) {
    verified = false;
    ErrorReporting::initErrorReporter(source, "<test>");
    Parser parser;
    std::string mutableSource = source;
    auto ast = parser.produceAST(mutableSource);
    if (!ast || (ErrorReporting::globalErrorReporter &&
                 ErrorReporting::globalErrorReporter->hasError())) {
        ErrorReporting::cleanupErrorReporter();
        return "";
    }

    Types::TypeContext types;
    Sema::Analyzer analyzer(types, ErrorReporting::globalErrorReporter.get());
    Sema::SemaResult sema = analyzer.analyze(ast, {});
    if (!sema.ok || (ErrorReporting::globalErrorReporter &&
                     ErrorReporting::globalErrorReporter->hasError())) {
        ErrorReporting::cleanupErrorReporter();
        return "";
    }

    Config::CompilerConfig config;
    config.noStd = true;
    CodeGenerator codegen(config, types, ErrorReporting::globalErrorReporter.get());
    if (!codegen.generate(ast, sema)) {
        ErrorReporting::cleanupErrorReporter();
        return "";
    }

    std::string err;
    verified = codegen.verify(err);
    if (!verified) {
        std::cerr << "verify error: " << err << "\n";
    }
    std::string ir = codegen.emitLLVM();
    ErrorReporting::cleanupErrorReporter();
    return ir;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

void testReturnInt() {
    bool verified = false;
    std::string ir = compileToIR("module main\nfun main() -> i32 {\n  return 42\n}\n", verified);
    CHECK(verified);
    CHECK(contains(ir, "define"));
    CHECK(contains(ir, "main"));
    CHECK(contains(ir, "42"));
}

void testArithmetic() {
    bool verified = false;
    std::string ir = compileToIR(
        "fun add(i64 a, i64 b) -> i64 {\n  return a + b\n}\n", verified);
    CHECK(verified);
    CHECK(contains(ir, "add"));
}

void testComparisonAndIf() {
    bool verified = false;
    std::string ir = compileToIR(
        "fun f(i32 x) -> i32 {\n  if x < 10 {\n    return 1\n  } else {\n    return 0\n  }\n}\n",
        verified);
    CHECK(verified);
    CHECK(contains(ir, "icmp"));
    CHECK(contains(ir, "br "));
}

void testWhileLoop() {
    bool verified = false;
    std::string ir = compileToIR(
        "fun f() -> i32 {\n  i32 i = 0\n  while i < 5 {\n    i = i + 1\n  }\n  return i\n}\n",
        verified);
    CHECK(verified);
    CHECK(contains(ir, "br "));
}

void testInfiniteLoop() {
    bool verified = false;
    std::string ir = compileToIR(
        "fun f() -> i32 {\n  loop {\n    return 7\n  }\n}\n", verified);
    CHECK(verified);
}

void testCall() {
    bool verified = false;
    std::string ir = compileToIR(
        "fun g(i64 a) -> i64 {\n  return a\n}\n"
        "fun main() -> i64 {\n  return g(3)\n}\n", verified);
    CHECK(verified);
    CHECK(contains(ir, "call"));
}

void testStringConstant() {
    bool verified = false;
    std::string ir = compileToIR(
        "module main\nfun main() -> i64 {\n  text s = \"hi\"\n  return 0\n}\n", verified);
    CHECK(verified);
    CHECK(contains(ir, "hi") || contains(ir, "private"));
}

void testStartEmitted() {
    bool verified = false;
    std::string ir = compileToIR("module main\nfun main() -> i32 {\n  return 0\n}\n", verified);
    CHECK(verified);
    CHECK(contains(ir, "_start"));
}

void testStruct() {
    bool verified = false;
    std::string ir = compileToIR(
        "module main\nstruct Point {\n  i32 x,\n  i32 y\n}\n"
        "fun main() -> i32 {\n  Point p = Point{ x: 3, y: 4 }\n  return p.x\n}\n",
        verified);
    CHECK(verified);
    CHECK(contains(ir, "getelementptr") || contains(ir, "Point"));
}

void testEnum() {
    bool verified = false;
    std::string ir = compileToIR(
        "module main\nenum Status : i32 {\n  Pending,\n  Ready\n}\n"
        "fun main() -> i32 {\n  i32 s = Ready\n  return s\n}\n",
        verified);
    CHECK(verified);
}

void testEnumInterop() {
    bool verified = false;
    std::string ir = compileToIR(
        "module main\nenum MemType : u32 {\n  Free,\n  Reserved,\n  Mmio\n}\n"
        "fun classify(u32 t) -> MemType {\n  if t == 7 {\n    return Free\n  }\n"
        "  return Reserved\n}\n"
        "fun main() -> i32 {\n  MemType m = classify(7)\n  if m == Free {\n"
        "    return 1\n  }\n  return 0\n}\n",
        verified);
    CHECK(verified);
    CHECK(contains(ir, "icmp eq i32"));
}

void testGlobalAndPointerArith() {
    bool verified = false;
    std::string ir = compileToIR(
        "module main\nu64 counter = 5\n"
        "fun main() -> i64 {\n  unsafe {\n    u64* p = &counter\n    return cast<i64>(~p)\n  }\n}\n",
        verified);
    CHECK(verified);
    CHECK(contains(ir, "@counter"));
}

void testIntrinsics() {
    bool verified = false;
    std::string ir = compileToIR(
        "module lib\nfun f(u16* r) -> u16 {\n  unsafe {\n"
        "    volatileStore<u16>(r, 5)\n    return volatileLoad<u16>(r)\n  }\n}\n",
        verified);
    CHECK(verified);
    CHECK(contains(ir, "volatile"));
}

void testInlineAsm() {
    bool verified = false;
    std::string ir = compileToIR(
        "module lib\nfun f() -> void {\n  unsafe {\n    asm(\"nop\")\n  }\n}\n",
        verified);
    CHECK(verified);
    CHECK(contains(ir, "asm"));
}

void testClasses() {
    bool verified = false;
    std::string ir = compileToIR(
        "module main\nclass Counter {\n  i32 value\n"
        "  constructor(i32 initial) {\n    this.value = initial\n  }\n"
        "  fun inc() -> void {\n    this.value = this.value + 1\n  }\n"
        "  fun get() -> i32 {\n    return this.value\n  }\n}\n"
        "fun main() -> i32 {\n  Counter c = Counter(5)\n  c.inc()\n  return c.get()\n}\n",
        verified);
    CHECK(verified);
    CHECK(contains(ir, "Counter_constructor_i32"));
    CHECK(contains(ir, "Counter_inc"));
    CHECK(contains(ir, "Counter_get"));
}

void testOperatorOverload() {
    bool verified = false;
    std::string ir = compileToIR(
        "module main\nclass Vec {\n  i32 x\n"
        "  constructor(i32 a) {\n    this.x = a\n  }\n"
        "  operator +(Vec rhs) -> i32 {\n    return this.x + rhs.x\n  }\n}\n"
        "fun main() -> i32 {\n  Vec a = Vec(1)\n  Vec b = Vec(2)\n  return a + b\n}\n",
        verified);
    CHECK(verified);
    CHECK(contains(ir, "Vec_operator_add"));
}

void testGenerics() {
    bool verified = false;
    std::string ir = compileToIR(
        "module main\nfun id<T>(T value) -> T {\n  return value\n}\n"
        "fun main() -> i32 {\n  i32 a = id<i32>(7)\n  i64 b = id<i64>(35)\n  return a\n}\n",
        verified);
    CHECK(verified);
    CHECK(contains(ir, "id_i32"));
    CHECK(contains(ir, "id_i64"));
}

void testInterpolation() {
    bool verified = false;
    std::string ir = compileToIR(
        "module main\nfun main() -> i32 {\n"
        "  i32 n = 7\n  bool ok = true\n  f64 pi = 3.5\n"
        "  text s = \"n=$n ok=$ok pi=$pi sum=${n + 1}\"\n"
        "  return 0\n}\n",
        verified);
    CHECK(verified);
    CHECK(contains(ir, "__ins_fmt_int"));
    CHECK(contains(ir, "__ins_fmt_float"));
    CHECK(contains(ir, "__ins_fmt_raw"));
    CHECK(contains(ir, "__ins_fmt_finish"));
    CHECK(!contains(ir, "__ins_alloc"));
}

void testInterpolationStruct() {
    bool verified = false;
    std::string ir = compileToIR(
        "module main\nstruct P {\n  i32 x\n  i32 y\n}\n"
        "fun main() -> i32 {\n  P p = P{ x: 1, y: 2 }\n"
        "  text s = \"p=$p\"\n  return 0\n}\n",
        verified);
    CHECK(verified);
    CHECK(contains(ir, "__ins_fmt_raw"));
}

void testTextIndex() {
    bool verified = false;
    std::string ir = compileToIR(
        "module main\nfun main() -> i32 {\n  text s = \"hi\"\n"
        "  unsafe {\n    return cast<i32>(s[0])\n  }\n}\n",
        verified);
    CHECK(verified);
    CHECK(contains(ir, "load i8"));
}

void testSizeof() {
    bool verified = false;
    std::string ir = compileToIR(
        "module main\nstruct R {\n  u64 base,\n  u64 pages,\n  u32 type,\n  u32 pad\n}\n"
        "fun main() -> i64 {\n  u64 a = sizeof<u32>()\n  u64 b = sizeof<R>()\n"
        "  return cast<i64>(a * 1000 + b)\n}\n",
        verified);
    CHECK(verified);
    CHECK(contains(ir, "store i64 4,"));
    CHECK(contains(ir, "store i64 24,"));
}

void testWideString() {
    bool verified = false;
    std::string ir = compileToIR(
        "module main\nfun main() -> i64 {\n  unsafe {\n"
        "    u16* p = @utf16(\"AB\")\n    return cast<i64>(p[0])\n  }\n}\n",
        verified);
    CHECK(verified);
    CHECK(contains(ir, "x i16]"));
    CHECK(contains(ir, "i16 65, i16 66, i16 0"));
}

void testReinterpretReads() {
    bool verified = false;
    std::string ir = compileToIR(
        "module main\nfun main() -> i64 {\n  unsafe {\n"
        "    u8[16] buf\n    u64 base = cast<u64>(&buf)\n"
        "    u32* w = cast<u32*>(base + 4)\n    w[0] = 0x1234\n"
        "    u64* q = cast<u64*>(base + 8)\n    q[0] = q[0] + 16\n"
        "    return cast<i64>(w[0])\n  }\n}\n",
        verified);
    CHECK(verified);
    CHECK(contains(ir, "load i32"));
    CHECK(contains(ir, "load i64"));
}

void testExtern() {
    bool verified = false;
    std::string ir = compileToIR(
        "module main\n"
        "extern fun ext_alloc(i64 size) -> i64\n"
        "fun main() -> i64 {\n  return ext_alloc(8)\n}\n",
        verified);
    CHECK(verified);
    // extern emits a declaration, not a definition, for the external symbol...
    CHECK(contains(ir, "declare"));
    // ...with an UNMANGLED symbol name (must not be mangled to main_ext_alloc).
    CHECK(contains(ir, "@ext_alloc"));
    CHECK(!contains(ir, "@main_ext_alloc"));
    // and main calls it.
    CHECK(contains(ir, "call"));
}

void testExport() {
    bool verified = false;
    // In a non-`main` module, exported functions keep external linkage so
    // importers can link against them; non-exported (private-by-default)
    // functions are lowered to internal linkage and hidden.
    std::string ir = compileToIR(
        "module lib\n"
        "export fun pub_fn(i64 n) -> i64 {\n  return priv_fn(n) + 1\n}\n"
        "fun priv_fn(i64 n) -> i64 {\n  return n * 2\n}\n",
        verified);
    CHECK(verified);
    // The exported function is an external definition (no `internal` modifier).
    CHECK(contains(ir, "define i64 @lib_pub_fn"));
    CHECK(!contains(ir, "define internal i64 @lib_pub_fn"));
    // The private helper is lowered with internal linkage.
    CHECK(contains(ir, "define internal i64 @lib_priv_fn"));
}

void testExportGlobal() {
    bool verified = false;
    // Exported globals get external linkage; private globals stay internal.
    std::string ir = compileToIR(
        "module lib\n"
        "export i32 PubG = 7\n"
        "i32 PrivG = 9\n"
        "export fun get() -> i32 {\n  return PubG + PrivG\n}\n",
        verified);
    CHECK(verified);
    CHECK(contains(ir, "@PubG = global"));
    CHECK(contains(ir, "@PrivG = internal global"));
}

}

int main() {
    testReturnInt();
    testArithmetic();
    testComparisonAndIf();
    testWhileLoop();
    testInfiniteLoop();
    testCall();
    testStringConstant();
    testStartEmitted();
    testStruct();
    testEnum();
    testEnumInterop();
    testGlobalAndPointerArith();
    testIntrinsics();
    testInlineAsm();
    testClasses();
    testOperatorOverload();
    testGenerics();
    testInterpolation();
    testInterpolationStruct();
    testTextIndex();
    testSizeof();
    testWideString();
    testReinterpretReads();
    testExtern();
    testExport();
    testExportGlobal();

    std::cout << (g_checks - g_failures) << "/" << g_checks << " codegen checks passed\n";
    if (g_failures > 0) {
        std::cerr << g_failures << " codegen checks FAILED\n";
        return 1;
    }
    std::cout << "all codegen tests passed\n";
    return 0;
}
