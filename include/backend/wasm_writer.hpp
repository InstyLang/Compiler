#pragma once

// WebAssembly binary-format module builder and writer.
//
// Sibling to ElfWriter / CoffWriter / PeWriter, but with a different shape: a
// wasm module is self-contained (no relocations, no external linker), so this
// file both *builds* the module and *serializes* it. There is no MachineCode
// intermediate -- the wasm code generator drives the Module builder directly.
//
// Nothing here knows about Insty, Machine IR or x86: this is a standalone
// encoder for the binary format described by the WebAssembly core spec
// (https://webassembly.github.io/spec/core/binary/index.html), and is unit
// tested on its own.
//
// Index spaces
// ------------
// wasm places imported entities *before* defined ones in each index space
// (functions, tables, memories, globals). The builder therefore requires every
// import of a given kind to be declared before the first definition of that
// kind; violating this records an error rather than silently producing a module
// whose indices are all shifted. Indices returned by the add*/import* methods
// are final.
//
// Error handling
// --------------
// Builder methods do not throw or return status codes. They record the first
// problem on the module; ok()/error() report it and encode() refuses to produce
// bytes. This keeps long build sequences readable.

#include <cstdint>
#include <string>
#include <vector>

namespace Backend::Wasm {

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

// Value types, using their binary encodings. V128 requires the SIMD proposal;
// FuncRef/ExternRef the reference-types proposal.
enum class ValType : std::uint8_t {
    I32 = 0x7F,
    I64 = 0x7E,
    F32 = 0x7D,
    F64 = 0x7C,
    V128 = 0x7B,
    FuncRef = 0x70,
    ExternRef = 0x6F,
};

// A function signature. wasm allows multiple results (multi-value proposal);
// the common case is zero or one.
struct FuncType {
    std::vector<ValType> params;
    std::vector<ValType> results;

    bool operator==(const FuncType& other) const {
        return params == other.params && results == other.results;
    }
};

// Resizable limits, shared by memory and table types. Memory sizes are in
// 64 KiB pages.
struct Limits {
    std::uint32_t min = 0;
    std::uint32_t max = 0;
    bool hasMax = false;

    static Limits atLeast(std::uint32_t pages) {
        Limits l;
        l.min = pages;
        return l;
    }
    static Limits range(std::uint32_t minPages, std::uint32_t maxPages) {
        Limits l;
        l.min = minPages;
        l.max = maxPages;
        l.hasMax = true;
        return l;
    }
};

// The signature of a block/loop/if. Void is the overwhelmingly common case and
// encodes as the single byte 0x40; a single result encodes as that value type;
// anything richer needs a type index (multi-value proposal) encoded as s33.
struct BlockType {
    enum class Kind : std::uint8_t { Void, Result, TypeIndex };

    Kind kind = Kind::Void;
    ValType result = ValType::I32;
    std::uint32_t typeIndex = 0;

    static BlockType none() { return {}; }
    static BlockType of(ValType v) {
        BlockType b;
        b.kind = Kind::Result;
        b.result = v;
        return b;
    }
    static BlockType index(std::uint32_t i) {
        BlockType b;
        b.kind = Kind::TypeIndex;
        b.typeIndex = i;
        return b;
    }
};

// ---------------------------------------------------------------------------
// Opcodes
// ---------------------------------------------------------------------------

// Single-byte opcodes. Instructions carrying immediates (constants, indices,
// memargs, block types) have dedicated CodeBuilder methods; the rest are
// emitted with CodeBuilder::op(). Values are the binary encodings.
enum class Op : std::uint8_t {
    // control
    Unreachable = 0x00,
    Nop = 0x01,
    Block = 0x02,
    Loop = 0x03,
    If = 0x04,
    Else = 0x05,
    End = 0x0B,
    Br = 0x0C,
    BrIf = 0x0D,
    BrTable = 0x0E,
    Return = 0x0F,
    Call = 0x10,
    CallIndirect = 0x11,

    // parametric
    Drop = 0x1A,
    Select = 0x1B,

    // variables
    LocalGet = 0x20,
    LocalSet = 0x21,
    LocalTee = 0x22,
    GlobalGet = 0x23,
    GlobalSet = 0x24,

