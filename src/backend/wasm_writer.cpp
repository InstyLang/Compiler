#include <backend/wasm_writer.hpp>

#include <cstring>
#include <fstream>
#include <utility>

namespace Backend::Wasm {

namespace {

// Section ids. Note that these are *not* emitted in numeric order: the data
// count section (12) sits between the element (9) and code (10) sections. See
// the binary format's module grammar.
constexpr std::uint8_t kSectionType = 1;
constexpr std::uint8_t kSectionImport = 2;
constexpr std::uint8_t kSectionFunction = 3;
constexpr std::uint8_t kSectionTable = 4;
constexpr std::uint8_t kSectionMemory = 5;
constexpr std::uint8_t kSectionGlobal = 6;
constexpr std::uint8_t kSectionExport = 7;
constexpr std::uint8_t kSectionStart = 8;
constexpr std::uint8_t kSectionElement = 9;
constexpr std::uint8_t kSectionCode = 10;
constexpr std::uint8_t kSectionData = 11;
constexpr std::uint8_t kSectionDataCount = 12;

constexpr std::uint8_t kOpEnd = 0x0B;
constexpr std::uint8_t kFuncTypeTag = 0x60;
constexpr std::uint8_t kBlockTypeVoid = 0x40;

void writeByte(std::vector<std::uint8_t>& out, std::uint8_t byte) {
    out.push_back(byte);
}

void writeBytes(std::vector<std::uint8_t>& out, const std::vector<std::uint8_t>& bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

// A name is a byte vector holding UTF-8: length-prefixed, not NUL-terminated.
void writeName(std::vector<std::uint8_t>& out, const std::string& name) {
    leb::writeU32(out, static_cast<std::uint32_t>(name.size()));
    for (char c : name) {
        out.push_back(static_cast<std::uint8_t>(c));
    }
}

void writeValType(std::vector<std::uint8_t>& out, ValType type) {
    out.push_back(static_cast<std::uint8_t>(type));
}

// Block signatures: void is the single byte 0x40, a single result is that value
// type's byte, and anything richer is a type index encoded as an s33 -- signed,
// so a positive index cannot be confused with the 0x40 void marker or with a
// value type byte (all of which have the sign bit set in their low 7 bits).
void writeBlockType(std::vector<std::uint8_t>& out, const BlockType& type) {
    switch (type.kind) {
        case BlockType::Kind::Void:
            out.push_back(kBlockTypeVoid);
            break;
        case BlockType::Kind::Result:
            writeValType(out, type.result);
            break;
        case BlockType::Kind::TypeIndex:
            leb::writeI64(out, static_cast<std::int64_t>(type.typeIndex));
            break;
    }
}

void writeValTypeVector(std::vector<std::uint8_t>& out, const std::vector<ValType>& types) {
    leb::writeU32(out, static_cast<std::uint32_t>(types.size()));
    for (ValType t : types) {
        writeValType(out, t);
    }
}

void writeLimits(std::vector<std::uint8_t>& out, const Limits& limits) {
    out.push_back(limits.hasMax ? 0x01 : 0x00);
    leb::writeU32(out, limits.min);
    if (limits.hasMax) {
        leb::writeU32(out, limits.max);
    }
}

// A constant expression is its instruction bytes followed by `end`.
void writeConstExpr(std::vector<std::uint8_t>& out, const ConstExpr& expr) {
    writeBytes(out, expr.bytes);
    writeByte(out, kOpEnd);
}

// Emits `id` + byte length + body, skipping the section entirely when empty so
// that a module with, say, no globals carries no global section at all.
void emitSection(std::vector<std::uint8_t>& out, std::uint8_t id,
                 const std::vector<std::uint8_t>& body) {
    if (body.empty()) {
        return;
    }
    out.push_back(id);
    leb::writeU32(out, static_cast<std::uint32_t>(body.size()));
    writeBytes(out, body);
}

}  // namespace

// ---------------------------------------------------------------------------
// LEB128
// ---------------------------------------------------------------------------

namespace leb {

void writeU64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    do {
        std::uint8_t byte = static_cast<std::uint8_t>(value & 0x7F);
        value >>= 7;
        if (value != 0) {
            byte |= 0x80;
        }
        out.push_back(byte);
    } while (value != 0);
}

void writeU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    writeU64(out, value);
}

void writeI64(std::vector<std::uint8_t>& out, std::int64_t value) {
    bool more = true;
    while (more) {
        std::uint8_t byte = static_cast<std::uint8_t>(value & 0x7F);
        // Arithmetic shift: sign bits propagate, so negative values converge on
        // -1 rather than 0.
        value >>= 7;
        const bool signBitSet = (byte & 0x40) != 0;
        if ((value == 0 && !signBitSet) || (value == -1 && signBitSet)) {
            more = false;
        } else {
            byte |= 0x80;
        }
        out.push_back(byte);
    }
}

void writeI32(std::vector<std::uint8_t>& out, std::int32_t value) {
    writeI64(out, static_cast<std::int64_t>(value));
}

}  // namespace leb

