#pragma once

// Frame layout + prologue/epilogue + lowering of allocated Machine IR to
// machine code via the Fadec Encoder.
//
// Pipeline position:  instruction selection -> [regalloc] -> Lowering -> bytes.
//
// Lowering assumes the register allocator has already run (every VirtReg
// operand has a `phys` assignment, or a spill slot recorded in the Allocation).
// It lays out the stack frame, emits a standard RBP-based prologue, lowers each
// instruction (reloading spilled uses and storing spilled defs around it), and
// emits a matching epilogue at each Ret.

#include <backend/abi.hpp>
#include <backend/encoder.hpp>
#include <backend/machine_code.hpp>
#include <backend/machine_ir.hpp>
#include <backend/regalloc.hpp>
#include <backend/reg.hpp>

namespace Backend {

class Lowering {
public:
    Lowering(MachineCode& code, const AbiInfo& abi) : enc_(code), abi_(abi) {}

    // Computes the frame layout for `fn` (slot offsets, aligned frame size, the
    // callee-saved registers actually used). Safe to call before emit(); emit()
    // calls it if not already done.
    void layoutFrame(MFunction& fn, const Allocation& alloc);

    // Lowers the whole function: defines its symbol, emits prologue, body, and
    // epilogue. Returns false (and sets errorOut) on an unsupported construct.
    bool emit(MFunction& fn, const Allocation& alloc, std::string& errorOut);

    Encoder& encoder() { return enc_; }

private:
    Encoder enc_;
    AbiInfo abi_;

    // Branch backpatching state (valid during emit()).
    std::vector<std::uint64_t> blockStart_;  // text offset of each block's start
    struct BranchFixup {
        std::uint64_t dispOffset;  // text offset of the rel32 field
        std::uint32_t targetBlock; // block index to jump to
    };
    std::vector<BranchFixup> fixups_;

    void emitPrologue(MFunction& fn);
    void emitEpilogue(MFunction& fn);
    bool emitInst(MFunction& fn, const Allocation& alloc, const MInst& inst,
                  std::string& errorOut);
    void resolveBranches();
};

}  // namespace Backend
