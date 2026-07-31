#pragma once

// Linear-scan register allocator for the Machine IR.
//
// Algorithm (Poletto & Sarkar style):
//   1. Linearize all instructions across blocks into a single numbered list,
//      recording each block's program-point range.
//   2. Build the CFG (successors from each block's terminator: Jmp/Jcc/Ret/
//      fall-through) and solve backward dataflow for liveIn/liveOut per block
//      to a fixpoint. Convert to a live interval [start,end] per virtual
//      register: a value is live across the full extent of any block where it
//      is live-in/live-out (so loop-carried values stay live across back-edges)
//      plus every actual def/use point. This is correct for arbitrary control
//      flow (if/else, loops), not just straight-line code.
//   3. Walk intervals by increasing start point, keeping an "active" set sorted
//      by end point. Assign a free physical register from the ABI's allocatable
//      pool; when none is free, spill the interval that ends latest.
//   4. Rewrite every VirtReg operand's `phys` field with its assignment, and
//      mark spilled vregs so lowering inserts Load/Store around their uses/defs.
//
// Physical-register operands (ABI constraints, call clobbers) are respected:
// a physreg in use across an interval is removed from the free pool for that
// span. This is intentionally simple; it is a foundation to grow, not a
// production allocator.

#include <unordered_map>
#include <vector>

#include <backend/abi.hpp>
#include <backend/machine_ir.hpp>
#include <backend/reg.hpp>

namespace Backend {

struct LiveInterval {
    VReg vreg = kInvalidVReg;
    int start = 0;  // first program point where vreg is defined
    int end = 0;    // last program point where vreg is used
};

struct Allocation {
    // For each vreg: the assigned physical register, or None if spilled.
    // GP-class vregs use vregToPhys; XMM-class vregs use vregToXmm.
    std::vector<PhysReg> vregToPhys;
    std::vector<XmmReg> vregToXmm;
    // For each spilled vreg: its frame slot index (else kInvalidVReg sentinel).
    std::vector<std::uint32_t> vregToSlot;
    bool anySpilled = false;
};

class LinearScanAllocator {
public:
    explicit LinearScanAllocator(const AbiInfo& abi) : abi_(abi) {}

    // Computes live intervals, assigns registers (spilling as needed), and
    // writes assignments back into every VirtReg operand's `phys` field in the
    // function. Returns the Allocation for inspection/testing.
    Allocation run(MFunction& fn);

    // Exposed for unit testing: compute intervals without mutating the function.
    std::vector<LiveInterval> computeIntervals(const MFunction& fn) const;

private:
    AbiInfo abi_;
};

}  // namespace Backend
