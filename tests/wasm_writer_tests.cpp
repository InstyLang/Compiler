// Unit tests for the WebAssembly binary writer.
//
// The writer has no dependency on Machine IR or the rest of the backend, so
// these tests are pure encoder tests: they build modules with the public API
// and compare the emitted bytes against expectations derived by hand from the
// binary-format spec. Getting LEB128 and section framing wrong produces a file
// that a runtime rejects with a useless "invalid section" message, so the
// golden byte comparisons here are the cheapest place to catch it.
//
// Usage: wasm_writer_tests [output-base]
//   writes <output-base>_add.wasm and <output-base>_start.wasm, which the
//   companion CMake test validates with wasm-tools / wasmtime when available.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <backend/wasm_writer.hpp>

using namespace Backend::Wasm;

static int g_failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    } else {
        std::printf("ok: %s\n", what);
    }
}

static void dump(const char* label, const std::vector<std::uint8_t>& bytes) {
    std::printf("  %s (%zu bytes):", label, bytes.size());
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i % 16 == 0) std::printf("\n   ");
        std::printf(" %02X", bytes[i]);
    }
    std::printf("\n");
}

static void checkBytes(const char* what, const std::vector<std::uint8_t>& got,
                       const std::vector<std::uint8_t>& want) {
    if (got == want) {
        std::printf("ok: %s\n", what);
        return;
    }
    std::printf("FAIL: %s\n", what);
    dump("got ", got);
    dump("want", want);
    ++g_failures;
}

// --- LEB128 ---------------------------------------------------------------

static std::vector<std::uint8_t> u32Bytes(std::uint32_t v) {
    std::vector<std::uint8_t> out;
    leb::writeU32(out, v);
    return out;
}

static std::vector<std::uint8_t> i64Bytes(std::int64_t v) {
    std::vector<std::uint8_t> out;
    leb::writeI64(out, v);
    return out;
}

static void testLeb() {
    std::printf("\n-- LEB128 --\n");

    checkBytes("uleb 0", u32Bytes(0), {0x00});
    checkBytes("uleb 1", u32Bytes(1), {0x01});
    checkBytes("uleb 127", u32Bytes(127), {0x7F});
    checkBytes("uleb 128", u32Bytes(128), {0x80, 0x01});
    // The canonical worked example from the LEB128 literature.
    checkBytes("uleb 624485", u32Bytes(624485), {0xE5, 0x8E, 0x26});
    checkBytes("uleb 0xFFFFFFFF", u32Bytes(0xFFFFFFFFu),
               {0xFF, 0xFF, 0xFF, 0xFF, 0x0F});

    checkBytes("sleb 0", i64Bytes(0), {0x00});
    checkBytes("sleb 1", i64Bytes(1), {0x01});
    checkBytes("sleb 42", i64Bytes(42), {0x2A});
    checkBytes("sleb 63", i64Bytes(63), {0x3F});
    // 64 sets bit 6, which would read as negative, so it needs a second byte.
    checkBytes("sleb 64 needs a continuation", i64Bytes(64), {0xC0, 0x00});
    checkBytes("sleb -1 is one byte", i64Bytes(-1), {0x7F});
    checkBytes("sleb -64", i64Bytes(-64), {0x40});
    checkBytes("sleb -65", i64Bytes(-65), {0xBF, 0x7F});
}

// --- section walking ------------------------------------------------------

// Reads back the section ids in the order they appear, so ordering rules can be
// asserted without a full parser.
static std::vector<std::uint8_t> sectionIds(const std::vector<std::uint8_t>& mod) {
    std::vector<std::uint8_t> ids;
    std::size_t i = 8;  // skip magic + version
    while (i < mod.size()) {
        ids.push_back(mod[i]);
        ++i;
        std::uint32_t size = 0;
        unsigned shift = 0;
        while (i < mod.size()) {
            const std::uint8_t byte = mod[i++];
            size |= static_cast<std::uint32_t>(byte & 0x7F) << shift;
            if ((byte & 0x80) == 0) break;
            shift += 7;
        }
        i += size;
    }
    return ids;
}

// --- the canonical add module --------------------------------------------