    // memory loads / stores (all take a memarg)
    I32Load = 0x28,
    I64Load = 0x29,
    F32Load = 0x2A,
    F64Load = 0x2B,
    I32Load8S = 0x2C,
    I32Load8U = 0x2D,
    I32Load16S = 0x2E,
    I32Load16U = 0x2F,
    I64Load8S = 0x30,
    I64Load8U = 0x31,
    I64Load16S = 0x32,
    I64Load16U = 0x33,
    I64Load32S = 0x34,
    I64Load32U = 0x35,
    I32Store = 0x36,
    I64Store = 0x37,
    F32Store = 0x38,
    F64Store = 0x39,
    I32Store8 = 0x3A,
    I32Store16 = 0x3B,
    I64Store8 = 0x3C,
    I64Store16 = 0x3D,
    I64Store32 = 0x3E,
    MemorySize = 0x3F,
    MemoryGrow = 0x40,

    // constants
    I32Const = 0x41,
    I64Const = 0x42,
    F32Const = 0x43,
    F64Const = 0x44,

    // i32 comparison
    I32Eqz = 0x45,
    I32Eq = 0x46,
    I32Ne = 0x47,
    I32LtS = 0x48,
    I32LtU = 0x49,
    I32GtS = 0x4A,
    I32GtU = 0x4B,
    I32LeS = 0x4C,
    I32LeU = 0x4D,
    I32GeS = 0x4E,
    I32GeU = 0x4F,

    // i64 comparison
    I64Eqz = 0x50,
    I64Eq = 0x51,
    I64Ne = 0x52,
    I64LtS = 0x53,
    I64LtU = 0x54,
    I64GtS = 0x55,
    I64GtU = 0x56,
    I64LeS = 0x57,
    I64LeU = 0x58,
    I64GeS = 0x59,
    I64GeU = 0x5A,

    // f32 / f64 comparison
    F32Eq = 0x5B,
    F32Ne = 0x5C,
    F32Lt = 0x5D,
    F32Gt = 0x5E,
    F32Le = 0x5F,
    F32Ge = 0x60,
    F64Eq = 0x61,
    F64Ne = 0x62,
    F64Lt = 0x63,
    F64Gt = 0x64,
    F64Le = 0x65,
    F64Ge = 0x66,

    // i32 arithmetic
    I32Clz = 0x67,
    I32Ctz = 0x68,
    I32Popcnt = 0x69,
    I32Add = 0x6A,
    I32Sub = 0x6B,
    I32Mul = 0x6C,
    I32DivS = 0x6D,
    I32DivU = 0x6E,
    I32RemS = 0x6F,
    I32RemU = 0x70,
    I32And = 0x71,
    I32Or = 0x72,
    I32Xor = 0x73,
    I32Shl = 0x74,
    I32ShrS = 0x75,
    I32ShrU = 0x76,
    I32Rotl = 0x77,
    I32Rotr = 0x78,

    // i64 arithmetic
    I64Clz = 0x79,
    I64Ctz = 0x7A,
    I64Popcnt = 0x7B,
    I64Add = 0x7C,
    I64Sub = 0x7D,
    I64Mul = 0x7E,
    I64DivS = 0x7F,
    I64DivU = 0x80,
    I64RemS = 0x81,
    I64RemU = 0x82,
    I64And = 0x83,
    I64Or = 0x84,
    I64Xor = 0x85,
    I64Shl = 0x86,
    I64ShrS = 0x87,
    I64ShrU = 0x88,
    I64Rotl = 0x89,
    I64Rotr = 0x8A,

    // f32 arithmetic
    F32Abs = 0x8B,
    F32Neg = 0x8C,
    F32Ceil = 0x8D,
    F32Floor = 0x8E,
    F32Trunc = 0x8F,
    F32Nearest = 0x90,
    F32Sqrt = 0x91,
    F32Add = 0x92,
    F32Sub = 0x93,
    F32Mul = 0x94,
    F32Div = 0x95,
    F32Min = 0x96,
    F32Max = 0x97,
    F32Copysign = 0x98,