// ---------------------------------------------------------------------------
// ConstExpr
// ---------------------------------------------------------------------------

ConstExpr ConstExpr::i32(std::int32_t value) {
    ConstExpr e;
    e.bytes.push_back(static_cast<std::uint8_t>(Op::I32Const));
    leb::writeI32(e.bytes, value);
    return e;
}

ConstExpr ConstExpr::i64(std::int64_t value) {
    ConstExpr e;
    e.bytes.push_back(static_cast<std::uint8_t>(Op::I64Const));
    leb::writeI64(e.bytes, value);
    return e;
}

ConstExpr ConstExpr::f32(float value) {
    ConstExpr e;
    e.bytes.push_back(static_cast<std::uint8_t>(Op::F32Const));
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (int i = 0; i < 4; ++i) {
        e.bytes.push_back(static_cast<std::uint8_t>((bits >> (8 * i)) & 0xFF));
    }
    return e;
}

ConstExpr ConstExpr::f64(double value) {
    ConstExpr e;
    e.bytes.push_back(static_cast<std::uint8_t>(Op::F64Const));
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (int i = 0; i < 8; ++i) {
        e.bytes.push_back(static_cast<std::uint8_t>((bits >> (8 * i)) & 0xFF));
    }
    return e;
}

ConstExpr ConstExpr::globalGet(std::uint32_t globalIndex) {
    ConstExpr e;
    e.bytes.push_back(static_cast<std::uint8_t>(Op::GlobalGet));
    leb::writeU32(e.bytes, globalIndex);
    return e;
}

// ---------------------------------------------------------------------------
// CodeBuilder
// ---------------------------------------------------------------------------

void CodeBuilder::op(Op opcode) {
    bytes_.push_back(static_cast<std::uint8_t>(opcode));
}

void CodeBuilder::opFC(OpFC opcode) {
    bytes_.push_back(static_cast<std::uint8_t>(Op::PrefixFC));
    leb::writeU32(bytes_, static_cast<std::uint32_t>(opcode));
}

void CodeBuilder::i32Const(std::int32_t value) {
    op(Op::I32Const);
    leb::writeI32(bytes_, value);
}

void CodeBuilder::i64Const(std::int64_t value) {
    op(Op::I64Const);
    leb::writeI64(bytes_, value);
}

void CodeBuilder::f32Const(float value) {
    op(Op::F32Const);
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (int i = 0; i < 4; ++i) {
        bytes_.push_back(static_cast<std::uint8_t>((bits >> (8 * i)) & 0xFF));
    }
}

void CodeBuilder::f64Const(double value) {
    op(Op::F64Const);
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (int i = 0; i < 8; ++i) {
        bytes_.push_back(static_cast<std::uint8_t>((bits >> (8 * i)) & 0xFF));
    }
}

void CodeBuilder::localGet(std::uint32_t index) {
    op(Op::LocalGet);
    leb::writeU32(bytes_, index);
}

void CodeBuilder::localSet(std::uint32_t index) {
    op(Op::LocalSet);
    leb::writeU32(bytes_, index);
}