// (module
//   (func (export "add") (param i32 i32) (result i32)
//     local.get 0
//     local.get 1
//     i32.add))
static Module buildAddModule() {
    Module m;
    const std::uint32_t type =
        m.addType(FuncType{{ValType::I32, ValType::I32}, {ValType::I32}});

    CodeBuilder b;
    b.localGet(0);
    b.localGet(1);
    b.op(Op::I32Add);

    FunctionBody body;
    body.code = b.take();

    const std::uint32_t fn = m.addFunction(type, std::move(body));
    m.exportFunction("add", fn);
    return m;
}

static void testAddModule() {
    std::printf("\n-- add(i32,i32) module --\n");

    Module m = buildAddModule();
    check(m.ok(), "module built without error");
    const std::vector<std::uint8_t> got = m.encode();

    // Assembled by hand from the binary format:
    //   preamble  00 61 73 6D 01 00 00 00
    //   type    1 size 7 : 1 type, 0x60, params [i32 i32], results [i32]
    //   func    3 size 2 : 1 function, type index 0
    //   export  7 size 7 : 1 export, "add", kind 0 (func), index 0
    //   code   10 size 9 : 1 body, size 7, 0 local runs, code, end
    const std::vector<std::uint8_t> want = {
        0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x07, 0x01, 0x60, 0x02, 0x7F, 0x7F, 0x01, 0x7F,
        0x03, 0x02, 0x01, 0x00,
        0x07, 0x07, 0x01, 0x03, 0x61, 0x64, 0x64, 0x00, 0x00,
        0x0A, 0x09, 0x01, 0x07, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6A, 0x0B,
    };
    checkBytes("add module encodes byte-for-byte", got, want);

    check(got.size() >= 8 && got[0] == 0x00 && got[1] == 0x61 && got[2] == 0x73 &&
              got[3] == 0x6D,
          "magic is \\0asm");
    check(got.size() >= 8 && got[4] == 0x01 && got[5] == 0x00 && got[6] == 0x00 &&
              got[7] == 0x00,
          "version is 1");
}

// --- type interning -------------------------------------------------------

static void testTypeDedup() {
    std::printf("\n-- type interning --\n");

    Module m;
    const std::uint32_t a = m.addType(FuncType{{ValType::I32}, {ValType::I32}});
    const std::uint32_t b = m.addType(FuncType{{ValType::I32}, {ValType::I32}});
    const std::uint32_t c = m.addType(FuncType{{ValType::I64}, {ValType::I32}});
    const std::uint32_t d = m.addType(FuncType{{ValType::I32}, {}});

    check(a == 0, "first type gets index 0");
    check(a == b, "structurally identical signature is interned");
    check(c == 1, "differing parameter type gets a new index");
    check(d == 2, "differing result arity gets a new index");
}

// --- index spaces ---------------------------------------------------------

static void testIndexSpaces() {
    std::printf("\n-- index spaces --\n");

    {
        Module m;
        const std::uint32_t t = m.addType(FuncType{{}, {}});
        const std::uint32_t i0 = m.importFunction("wasi_snapshot_preview1", "proc_exit", t);
        const std::uint32_t i1 = m.importFunction("wasi_snapshot_preview1", "fd_close", t);
        FunctionBody body;
        const std::uint32_t f0 = m.addFunction(t, body);
        const std::uint32_t f1 = m.addFunction(t, body);

        check(i0 == 0 && i1 == 1, "imported functions take the low indices");
        check(f0 == 2 && f1 == 3, "defined functions follow the imports");
        check(m.functionCount() == 4, "functionCount counts imports and definitions");
        check(m.ok(), "well-ordered module has no error");
    }

    {
        // Declaring an import after a definition would silently shift every
        // index already handed out, so it must be refused.
        Module m;
        const std::uint32_t t = m.addType(FuncType{{}, {}});
        FunctionBody body;
        m.addFunction(t, body);
        m.importFunction("env", "late", t);

        check(!m.ok(), "import after definition is rejected");
        check(m.encode().empty(), "a failed module encodes to nothing");
    }

    {
        Module m;
        const std::uint32_t t = m.addType(FuncType{{}, {}});
        FunctionBody body;
        m.addFunction(t, body);
        m.exportFunction("dup", 0);
        m.exportFunction("dup", 0);
        check(!m.ok(), "duplicate export name is rejected");
    }

    {
        Module m;
        FunctionBody body;
        m.addFunction(7, body);
        check(!m.ok(), "out-of-range type index is rejected");
    }
}