    // f64 arithmetic
    F64Abs = 0x99,
    F64Neg = 0x9A,
    F64Ceil = 0x9B,
    F64Floor = 0x9C,
    F64Trunc = 0x9D,
    F64Nearest = 0x9E,
    F64Sqrt = 0x9F,
    F64Add = 0xA0,
    F64Sub = 0xA1,
    F64Mul = 0xA2,
    F64Div = 0xA3,
    F64Min = 0xA4,
    F64Max = 0xA5,
    F64Copysign = 0xA6,

    // conversions
    I32WrapI64 = 0xA7,
    I32TruncF32S = 0xA8,
    I32TruncF32U = 0xA9,
    I32TruncF64S = 0xAA,
    I32TruncF64U = 0xAB,
    I64ExtendI32S = 0xAC,
    I64ExtendI32U = 0xAD,
    I64TruncF32S = 0xAE,
    I64TruncF32U = 0xAF,
    I64TruncF64S = 0xB0,
    I64TruncF64U = 0xB1,
    F32ConvertI32S = 0xB2,
    F32ConvertI32U = 0xB3,
    F32ConvertI64S = 0xB4,
    F32ConvertI64U = 0xB5,
    F32DemoteF64 = 0xB6,
    F64ConvertI32S = 0xB7,
    F64ConvertI32U = 0xB8,
    F64ConvertI64S = 0xB9,
    F64ConvertI64U = 0xBA,
    F64PromoteF32 = 0xBB,
    I32ReinterpretF32 = 0xBC,
    I64ReinterpretF64 = 0xBD,
    F32ReinterpretI32 = 0xBE,
    F64ReinterpretI64 = 0xBF,

    // sign extension (sign-extension-ops proposal)
    I32Extend8S = 0xC0,
    I32Extend16S = 0xC1,
    I64Extend8S = 0xC2,
    I64Extend16S = 0xC3,
    I64Extend32S = 0xC4,

    // prefix for the 0xFC opcode space (see OpFC)
    PrefixFC = 0xFC,
};

// Opcodes in the 0xFC prefix space, identified by a ULEB sub-opcode.
//
// The saturating truncations matter for a C-like source language: the plain
// 0xA8-0xB1 truncations *trap* on NaN/out-of-range, whereas x86 cvttsd2si
// produces the "integer indefinite" value. Saturating forms are the closer
// match and never trap.
enum class OpFC : std::uint32_t {
    I32TruncSatF32S = 0,
    I32TruncSatF32U = 1,
    I32TruncSatF64S = 2,
    I32TruncSatF64U = 3,
    I64TruncSatF32S = 4,
    I64TruncSatF32U = 5,
    I64TruncSatF64S = 6,
    I64TruncSatF64U = 7,

    // bulk memory proposal
    MemoryInit = 8,
    DataDrop = 9,
    MemoryCopy = 10,
    MemoryFill = 11,
    TableInit = 12,
    ElemDrop = 13,
    TableCopy = 14,
};

// ---------------------------------------------------------------------------
// Constant expressions
// ---------------------------------------------------------------------------

// A constant initializer, used for global initializers and active data/element
// segment offsets. wasm restricts these to a handful of instruction forms.
// Stored pre-encoded, without the terminating `end`.
struct ConstExpr {
    std::vector<std::uint8_t> bytes;

    static ConstExpr i32(std::int32_t value);
    static ConstExpr i64(std::int64_t value);
    static ConstExpr f32(float value);
    static ConstExpr f64(double value);
    static ConstExpr globalGet(std::uint32_t globalIndex);
};

// ---------------------------------------------------------------------------
// Code
// ---------------------------------------------------------------------------

// Accumulates the instruction bytes of one function body.
//
// The builder is a thin encoder, not a validator: it does not track the operand
// stack or verify that branch depths are in range. Validation is the job of
// `wasm-tools validate` / the embedding engine.
class CodeBuilder {
public:
    // Zero-immediate instructions (arithmetic, comparison, conversions, `end`,
    // `return`, `drop`, ...).
    void op(Op opcode);