void CodeBuilder::localTee(std::uint32_t index) {
    op(Op::LocalTee);
    leb::writeU32(bytes_, index);
}

void CodeBuilder::globalGet(std::uint32_t index) {
    op(Op::GlobalGet);
    leb::writeU32(bytes_, index);
}

void CodeBuilder::globalSet(std::uint32_t index) {
    op(Op::GlobalSet);
    leb::writeU32(bytes_, index);
}

void CodeBuilder::block(BlockType type) {
    op(Op::Block);
    writeBlockType(bytes_, type);
}

void CodeBuilder::loop(BlockType type) {
    op(Op::Loop);
    writeBlockType(bytes_, type);
}

void CodeBuilder::ifThen(BlockType type) {
    op(Op::If);
    writeBlockType(bytes_, type);
}

void CodeBuilder::br(std::uint32_t depth) {
    op(Op::Br);
    leb::writeU32(bytes_, depth);
}

void CodeBuilder::brIf(std::uint32_t depth) {
    op(Op::BrIf);
    leb::writeU32(bytes_, depth);
}

void CodeBuilder::brTable(const std::vector<std::uint32_t>& targets,
                          std::uint32_t defaultTarget) {
    op(Op::BrTable);
    leb::writeU32(bytes_, static_cast<std::uint32_t>(targets.size()));
    for (std::uint32_t t : targets) {
        leb::writeU32(bytes_, t);
    }
    leb::writeU32(bytes_, defaultTarget);
}

void CodeBuilder::call(std::uint32_t funcIndex) {
    op(Op::Call);
    leb::writeU32(bytes_, funcIndex);
}

void CodeBuilder::callIndirect(std::uint32_t typeIndex, std::uint32_t tableIndex) {
    op(Op::CallIndirect);
    leb::writeU32(bytes_, typeIndex);
    leb::writeU32(bytes_, tableIndex);
}

void CodeBuilder::load(Op opcode, std::uint32_t alignLog2, std::uint32_t offset) {
    op(opcode);
    leb::writeU32(bytes_, alignLog2);
    leb::writeU32(bytes_, offset);
}

void CodeBuilder::store(Op opcode, std::uint32_t alignLog2, std::uint32_t offset) {
    load(opcode, alignLog2, offset);
}

void CodeBuilder::memorySize() {
    op(Op::MemorySize);
    bytes_.push_back(0x00);  // memory index
}

void CodeBuilder::memoryGrow() {
    op(Op::MemoryGrow);
    bytes_.push_back(0x00);  // memory index
}

void CodeBuilder::memoryCopy() {
    opFC(OpFC::MemoryCopy);
    bytes_.push_back(0x00);  // destination memory index
    bytes_.push_back(0x00);  // source memory index
}

void CodeBuilder::memoryFill() {
    opFC(OpFC::MemoryFill);
    bytes_.push_back(0x00);  // memory index
}

void CodeBuilder::raw(const std::uint8_t* data, std::size_t size) {
    bytes_.insert(bytes_.end(), data, data + size);
}

void CodeBuilder::rawByte(std::uint8_t byte) {
    bytes_.push_back(byte);
}

// ---------------------------------------------------------------------------
// Module
// ---------------------------------------------------------------------------

void Module::fail(const std::string& message) {
    if (error_.empty()) {
        error_ = message;
    }
}

std::uint32_t Module::importedCount(ExternKind kind) const {
    std::uint32_t n = 0;
    for (const auto& imp : imports_) {
        if (imp.kind == kind) {
            ++n;
        }
    }
    return n;
}

std::uint32_t Module::functionCount() const {
    return importedCount(ExternKind::Function) +
           static_cast<std::uint32_t>(functionTypes_.size());
}

std::uint32_t Module::globalCount() const {
    return importedCount(ExternKind::Global) + static_cast<std::uint32_t>(globals_.size());
}

std::uint32_t Module::memoryCount() const {
    return importedCount(ExternKind::Memory) + static_cast<std::uint32_t>(memories_.size());
}

std::uint32_t Module::tableCount() const {
    return importedCount(ExternKind::Table) + static_cast<std::uint32_t>(tables_.size());
}