// --- section ordering -----------------------------------------------------

// Exercises every section the writer can emit. Kept linkable against a real
// WASI host (proc_exit has its true (i32)->() signature) so the artifact is
// worth validating and not just decoding.
static Module buildAllSectionsModule() {
    Module m;
    const std::uint32_t voidType = m.addType(FuncType{{}, {}});
    const std::uint32_t exitType = m.addType(FuncType{{ValType::I32}, {}});

    m.importFunction("wasi_snapshot_preview1", "proc_exit", exitType);
    m.addTable(ValType::FuncRef, Limits::range(1, 1));
    m.addMemory(Limits::atLeast(1));
    // Conventional shadow stack pointer: one page in, growing down.
    m.addGlobal(ValType::I32, /*mutableGlobal=*/true, ConstExpr::i32(65536));

    FunctionBody body;
    const std::uint32_t fn = m.addFunction(voidType, std::move(body));
    m.exportFunction("_start", fn);
    m.exportMemory("memory", 0);
    m.setStart(fn);
    m.addActiveElements(0, {fn});
    m.addActiveData(1024, {'h', 'i'});
    return m;
}

static void testSectionOrder() {
    std::printf("\n-- section ordering --\n");

    Module m = buildAllSectionsModule();
    check(m.ok(), "all-sections module built without error");
    const std::vector<std::uint8_t> ids = sectionIds(m.encode());

    // type, import, func, table, mem, global, export, start, elem, datacount,
    // code, data. Note that data count (12) deliberately precedes code (10).
    const std::vector<std::uint8_t> want = {1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 10, 11};
    checkBytes("sections appear in the order the grammar requires", ids, want);
}

// --- empty sections -------------------------------------------------------

static void testEmptySectionsOmitted() {
    std::printf("\n-- empty sections --\n");

    Module m;
    const std::uint32_t t = m.addType(FuncType{{}, {}});
    FunctionBody body;
    m.addFunction(t, body);

    const std::vector<std::uint8_t> ids = sectionIds(m.encode());
    const std::vector<std::uint8_t> want = {1, 3, 10};
    checkBytes("modules carry no empty sections", ids, want);
}

// --- locals run-length encoding ------------------------------------------

static void testLocalsRle() {
    std::printf("\n-- locals RLE --\n");

    Module m;
    const std::uint32_t t = m.addType(FuncType{{}, {}});
    FunctionBody body;
    // i64 i64 i64 f64 i64  ->  runs (3 x i64) (1 x f64) (1 x i64)
    body.locals = {ValType::I64, ValType::I64, ValType::I64, ValType::F64, ValType::I64};
    m.addFunction(t, std::move(body));

    const std::vector<std::uint8_t> mod = m.encode();
    // Locate the code section and read its single entry's locals vector.
    std::size_t i = 8;
    std::vector<std::uint8_t> codeBody;
    while (i < mod.size()) {
        const std::uint8_t id = mod[i++];
        std::uint32_t size = 0;
        unsigned shift = 0;
        while (i < mod.size()) {
            const std::uint8_t byte = mod[i++];
            size |= static_cast<std::uint32_t>(byte & 0x7F) << shift;
            if ((byte & 0x80) == 0) break;
            shift += 7;
        }
        if (id == 10) {
            codeBody.assign(mod.begin() + static_cast<std::ptrdiff_t>(i),
                            mod.begin() + static_cast<std::ptrdiff_t>(i + size));
            break;
        }
        i += size;
    }

    // count=1, entry size, 3 runs, (3,i64) (1,f64) (1,i64), end
    const std::vector<std::uint8_t> want = {
        0x01, 0x08, 0x03, 0x03, 0x7E, 0x01, 0x7C, 0x01, 0x7E, 0x0B,
    };
    checkBytes("consecutive identical locals collapse into runs", codeBody, want);
}

// --- instruction encodings ------------------------------------------------

