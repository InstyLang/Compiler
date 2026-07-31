#pragma once

// Machine-IR optimization passes for the custom backend.
//
// These run on a freshly selected `MFunction` while it is still in virtual-
// register form (after instruction selection, before register allocation), so
// they reduce both the live-range pressure the allocator sees and the number of
// instructions the lowering ultimately encodes.
//
// Every pass here is deliberately conservative — it preserves observable
// behavior exactly, including:
//   * memory effects (Load/Store/*Ind/Atomic*/Call are never removed),
//   * the implicit flag dependency between Cmp/FCmp and a following SetCC/Jcc
//     (flag-producing instructions are never treated as dead),
//   * two-address semantics (Add/Sub/.../Shl define-and-use operand0),
//   * call/div clobber sets,
//   * fall-through block ordering used by the CFG (a branch is only dropped
//     when its target is the next block).
//
// `optLevel` selects how much work is done:
//   0  -> no optimization (passes are skipped entirely).
//   1+ -> peephole + dead-code elimination + branch simplification, iterated
//         to a fixpoint. Higher levels currently run the same set (room to add
//         more aggressive passes later without changing the call sites).

#include <backend/machine_ir.hpp>

namespace Backend {

// Runs the optimization pipeline on `fn` in place. Safe to call with optLevel 0
// (it returns immediately). Returns the number of instructions removed across
// all passes (useful for tests/diagnostics).
unsigned optimizeFunction(MFunction& fn, int optLevel);

}  // namespace Backend