    // 0xFC-prefixed instructions that take no further immediates.
    void opFC(OpFC opcode);

    // Constants.
    void i32Const(std::int32_t value);
    void i64Const(std::int64_t value);
    void f32Const(float value);
    void f64Const(double value);

    // Locals and globals.
    void localGet(std::uint32_t index);
    void localSet(std::uint32_t index);
    void localTee(std::uint32_t index);
    void globalGet(std::uint32_t index);
    void globalSet(std::uint32_t index);

    // Structured control flow. Every block/loop/if must be closed with
    // op(Op::End); `if` may be split by op(Op::Else).
    void block(BlockType type = BlockType::none());
    void loop(BlockType type = BlockType::none());
    void ifThen(BlockType type = BlockType::none());

    // Branches. `depth` is relative: 0 is the innermost enclosing block.
    void br(std::uint32_t depth);
    void brIf(std::uint32_t depth);
    void brTable(const std::vector<std::uint32_t>& targets, std::uint32_t defaultTarget);

    // Calls. `call_indirect` names the expected signature and the table.
    void call(std::uint32_t funcIndex);
    void callIndirect(std::uint32_t typeIndex, std::uint32_t tableIndex = 0);

    // Loads and stores. `alignLog2` is the base-2 log of the guaranteed
    // alignment (0 => 1 byte, 3 => 8 bytes) and is only a hint; `offset` is a
    // static byte displacement folded into the address.
    void load(Op opcode, std::uint32_t alignLog2, std::uint32_t offset);
    void store(Op opcode, std::uint32_t alignLog2, std::uint32_t offset);

    // Linear-memory intrinsics. memory.size/grow operate in 64 KiB pages.
    void memorySize();
    void memoryGrow();
    void memoryCopy();
    void memoryFill();

    // Escape hatch for encodings not modelled above.
    void raw(const std::uint8_t* data, std::size_t size);
    void rawByte(std::uint8_t byte);

    const std::vector<std::uint8_t>& bytes() const { return bytes_; }
    std::vector<std::uint8_t> take() { return std::move(bytes_); }
    bool empty() const { return bytes_.empty(); }
    std::size_t size() const { return bytes_.size(); }

private:
    std::vector<std::uint8_t> bytes_;
};

// One function body: its extra locals plus its code.
struct FunctionBody {
    // Locals *beyond* the parameters. Parameters already occupy local indices
    // 0..params-1, so the first entry here is local index `params`. The encoder
    // run-length compresses consecutive identical types, as the format expects.
    std::vector<ValType> locals;

    // Instruction bytes, *without* the function's terminating `end` (0x0B) --
    // the encoder appends it, so a body can never be accidentally unterminated.
    // Nested block/loop/if ends are the caller's responsibility.
    std::vector<std::uint8_t> code;
};

// ---------------------------------------------------------------------------
// Module
// ---------------------------------------------------------------------------

// What an export or import refers to. Values are the binary encodings.
enum class ExternKind : std::uint8_t {
    Function = 0x00,
    Table = 0x01,
    Memory = 0x02,
    Global = 0x03,
};

class Module {
public:
    // --- types -------------------------------------------------------------
    // Interns `type`, returning its index. Structurally identical signatures
    // share one entry.
    std::uint32_t addType(const FuncType& type);

    // --- imports -----------------------------------------------------------
    // All imports of a kind must be declared before the first definition of
    // that kind; otherwise an error is recorded.
    std::uint32_t importFunction(const std::string& module, const std::string& field,
                                 std::uint32_t typeIndex);
    std::uint32_t importTable(const std::string& module, const std::string& field,
                              ValType refType, Limits limits);
    std::uint32_t importMemory(const std::string& module, const std::string& field,
                               Limits limits);
    std::uint32_t importGlobal(const std::string& module, const std::string& field,
                               ValType type, bool mutableGlobal);

    // --- definitions -------------------------------------------------------
    std::uint32_t addFunction(std::uint32_t typeIndex, FunctionBody body);
    std::uint32_t addTable(ValType refType, Limits limits);
    std::uint32_t addMemory(Limits limits);
    std::uint32_t addGlobal(ValType type, bool mutableGlobal, ConstExpr init);