static void testInstructions() {
    std::printf("\n-- instruction encodings --\n");

    {
        CodeBuilder b;
        b.i32Const(-1);
        checkBytes("i32.const -1 uses signed LEB", b.bytes(), {0x41, 0x7F});
    }
    {
        CodeBuilder b;
        b.i64Const(64);
        checkBytes("i64.const 64", b.bytes(), {0x42, 0xC0, 0x00});
    }
    {
        CodeBuilder b;
        b.f64Const(1.0);
        // 1.0 is 0x3FF0000000000000, little-endian in the instruction stream.
        checkBytes("f64.const 1.0 is 8 raw little-endian bytes", b.bytes(),
                   {0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x3F});
    }
    {
        CodeBuilder b;
        b.block(BlockType::none());
        b.loop(BlockType::of(ValType::I32));
        b.ifThen(BlockType::none());
        b.op(Op::Else);
        b.op(Op::End);
        b.op(Op::End);
        b.op(Op::End);
        checkBytes("block/loop/if signatures", b.bytes(),
                   {0x02, 0x40, 0x03, 0x7F, 0x04, 0x40, 0x05, 0x0B, 0x0B, 0x0B});
    }
    {
        CodeBuilder b;
        b.load(Op::I64Load, /*alignLog2=*/3, /*offset=*/16);
        checkBytes("i64.load carries align and offset", b.bytes(), {0x29, 0x03, 0x10});
    }
    {
        CodeBuilder b;
        b.brTable({0, 1, 2}, 3);
        checkBytes("br_table encodes targets then default", b.bytes(),
                   {0x0E, 0x03, 0x00, 0x01, 0x02, 0x03});
    }
    {
        CodeBuilder b;
        b.memoryCopy();
        b.memoryFill();
        b.memoryGrow();
        checkBytes("bulk memory and memory.grow carry their memory indices",
                   b.bytes(), {0xFC, 0x0A, 0x00, 0x00, 0xFC, 0x0B, 0x00, 0x40, 0x00});
    }
    {
        CodeBuilder b;
        b.opFC(OpFC::I64TruncSatF64S);
        checkBytes("saturating truncation is 0xFC-prefixed", b.bytes(), {0xFC, 0x06});
    }
    {
        CodeBuilder b;
        b.callIndirect(/*typeIndex=*/2, /*tableIndex=*/0);
        checkBytes("call_indirect names signature then table", b.bytes(),
                   {0x11, 0x02, 0x00});
    }
}

// --- runnable artifacts ---------------------------------------------------

// A minimal WASI command: exported memory plus an exported `_start` that
// returns immediately. `wasmtime run` should execute it and exit 0.
static Module buildStartModule() {
    Module m;
    const std::uint32_t voidType = m.addType(FuncType{{}, {}});
    m.addMemory(Limits::atLeast(1));

    FunctionBody body;
    const std::uint32_t fn = m.addFunction(voidType, std::move(body));
    m.exportFunction("_start", fn);
    m.exportMemory("memory", 0);
    return m;
}

static void writeArtifacts(const std::string& base) {
    std::printf("\n-- artifacts --\n");

    std::string err;
    const std::string addPath = base + "_add.wasm";
    Module add = buildAddModule();
    const bool wroteAdd = WasmWriter::write(add, addPath, err);
    check(wroteAdd, "wrote the add module");
    if (!wroteAdd) std::printf("  error: %s\n", err.c_str());

    const std::string startPath = base + "_start.wasm";
    Module start = buildStartModule();
    const bool wroteStart = WasmWriter::write(start, startPath, err);
    check(wroteStart, "wrote the _start command module");
    if (!wroteStart) std::printf("  error: %s\n", err.c_str());

    const std::string allPath = base + "_all.wasm";
    Module all = buildAllSectionsModule();
    const bool wroteAll = WasmWriter::write(all, allPath, err);
    check(wroteAll, "wrote the all-sections module");
    if (!wroteAll) std::printf("  error: %s\n", err.c_str());
}

int main(int argc, char** argv) {
    const std::string base = (argc > 1) ? argv[1] : "wasm_writer_test";

    testLeb();
    testAddModule();
    testTypeDedup();
    testIndexSpaces();
    testSectionOrder();
    testEmptySectionsOmitted();
    testLocalsRle();
    testInstructions();
    writeArtifacts(base);

    std::printf("\n%s (%d failure(s))\n", g_failures == 0 ? "PASSED" : "FAILED",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