std::uint32_t Module::addType(const FuncType& type) {
    for (std::size_t i = 0; i < types_.size(); ++i) {
        if (types_[i] == type) {
            return static_cast<std::uint32_t>(i);
        }
    }
    types_.push_back(type);
    return static_cast<std::uint32_t>(types_.size() - 1);
}

std::uint32_t Module::importFunction(const std::string& module, const std::string& field,
                                     std::uint32_t typeIndex) {
    if (!functionTypes_.empty()) {
        fail("wasm: function import '" + module + "." + field +
             "' declared after a function definition; imports occupy the low "
             "indices of the function index space and must come first");
        return 0;
    }
    if (typeIndex >= types_.size()) {
        fail("wasm: function import '" + module + "." + field +
             "' references an out-of-range type index");
        return 0;
    }
    Import imp;
    imp.module = module;
    imp.field = field;
    imp.kind = ExternKind::Function;
    imp.typeIndex = typeIndex;
    const std::uint32_t index = importedCount(ExternKind::Function);
    imports_.push_back(std::move(imp));
    return index;
}

std::uint32_t Module::importTable(const std::string& module, const std::string& field,
                                  ValType refType, Limits limits) {
    if (!tables_.empty()) {
        fail("wasm: table import '" + module + "." + field +
             "' declared after a table definition");
        return 0;
    }
    Import imp;
    imp.module = module;
    imp.field = field;
    imp.kind = ExternKind::Table;
    imp.valType = refType;
    imp.limits = limits;
    const std::uint32_t index = importedCount(ExternKind::Table);
    imports_.push_back(std::move(imp));
    return index;
}

std::uint32_t Module::importMemory(const std::string& module, const std::string& field,
                                   Limits limits) {
    if (!memories_.empty()) {
        fail("wasm: memory import '" + module + "." + field +
             "' declared after a memory definition");
        return 0;
    }
    Import imp;
    imp.module = module;
    imp.field = field;
    imp.kind = ExternKind::Memory;
    imp.limits = limits;
    const std::uint32_t index = importedCount(ExternKind::Memory);
    imports_.push_back(std::move(imp));
    return index;
}

std::uint32_t Module::importGlobal(const std::string& module, const std::string& field,
                                   ValType type, bool mutableGlobal) {
    if (!globals_.empty()) {
        fail("wasm: global import '" + module + "." + field +
             "' declared after a global definition");
        return 0;
    }
    Import imp;
    imp.module = module;
    imp.field = field;
    imp.kind = ExternKind::Global;
    imp.valType = type;
    imp.mutableGlobal = mutableGlobal;
    const std::uint32_t index = importedCount(ExternKind::Global);
    imports_.push_back(std::move(imp));
    return index;
}

std::uint32_t Module::addFunction(std::uint32_t typeIndex, FunctionBody body) {
    if (typeIndex >= types_.size()) {
        fail("wasm: function definition references an out-of-range type index");
        return 0;
    }
    const std::uint32_t index = functionCount();
    functionTypes_.push_back(typeIndex);
    functionBodies_.push_back(std::move(body));
    return index;
}

std::uint32_t Module::addTable(ValType refType, Limits limits) {
    const std::uint32_t index = tableCount();
    tables_.push_back(Table{refType, limits});
    return index;
}

std::uint32_t Module::addMemory(Limits limits) {
    const std::uint32_t index = memoryCount();
    memories_.push_back(limits);
    return index;
}

std::uint32_t Module::addGlobal(ValType type, bool mutableGlobal, ConstExpr init) {
    const std::uint32_t index = globalCount();
    globals_.push_back(Global{type, mutableGlobal, std::move(init)});
    return index;
}

void Module::addExport(const std::string& name, ExternKind kind, std::uint32_t index) {
    for (const auto& e : exports_) {
        if (e.name == name) {
            fail("wasm: duplicate export name '" + name + "'");
            return;
        }
    }
    exports_.push_back(Export{name, kind, index});
}

void Module::exportFunction(const std::string& name, std::uint32_t funcIndex) {
    addExport(name, ExternKind::Function, funcIndex);
}

