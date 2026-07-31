#pragma once

// Raw inline-assembly blocks: `asm [keep(rax)]( ... NASM text ... )`.
//
// The backend has no text assembler, so this module is one: it parses a NASM
// subset into instructions and encodes them through the vendored Fadec encoder
// (which covers the x86-64 ISA -- the limit here is this file's dispatch table,
// not the encoder).
//
// Parsing and encoding are separate on purpose. A `$var` operand refers to an
// Insty local, whose frame offset is not known until the frame is laid out in
// lower.cpp; so isel parses (resolving each `$var` to a frame-slot index) and
// lower.cpp rewrites those slots to `[rbp - off]` and encodes.

#include <cstdint>
#include <string>
#include <vector>

namespace Backend {

enum class AsmOpKind {
    None,
    Reg,   // a general-purpose register
    Imm,   // an integer literal
    Mem,   // [base + index*scale + disp]
    Slot,  // `$var`: a local's frame slot, rewritten to Mem before encoding
    Label, // a branch target defined by a `name:` line in the same block
};

// Conditional/unconditional branches whose target is a label. These are emitted
// by the lowering pass rather than by encodeAsmInst, because a forward branch has
// to be back-patched once the label's offset is known -- the same mechanism the
// compiler already uses for its own jumps.
enum class AsmBranch {
    None, Jmp, Je, Jne, Jl, Jle, Jg, Jge, Jb, Jbe, Ja, Jae,
};

// The branch kind a mnemonic denotes, or None. Accepts the usual synonyms
// (je/jz, jne/jnz, jb/jnae/jc, ...).
AsmBranch asmBranchOf(const std::string& mnemonic);

struct AsmOperand {
    AsmOpKind kind = AsmOpKind::None;
    int reg = -1;             // Reg: GP index 0..15. Mem: base register (-1 = none)
    unsigned width = 0;       // Reg: 1/2/4/8 bytes. Mem: size hint, 0 if unstated
    long long imm = 0;        // Imm: the value. Mem: the displacement
    int index = -1;           // Mem: index register, -1 for none
    unsigned scale = 0;       // Mem: SIB scale (0 when there is no index)
    std::uint32_t slot = 0;   // Slot: frame-slot index
    std::string var;          // Slot: the source name, for diagnostics
};

struct AsmInst {
    // A label defined at this point, from a `name:` line. When the line carried
    // nothing else `mnemonic` is empty and the entry exists only to mark the spot.
    std::string labelDef;
    std::string mnemonic;         // lower-case
    std::vector<AsmOperand> ops;  // NASM order: destination first
    int line = 1;                 // 1-based line within the block, for errors
};

struct AsmProgram {
    std::vector<AsmInst> insts;
    // Registers named by `keep(reg)`. These are reserved for the block: the
    // allocator keeps nothing live in them across it.
    std::vector<int> keepRegs;
    // `$var` names in first-seen order, so isel can resolve them to slots.
    std::vector<std::string> varRefs;
};

// Maps a register name ("rax", "eax", "ax", "al", "r8", "r8d", ...) to its GP
// index and width in bytes. False for names this module does not model
// (segment, x87, SSE, and the legacy high-byte registers ah/ch/dh/bh).
bool asmRegByName(const std::string& name, int& idx, unsigned& width);

// Parses a block body. On failure `err` carries a message naming the line.
bool parseAsmBlock(const std::string& text, AsmProgram& out, std::string& err);

// Encodes one instruction into `buf` (which must have room for 16 bytes).
// Every Slot operand must already have been rewritten to Mem. Returns the byte
// count, or -1 with `err` set when the mnemonic or operand shape is not in the
// dispatch table.
int encodeAsmInst(const AsmInst& inst, std::uint8_t* buf, std::string& err);

}  // namespace Backend