    // --- exports / start ---------------------------------------------------
    void addExport(const std::string& name, ExternKind kind, std::uint32_t index);
    void exportFunction(const std::string& name, std::uint32_t funcIndex);
    void exportMemory(const std::string& name, std::uint32_t memoryIndex);
    void exportGlobal(const std::string& name, std::uint32_t globalIndex);
    void exportTable(const std::string& name, std::uint32_t tableIndex);

    // The `start` function runs on instantiation. Note that a WASI *command*
    // uses an exported `_start` instead, which is not this.
    void setStart(std::uint32_t funcIndex);

    // --- segments ----------------------------------------------------------
    // Active data segment copied into memory 0 at `offset` on instantiation.
    void addActiveData(ConstExpr offset, std::vector<std::uint8_t> bytes);
    void addActiveData(std::uint32_t offset, std::vector<std::uint8_t> bytes);

    // Active element segment filling table 0 from `offset` with function
    // references -- the backing store for `call_indirect`.
    void addActiveElements(std::uint32_t offset, std::vector<std::uint32_t> funcIndices);

    // --- output ------------------------------------------------------------
    bool ok() const { return error_.empty(); }
    const std::string& error() const { return error_; }

    // Serializes the module. Returns an empty vector if an error was recorded.
    std::vector<std::uint8_t> encode() const;

    // Index-space sizes, counting imports.
    std::uint32_t functionCount() const;
    std::uint32_t globalCount() const;
    std::uint32_t memoryCount() const;
    std::uint32_t tableCount() const;

private:
    struct Import {
        std::string module;
        std::string field;
        ExternKind kind = ExternKind::Function;
        std::uint32_t typeIndex = 0;   // Function
        ValType valType = ValType::I32;  // Global / Table element type
        bool mutableGlobal = false;    // Global
        Limits limits;                 // Table / Memory
    };
    struct Global {
        ValType type = ValType::I32;
        bool mutableGlobal = false;
        ConstExpr init;
    };
    struct Table {
        ValType refType = ValType::FuncRef;
        Limits limits;
    };
    struct Export {
        std::string name;
        ExternKind kind = ExternKind::Function;
        std::uint32_t index = 0;
    };
    struct DataSegment {
        ConstExpr offset;
        std::vector<std::uint8_t> bytes;
    };
    struct ElementSegment {
        std::uint32_t offset = 0;
        std::vector<std::uint32_t> funcIndices;
    };

    void fail(const std::string& message);
    std::uint32_t importedCount(ExternKind kind) const;

    std::vector<FuncType> types_;
    std::vector<Import> imports_;
    std::vector<std::uint32_t> functionTypes_;  // defined functions
    std::vector<FunctionBody> functionBodies_;  // parallel to functionTypes_
    std::vector<Table> tables_;
    std::vector<Limits> memories_;
    std::vector<Global> globals_;
    std::vector<Export> exports_;
    std::vector<DataSegment> dataSegments_;
    std::vector<ElementSegment> elementSegments_;
    bool hasStart_ = false;
    std::uint32_t startFunction_ = 0;
    std::string error_;
};

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

class WasmWriter {
public:
    // Serializes `module` to a `.wasm` file at `path`. Returns false and sets
    // `errorOut` if the module recorded a build error or the write failed.
    static bool write(const Module& module, const std::string& path,
                      std::string& errorOut);
};

// ---------------------------------------------------------------------------
// Primitives (exposed for unit tests)
// ---------------------------------------------------------------------------

namespace leb {

// Unsigned LEB128.
void writeU32(std::vector<std::uint8_t>& out, std::uint32_t value);
void writeU64(std::vector<std::uint8_t>& out, std::uint64_t value);

// Signed LEB128. Note that wasm encodes i32.const/i64.const operands as
// *signed* LEB, so -1 is a single byte (0x7F), not five.
void writeI32(std::vector<std::uint8_t>& out, std::int32_t value);
void writeI64(std::vector<std::uint8_t>& out, std::int64_t value);

}  // namespace leb

}  // namespace Backend::Wasm