void Module::exportMemory(const std::string& name, std::uint32_t memoryIndex) {
    addExport(name, ExternKind::Memory, memoryIndex);
}

void Module::exportGlobal(const std::string& name, std::uint32_t globalIndex) {
    addExport(name, ExternKind::Global, globalIndex);
}

void Module::exportTable(const std::string& name, std::uint32_t tableIndex) {
    addExport(name, ExternKind::Table, tableIndex);
}

void Module::setStart(std::uint32_t funcIndex) {
    hasStart_ = true;
    startFunction_ = funcIndex;
}

void Module::addActiveData(ConstExpr offset, std::vector<std::uint8_t> bytes) {
    dataSegments_.push_back(DataSegment{std::move(offset), std::move(bytes)});
}

void Module::addActiveData(std::uint32_t offset, std::vector<std::uint8_t> bytes) {
    addActiveData(ConstExpr::i32(static_cast<std::int32_t>(offset)), std::move(bytes));
}

void Module::addActiveElements(std::uint32_t offset, std::vector<std::uint32_t> funcIndices) {
    elementSegments_.push_back(ElementSegment{offset, std::move(funcIndices)});
}

std::vector<std::uint8_t> Module::encode() const {
    std::vector<std::uint8_t> out;
    if (!ok()) {
        return out;
    }

    // Preamble: the magic "\0asm" and format version 1.
    out.push_back(0x00);
    out.push_back(0x61);
    out.push_back(0x73);
    out.push_back(0x6D);
    out.push_back(0x01);
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0x00);

    std::vector<std::uint8_t> body;

    // 1: types
    body.clear();
    if (!types_.empty()) {
        leb::writeU32(body, static_cast<std::uint32_t>(types_.size()));
        for (const auto& t : types_) {
            writeByte(body, kFuncTypeTag);
            writeValTypeVector(body, t.params);
            writeValTypeVector(body, t.results);
        }
        emitSection(out, kSectionType, body);
    }

    // 2: imports
    body.clear();
    if (!imports_.empty()) {
        leb::writeU32(body, static_cast<std::uint32_t>(imports_.size()));
        for (const auto& imp : imports_) {
            writeName(body, imp.module);
            writeName(body, imp.field);
            writeByte(body, static_cast<std::uint8_t>(imp.kind));
            switch (imp.kind) {
                case ExternKind::Function:
                    leb::writeU32(body, imp.typeIndex);
                    break;
                case ExternKind::Table:
                    writeValType(body, imp.valType);
                    writeLimits(body, imp.limits);
                    break;
                case ExternKind::Memory:
                    writeLimits(body, imp.limits);
                    break;
                case ExternKind::Global:
                    writeValType(body, imp.valType);
                    writeByte(body, imp.mutableGlobal ? 0x01 : 0x00);
                    break;
            }
        }
        emitSection(out, kSectionImport, body);
    }

    // 3: function declarations (signatures only; bodies live in the code section)
    body.clear();
    if (!functionTypes_.empty()) {
        leb::writeU32(body, static_cast<std::uint32_t>(functionTypes_.size()));
        for (std::uint32_t t : functionTypes_) {
            leb::writeU32(body, t);
        }
        emitSection(out, kSectionFunction, body);
    }

    // 4: tables
    body.clear();
    if (!tables_.empty()) {
        leb::writeU32(body, static_cast<std::uint32_t>(tables_.size()));
        for (const auto& t : tables_) {
            writeValType(body, t.refType);
            writeLimits(body, t.limits);
        }
        emitSection(out, kSectionTable, body);
    }

    // 5: memories
    body.clear();
    if (!memories_.empty()) {
        leb::writeU32(body, static_cast<std::uint32_t>(memories_.size()));
        for (const auto& m : memories_) {
            writeLimits(body, m);
        }
        emitSection(out, kSectionMemory, body);
    }

    // 6: globals
    body.clear();
    if (!globals_.empty()) {
        leb::writeU32(body, static_cast<std::uint32_t>(globals_.size()));
        for (const auto& g : globals_) {
            writeValType(body, g.type);
            writeByte(body, g.mutableGlobal ? 0x01 : 0x00);
            writeConstExpr(body, g.init);
        }
        emitSection(out, kSectionGlobal, body);
    }

    // 7: exports
    body.clear();
    if (!exports_.empty()) {
        leb::writeU32(body, static_cast<std::uint32_t>(exports_.size()));
        for (const auto& e : exports_) {
            writeName(body, e.name);
            writeByte(body, static_cast<std::uint8_t>(e.kind));
            leb::writeU32(body, e.index);
        }
        emitSection(out, kSectionExport, body);
    }

    // 8: start. Not a vector, so it is emitted directly rather than via
    // emitSection's empty-body check.
    if (hasStart_) {
        body.clear();
        leb::writeU32(body, startFunction_);
        emitSection(out, kSectionStart, body);
    }

    // 9: elements (active, table 0, function references)
    body.clear();
    if (!elementSegments_.empty()) {
        leb::writeU32(body, static_cast<std::uint32_t>(elementSegments_.size()));
        for (const auto& seg : elementSegments_) {
            writeByte(body, 0x00);  // active, table 0, funcidx payload
            writeConstExpr(body, ConstExpr::i32(static_cast<std::int32_t>(seg.offset)));
            leb::writeU32(body, static_cast<std::uint32_t>(seg.funcIndices.size()));
            for (std::uint32_t f : seg.funcIndices) {
                leb::writeU32(body, f);
            }
        }
        emitSection(out, kSectionElement, body);
    }

    // 12: data count -- must precede the code section so that a validator can
    // check memory.init/data.drop immediates in a single pass. Emitting it
    // whenever data segments exist is always valid.
    if (!dataSegments_.empty()) {
        body.clear();
        leb::writeU32(body, static_cast<std::uint32_t>(dataSegments_.size()));
        emitSection(out, kSectionDataCount, body);
    }

    // 10: code
    body.clear();
    if (!functionBodies_.empty()) {
        leb::writeU32(body, static_cast<std::uint32_t>(functionBodies_.size()));
        for (const auto& fn : functionBodies_) {
            // Build the entry first: its byte length prefixes it.
            std::vector<std::uint8_t> entry;

            // Locals are run-length encoded as (count, type) pairs.
            std::vector<std::pair<std::uint32_t, ValType>> runs;
            for (ValType v : fn.locals) {
                if (!runs.empty() && runs.back().second == v) {
                    ++runs.back().first;
                } else {
                    runs.push_back({1, v});
                }
            }
            leb::writeU32(entry, static_cast<std::uint32_t>(runs.size()));
            for (const auto& run : runs) {
                leb::writeU32(entry, run.first);
                writeValType(entry, run.second);
            }

            writeBytes(entry, fn.code);
            writeByte(entry, kOpEnd);  // the body's terminating `end`

            leb::writeU32(body, static_cast<std::uint32_t>(entry.size()));
            writeBytes(body, entry);
        }
        emitSection(out, kSectionCode, body);
    }

    // 11: data (active, memory 0)
    body.clear();
    if (!dataSegments_.empty()) {
        leb::writeU32(body, static_cast<std::uint32_t>(dataSegments_.size()));
        for (const auto& seg : dataSegments_) {
            writeByte(body, 0x00);  // active, memory 0
            writeConstExpr(body, seg.offset);
            leb::writeU32(body, static_cast<std::uint32_t>(seg.bytes.size()));
            writeBytes(body, seg.bytes);
        }
        emitSection(out, kSectionData, body);
    }

    return out;
}

// ---------------------------------------------------------------------------
// WasmWriter
// ---------------------------------------------------------------------------

bool WasmWriter::write(const Module& module, const std::string& path,
                       std::string& errorOut) {
    if (!module.ok()) {
        errorOut = module.error();
        return false;
    }
    const std::vector<std::uint8_t> bytes = module.encode();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        errorOut = "wasm: cannot open '" + path + "' for writing";
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        errorOut = "wasm: failed writing '" + path + "'";
        return false;
    }
    return true;
}

}  // namespace Backend::Wasm
