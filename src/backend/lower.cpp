#include <backend/lower.hpp>

#include <map>
#include <string>
#include <vector>

#include <fadec-enc2.h>

namespace Backend {

namespace {

// Map a Backend::PhysReg to a Fadec FeRegGP.
FeRegGP fe(PhysReg r) { return FE_GP(regIndex(r)); }

// Map a Backend::XmmReg to a Fadec FeRegXMM.
FeRegXMM fx(XmmReg r) { return FE_XMM(xmmIndex(r)); }

// A memory operand [rbp + off].
FeMem rbpMem(std::int32_t off) { return FE_MEM(FE_BP, 0, FE_NOREG, off); }

// A memory operand [rsp + off].
FeMem rspMem(std::int32_t off) { return FE_MEM(FE_SP, 0, FE_NOREG, off); }

// Emit dst(64) = sext/zext of the `width`-byte value in memory `m`.
void emitLoadExt(Encoder& enc, FeRegGP dst, FeMem m, unsigned width, bool isSigned) {
    if (isSigned) {
        switch (width) {
            case 1: enc.emit([=](std::uint8_t* p){ return fe64_MOVSXr64m8(p, 0, dst, m); }); return;
            case 2: enc.emit([=](std::uint8_t* p){ return fe64_MOVSXr64m16(p, 0, dst, m); }); return;
            case 4: enc.emit([=](std::uint8_t* p){ return fe64_MOVSXr64m32(p, 0, dst, m); }); return;
            default: enc.emit([=](std::uint8_t* p){ return fe64_MOV64rm(p, 0, dst, m); }); return;
        }
    }
    switch (width) {
        case 1: enc.emit([=](std::uint8_t* p){ return fe64_MOVZXr64m8(p, 0, dst, m); }); return;
        case 2: enc.emit([=](std::uint8_t* p){ return fe64_MOVZXr64m16(p, 0, dst, m); }); return;
        // 32-bit load into a 32-bit reg auto-zero-extends to 64.
        case 4: enc.emit([=](std::uint8_t* p){ return fe64_MOV32rm(p, 0, dst, m); }); return;
        default: enc.emit([=](std::uint8_t* p){ return fe64_MOV64rm(p, 0, dst, m); }); return;
    }
}

// Emit a store of the low `width` bytes of register `src` to memory `m`.
void emitStoreNarrow(Encoder& enc, FeMem m, FeRegGP src, unsigned width) {
    switch (width) {
        case 1: enc.emit([=](std::uint8_t* p){ return fe64_MOV8mr(p, 0, m, src); }); return;
        case 2: enc.emit([=](std::uint8_t* p){ return fe64_MOV16mr(p, 0, m, src); }); return;
        case 4: enc.emit([=](std::uint8_t* p){ return fe64_MOV32mr(p, 0, m, src); }); return;
        default: enc.emit([=](std::uint8_t* p){ return fe64_MOV64mr(p, 0, m, src); }); return;
    }
}

// Emit reg(64) = sext/zext of its own low `width` bytes (register form).
void emitRegExt(Encoder& enc, FeRegGP reg, unsigned width, bool isSigned) {
    if (width >= 8) return;
    if (isSigned) {
        switch (width) {
            case 1: enc.emit([=](std::uint8_t* p){ return fe64_MOVSXr64r8(p, 0, reg, reg); }); return;
            case 2: enc.emit([=](std::uint8_t* p){ return fe64_MOVSXr64r16(p, 0, reg, reg); }); return;
            default: enc.emit([=](std::uint8_t* p){ return fe64_MOVSXr64r32(p, 0, reg, reg); }); return;
        }
    }
    switch (width) {
        case 1: enc.emit([=](std::uint8_t* p){ return fe64_MOVZXr64r8(p, 0, reg, reg); }); return;
        case 2: enc.emit([=](std::uint8_t* p){ return fe64_MOVZXr64r16(p, 0, reg, reg); }); return;
        // 32->64 zero-extend: a 32-bit reg-reg mov clears the upper 32 bits.
        default: enc.emit([=](std::uint8_t* p){ return fe64_MOV32rr(p, 0, reg, reg); }); return;
    }
}

}  // namespace

void Lowering::layoutFrame(MFunction& fn, const Allocation& alloc) {
    FrameLayout& fl = fn.layout();
    if (fl.laidOut) return;

    // Determine which callee-saved registers the allocation actually uses.
    std::vector<bool> used(kNumGPRegs, false);
    std::vector<bool> usedXmm(kNumXmmRegs, false);
    for (auto& bb : fn.blocks()) {
        for (auto& inst : bb.insts) {
            for (auto& op : inst.operands) {
                if (op.kind == OperandKind::VirtReg && op.phys != PhysReg::None) {
                    used[regIndex(op.phys)] = true;
                } else if (op.kind == OperandKind::PhysReg && op.phys != PhysReg::None) {
                    used[regIndex(op.phys)] = true;
                }
                if (op.xmm != XmmReg::None) {
                    usedXmm[xmmIndex(op.xmm)] = true;
                }
            }
            // Explicitly clobbered registers (e.g. constrained inline asm that
            // binds an input/output to a callee-saved register) must also be
            // preserved across the function, so treat them as "used".
            for (PhysReg c : inst.clobbers) {
                if (c != PhysReg::None) used[regIndex(c)] = true;
            }
        }
    }
    fl.savedCalleeRegs.clear();
    for (PhysReg r : abi_.calleeSaved) {
        if (used[regIndex(r)]) fl.savedCalleeRegs.push_back(r);
    }
    fl.savedXmmRegs.clear();
    for (XmmReg r : abi_.xmmCalleeSaved) {
        if (usedXmm[xmmIndex(r)]) fl.savedXmmRegs.push_back(r);
    }

    // Assign each frame slot an offset from RBP. Slots grow downward; the first
    // local sits just below the saved RBP and any pushed callee-saved registers.
    //
    // The prologue does `push rbp; mov rbp, rsp` (so RBP points at the saved
    // RBP), then pushes each used callee-saved GP register. Those pushes occupy
    // [rbp-8], [rbp-16], ... one 8-byte slot each. Locals must therefore start
    // BELOW that pushed region, otherwise the first local would alias a saved
    // register (clobbering it / reading garbage).
    std::int64_t cursor = 8 * static_cast<std::int64_t>(fl.savedCalleeRegs.size());
    // Bytes occupied by the pushed callee-saved GP registers (allocated via
    // `push`, not via `sub rsp`). The locals/spill/XMM-save region grows below
    // this and is what `sub rsp` must actually reserve.
    const std::int64_t pushedRegBytes = cursor;
    for (auto& slot : fn.frameSlots()) {
        if (slot.isIncoming) continue;  // caller-frame slot: rbpOffset preset (+)
        unsigned align = slot.align ? slot.align : 8;
        cursor += slot.size;
        // align the (negative) offset's magnitude up to the slot alignment
        std::int64_t rem = cursor % align;
        if (rem != 0) cursor += (align - rem);
        slot.rbpOffset = -cursor;
    }
    fl.localsSize = cursor - pushedRegBytes;

    // Reserve 8-byte RBP-relative slots for each used XMM callee-saved register,
    // placed just below the locals. XMM registers cannot be pushed, so they are
    // saved with movsd into these slots (inside the `sub rsp` region) in the
    // prologue and reloaded in the epilogue. savedXmmRegs[k] lives at
    // savedXmmBaseOffset - 8*k.
    fl.savedXmmBaseOffset = 0;
    if (!fl.savedXmmRegs.empty()) {
        cursor += 8;  // first saved-XMM slot sits 8 bytes below the locals
        fl.savedXmmBaseOffset = -cursor;
        cursor += 8 * static_cast<std::int64_t>(fl.savedXmmRegs.size() - 1);
        fl.localsSize = cursor - pushedRegBytes;
    }

    // Reserve space for outgoing call arguments (incl. Win64 shadow space) at the
    // bottom of the frame, addressed RSP-relative at call sites. Callee-saved
    // registers are pushed BEFORE `sub rsp` (see emitPrologue), so RSP stays
    // fixed at the frame bottom for the whole body and outgoing args live at
    // [rsp + 0 .. outgoingSize).
    fl.outgoingSize = fn.maxOutgoingArgBytes();

    // Keep RSP 16-byte aligned at the point of a call. At entry, [rsp] holds the
    // return address (so rsp % 16 == 8). After `push rbp` and the callee-saved
    // pushes, rsp has moved by 8*(1 + savedCount). We then `sub rsp, frameSize`;
    // size the frame so the total stays 16-aligned at call sites.
    std::int64_t frame = fl.localsSize + fl.outgoingSize +
                         static_cast<std::int64_t>(abi_.shadowSpace);
    // Account for return address (8) + saved RBP (8) + saved callee regs.
    std::int64_t pushed = 8 /*retaddr*/ + 8 /*rbp*/ +
                          8 * static_cast<std::int64_t>(fl.savedCalleeRegs.size());
    std::int64_t total = pushed + frame;
    std::int64_t pad = (16 - (total % 16)) % 16;
    frame += pad;
    fl.frameSize = frame;
    fl.laidOut = true;
}

void Lowering::emitPrologue(MFunction& fn) {
    const FrameLayout& fl = fn.layout();
    // push rbp ; mov rbp, rsp
    enc_.emit([](std::uint8_t* p) { return fe64_PUSHr(p, 0, FE_BP); });
    enc_.emit([](std::uint8_t* p) { return fe64_MOV64rr(p, 0, FE_BP, FE_SP); });
    // push callee-saved registers used by the body (BEFORE the rsp adjustment so
    // rsp is fixed at the frame bottom for the body, enabling RSP-relative
    // outgoing-arg stores).
    for (PhysReg r : fl.savedCalleeRegs) {
        enc_.emit([r](std::uint8_t* p) { return fe64_PUSHr(p, 0, fe(r)); });
    }
    // Reserve the local frame. For frames larger than one page (4096 bytes) the
    // stack must be touched page-by-page from the top down so the OS guard page
    // is hit in order and stack pages are committed (Windows raises an access
    // violation otherwise; this is the role __chkstk plays in the MSVC ABI). We
    // inline an equivalent probe loop rather than calling __chkstk so the output
    // stays freestanding (no CRT dependency). Small frames use a single sub.
    constexpr std::int64_t kPageSize = 4096;
    if (fl.frameSize > 0 && fl.frameSize <= kPageSize) {
        const std::int32_t sz = static_cast<std::int32_t>(fl.frameSize);
        enc_.emit([sz](std::uint8_t* p) { return fe64_SUB64ri(p, 0, FE_SP, sz); });
    } else if (fl.frameSize > kPageSize) {
        // mov r11, frameSize
        const std::int64_t sz = fl.frameSize;
        enc_.emit([sz](std::uint8_t* p) { return fe64_MOV64ri(p, 0, FE_R11, sz); });
        const std::uint64_t loopTop = enc_.offset();
        //   sub rsp, 4096 ; touch [rsp] ; sub r11, 4096 ; cmp r11, 4096 ; ja loop
        enc_.emit([](std::uint8_t* p) { return fe64_SUB64ri(p, 0, FE_SP, 4096); });
        enc_.emit([](std::uint8_t* p) { return fe64_MOV8mi(p, 0, rspMem(0), 0); });
        enc_.emit([](std::uint8_t* p) { return fe64_SUB64ri(p, 0, FE_R11, 4096); });
        enc_.emit([](std::uint8_t* p) { return fe64_CMP64ri(p, 0, FE_R11, 4096); });
        const std::uint64_t dispOff = enc_.branchPlaceholder(Encoder::Branch::Ja);
        enc_.patchRel32(dispOff, loopTop);
        // sub rsp, r11   (reserve the remaining < 4096 bytes)
        enc_.emit([](std::uint8_t* p) { return fe64_SUB64rr(p, 0, FE_SP, FE_R11); });
    }
    // Save XMM callee-saved registers into their reserved RBP-relative slots
    // (movsd [rbp + off], xmm). Done after `sub rsp` so the slots are in-frame.
    for (std::size_t k = 0; k < fl.savedXmmRegs.size(); ++k) {
        XmmReg r = fl.savedXmmRegs[k];
        std::int32_t off = static_cast<std::int32_t>(fl.savedXmmBaseOffset - 8 * static_cast<std::int64_t>(k));
        enc_.emit([r, off](std::uint8_t* p) { return fe64_SSE_MOVSDmr(p, 0, rbpMem(off), fx(r)); });
    }
}

void Lowering::emitEpilogue(MFunction& fn) {
    const FrameLayout& fl = fn.layout();
    // Restore XMM callee-saved registers from their reserved slots before tearing
    // down the frame (movsd xmm, [rbp + off]).
    for (std::size_t k = 0; k < fl.savedXmmRegs.size(); ++k) {
        XmmReg r = fl.savedXmmRegs[k];
        std::int32_t off = static_cast<std::int32_t>(fl.savedXmmBaseOffset - 8 * static_cast<std::int64_t>(k));
        enc_.emit([r, off](std::uint8_t* p) { return fe64_SSE_MOVSDrm(p, 0, fx(r), rbpMem(off)); });
    }
    // undo `sub rsp` first
    if (fl.frameSize > 0) {
        const std::int32_t sz = static_cast<std::int32_t>(fl.frameSize);
        enc_.emit([sz](std::uint8_t* p) { return fe64_ADD64ri(p, 0, FE_SP, sz); });
    }
    // pop callee-saved in reverse order
    for (auto it = fl.savedCalleeRegs.rbegin(); it != fl.savedCalleeRegs.rend(); ++it) {
        PhysReg r = *it;
        enc_.emit([r](std::uint8_t* p) { return fe64_POPr(p, 0, fe(r)); });
    }
    // pop rbp ; ret
    enc_.emit([](std::uint8_t* p) { return fe64_POPr(p, 0, FE_BP); });
    enc_.emit([](std::uint8_t* p) { return fe64_RET(p, 0); });
}

bool Lowering::emit(MFunction& fn, const Allocation& alloc, std::string& errorOut) {
    layoutFrame(fn, alloc);

    // Intern this function's string literals into .rodata BEFORE lowering the
    // body: each gets a defined local symbol at its blob offset, so the body's
    // `Lea def, symbol(.Lstr.N)` resolves to a defined (not external) symbol.
    // The bytes are stored verbatim followed by an implicit NUL terminator
    // (unless the constant carries its own, e.g. a UTF-16 wide string).
    for (const auto& sc : fn.stringConstants()) {
        Section& ro = enc_.code().rodata;
        unsigned al = sc.align < 1 ? 1 : sc.align;
        if (ro.alignment < al) ro.alignment = al;
        while (ro.bytes.size() % al != 0) ro.bytes.push_back(0);  // align the blob
        std::uint64_t off = ro.bytes.size();
        ro.bytes.insert(ro.bytes.end(), sc.bytes.begin(), sc.bytes.end());
        if (sc.appendNul) ro.bytes.push_back(0);  // NUL terminator
        enc_.code().defineSymbol(sc.symbol, SectionKind::RoData, off,
                                 SymbolBinding::Local, /*isFunction=*/false);
    }

    // A link-once definition goes in a section of its own, named after the symbol.
    // These are the same body emitted by every module that uses them
    // (a monomorphized generic, say), so the object writer has to tell the linker
    // to fold the copies -- and in COFF that is a per-section property (a COMDAT),
    // not a per-symbol one. Giving each its own section is what makes that
    // expressible; an explicit `[section("name")]` still wins.
    std::string codeSection = fn.customSection;
    if (codeSection.empty() && fn.linkage == MFunction::Linkage::LinkOnce) {
        codeSection = ".text$w$" + fn.name();
    }

    // Redirect code emission into this function's section. A `[section("name")]`
    // function's machine code, its branch fixups, and its code-relative
    // relocations all land in the named section instead of the primary .text.
    enc_.code().selectCodeSection(codeSection);

    // Resolve the symbol binding from the requested linkage. Default rule: an
    // exported function (or one with no explicit choice) is Global so it is
    // visible to the linker; everything else still defaults to Global to match
    // the prior hardcoded behavior (a future pass may infer Local for
    // file-private functions).
    SymbolBinding binding = SymbolBinding::Global;
    switch (fn.linkage) {
        case MFunction::Linkage::Internal: binding = SymbolBinding::Local; break;
        case MFunction::Linkage::External: binding = SymbolBinding::Global; break;
        case MFunction::Linkage::Weak: binding = SymbolBinding::Weak; break;
        case MFunction::Linkage::LinkOnce: binding = SymbolBinding::LinkOnce; break;
        case MFunction::Linkage::Default: binding = SymbolBinding::Global; break;
    }

    if (codeSection.empty()) {
        enc_.code().defineSymbol(fn.name(), SectionKind::Text, enc_.offset(),
                                 binding, /*isFunction=*/true);
    } else {
        enc_.code().defineSymbolInSection(fn.name(), codeSection, enc_.offset(),
                                          binding, /*isFunction=*/true);
    }

    // Naked functions emit their body verbatim: no compiler prologue/epilogue.
    if (!fn.naked) emitPrologue(fn);

    blockStart_.assign(fn.blocks().size(), 0);
    fixups_.clear();

    for (std::uint32_t bi = 0; bi < fn.blocks().size(); ++bi) {
        blockStart_[bi] = enc_.offset();
        for (auto& inst : fn.block(bi).insts) {
            if (!emitInst(fn, alloc, inst, errorOut)) return false;
        }
    }

    resolveBranches();
    return true;
}

void Lowering::resolveBranches() {
    for (const auto& fx : fixups_) {
        enc_.patchRel32(fx.dispOffset, blockStart_[fx.targetBlock]);
    }
}

bool Lowering::emitInst(MFunction& fn, const Allocation& alloc, const MInst& inst,
                        std::string& errorOut) {
    // Helper: resolve a VirtReg/PhysReg operand to a concrete PhysReg, materially
    // reloading from its spill slot into a scratch register if it was spilled.
    // For the scaffold we use R10/R11 as fixed scratch for reloaded operands.
    auto slotOffset = [&](std::uint32_t slotIdx) -> std::int32_t {
        return static_cast<std::int32_t>(fn.frameSlots()[slotIdx].rbpOffset);
    };

    auto physOf = [&](const MOperand& op, PhysReg scratch, bool isUse) -> PhysReg {
        if (op.kind == OperandKind::PhysReg) return op.phys;
        if (op.kind == OperandKind::VirtReg) {
            if (op.phys != PhysReg::None) return op.phys;
            // spilled: reload into scratch for a use
            std::uint32_t slot = alloc.vregToSlot[op.vreg];
            std::int32_t off = slotOffset(slot);
            if (isUse) {
                enc_.emit([scratch, off](std::uint8_t* p) {
                    return fe64_MOV64rm(p, 0, fe(scratch), rbpMem(off));
                });
            }
            return scratch;
        }
        return PhysReg::None;
    };

    // If a def operand was spilled, store the scratch back to its slot.
    auto storeIfSpilled = [&](const MOperand& op, PhysReg scratch) {
        if (op.kind == OperandKind::VirtReg && op.phys == PhysReg::None) {
            std::uint32_t slot = alloc.vregToSlot[op.vreg];
            std::int32_t off = slotOffset(slot);
            enc_.emit([scratch, off](std::uint8_t* p) {
                return fe64_MOV64mr(p, 0, rbpMem(off), fe(scratch));
            });
        }
    };

    // --- XMM (float/double) operand helpers --------------------------------
    // Resolve an XMM operand to a concrete XmmReg, reloading a spilled value into
    // the given scratch XMM (XMM4/XMM5 are caller-saved scratch in both ABIs).
    auto physOfXmm = [&](const MOperand& op, XmmReg scratch, bool isUse) -> XmmReg {
        if (op.kind == OperandKind::PhysReg) return op.xmm;
        if (op.kind == OperandKind::VirtReg) {
            if (op.xmm != XmmReg::None) return op.xmm;
            std::uint32_t slot = alloc.vregToSlot[op.vreg];
            std::int32_t off = slotOffset(slot);
            if (isUse) {
                enc_.emit([scratch, off](std::uint8_t* p) {
                    return fe64_SSE_MOVSDrm(p, 0, fx(scratch), rbpMem(off));
                });
            }
            return scratch;
        }
        return XmmReg::None;
    };
    auto storeIfSpilledXmm = [&](const MOperand& op, XmmReg scratch) {
        if (op.kind == OperandKind::VirtReg && op.xmm == XmmReg::None) {
            std::uint32_t slot = alloc.vregToSlot[op.vreg];
            std::int32_t off = slotOffset(slot);
            enc_.emit([scratch, off](std::uint8_t* p) {
                return fe64_SSE_MOVSDmr(p, 0, rbpMem(off), fx(scratch));
            });
        }
    };

    switch (inst.op) {
        case MOpcode::MovRI: {
            const MOperand& d = inst.operands[0];
            std::int64_t imm = inst.operands[1].imm;
            PhysReg dr = physOf(d, PhysReg::R10, /*isUse=*/false);
            enc_.emit([dr, imm](std::uint8_t* p) {
                return fe64_MOV64ri(p, 0, fe(dr), imm);
            });
            storeIfSpilled(d, dr);
            return true;
        }
        case MOpcode::MovRR: {
            const MOperand& d = inst.operands[0];
            const MOperand& s = inst.operands[1];
            PhysReg sr = physOf(s, PhysReg::R11, /*isUse=*/true);
            PhysReg dr = physOf(d, PhysReg::R10, /*isUse=*/false);
            enc_.emit([dr, sr](std::uint8_t* p) {
                return fe64_MOV64rr(p, 0, fe(dr), fe(sr));
            });
            storeIfSpilled(d, dr);
            return true;
        }
        case MOpcode::Add:
        case MOpcode::Sub:
        case MOpcode::IMul:
        case MOpcode::And:
        case MOpcode::Or:
        case MOpcode::Xor: {
            const MOperand& d = inst.operands[0];  // usedef
            const MOperand& s = inst.operands[1];  // use
            PhysReg dr = physOf(d, PhysReg::R10, /*isUse=*/true);
            PhysReg sr = physOf(s, PhysReg::R11, /*isUse=*/true);
            switch (inst.op) {
                case MOpcode::Add:
                    enc_.emit([dr, sr](std::uint8_t* p) { return fe64_ADD64rr(p, 0, fe(dr), fe(sr)); }); break;
                case MOpcode::Sub:
                    enc_.emit([dr, sr](std::uint8_t* p) { return fe64_SUB64rr(p, 0, fe(dr), fe(sr)); }); break;
                case MOpcode::IMul:
                    enc_.emit([dr, sr](std::uint8_t* p) { return fe64_IMUL64rr(p, 0, fe(dr), fe(sr)); }); break;
                case MOpcode::And:
                    enc_.emit([dr, sr](std::uint8_t* p) { return fe64_AND64rr(p, 0, fe(dr), fe(sr)); }); break;
                case MOpcode::Or:
                    enc_.emit([dr, sr](std::uint8_t* p) { return fe64_OR64rr(p, 0, fe(dr), fe(sr)); }); break;
                default:  // Xor
                    enc_.emit([dr, sr](std::uint8_t* p) { return fe64_XOR64rr(p, 0, fe(dr), fe(sr)); }); break;
            }
            storeIfSpilled(d, dr);
            return true;
        }
        case MOpcode::Neg:
        case MOpcode::Not: {
            const MOperand& d = inst.operands[0];  // usedef
            PhysReg dr = physOf(d, PhysReg::R10, /*isUse=*/true);
            if (inst.op == MOpcode::Neg)
                enc_.emit([dr](std::uint8_t* p) { return fe64_NEG64r(p, 0, fe(dr)); });
            else
                enc_.emit([dr](std::uint8_t* p) { return fe64_NOT64r(p, 0, fe(dr)); });
            storeIfSpilled(d, dr);
            return true;
        }
        case MOpcode::Shl:
        case MOpcode::Shr: {
            // usedef0 = usedef0 <shift> use1 ; the shift count must live in CL.
            const MOperand& d = inst.operands[0];   // usedef value
            const MOperand& s = inst.operands[1];   // use shift amount
            PhysReg amt = physOf(s, PhysReg::R11, /*isUse=*/true);
            // mov rcx, amount  (count register; SHL/SHR/SAR read CL)
            enc_.emit([amt](std::uint8_t* p) { return fe64_MOV64rr(p, 0, fe(PhysReg::RCX), fe(amt)); });
            PhysReg dr = physOf(d, PhysReg::R10, /*isUse=*/true);
            const FeRegGP cl = fe(PhysReg::RCX);
            if (inst.op == MOpcode::Shl) {
                enc_.emit([dr, cl](std::uint8_t* p) { return fe64_SHL64rr(p, 0, fe(dr), cl); });
            } else if (inst.isSigned) {
                enc_.emit([dr, cl](std::uint8_t* p) { return fe64_SAR64rr(p, 0, fe(dr), cl); });
            } else {
                enc_.emit([dr, cl](std::uint8_t* p) { return fe64_SHR64rr(p, 0, fe(dr), cl); });
            }
            storeIfSpilled(d, dr);
            return true;
        }
        case MOpcode::Div:
        case MOpcode::Mod: {
            // def0 = use1 (dividend) / or % use2 (divisor).
            // x86-64: dividend in RDX:RAX, idiv/div divisor -> RAX=quot, RDX=rem.
            const MOperand& d = inst.operands[0];
            const MOperand& dividend = inst.operands[1];
            const MOperand& divisor = inst.operands[2];
            PhysReg dvd = physOf(dividend, PhysReg::R11, /*isUse=*/true);
            // mov rax, dividend
            enc_.emit([dvd](std::uint8_t* p) { return fe64_MOV64rr(p, 0, fe(PhysReg::RAX), fe(dvd)); });
            // The divisor must not be RAX/RDX (those hold the dividend); reload a
            // spilled divisor into R10 (a fixed scratch outside RAX/RDX).
            PhysReg dvs = physOf(divisor, PhysReg::R10, /*isUse=*/true);
            if (dvs == PhysReg::RAX || dvs == PhysReg::RDX) {
                enc_.emit([dvs](std::uint8_t* p) { return fe64_MOV64rr(p, 0, fe(PhysReg::R10), fe(dvs)); });
                dvs = PhysReg::R10;
            }
            if (inst.isSigned) {
                // cqo: sign-extend RAX into RDX:RAX, then idiv.
                enc_.emit([](std::uint8_t* p) { return fe64_CQO(p, 0); });
                enc_.emit([dvs](std::uint8_t* p) { return fe64_IDIV64r(p, 0, fe(dvs)); });
            } else {
                // xor edx, edx (clear upper dividend), then div.
                enc_.emit([](std::uint8_t* p) { return fe64_XOR32rr(p, 0, fe(PhysReg::RDX), fe(PhysReg::RDX)); });
                enc_.emit([dvs](std::uint8_t* p) { return fe64_DIV64r(p, 0, fe(dvs)); });
            }
            // Result: quotient in RAX (Div), remainder in RDX (Mod).
            PhysReg resReg = (inst.op == MOpcode::Div) ? PhysReg::RAX : PhysReg::RDX;
            PhysReg dr = physOf(d, PhysReg::R10, /*isUse=*/false);
            enc_.emit([dr, resReg](std::uint8_t* p) { return fe64_MOV64rr(p, 0, fe(dr), fe(resReg)); });
            storeIfSpilled(d, dr);
            return true;
        }
        case MOpcode::UMulHi: {
            // def0 = high 64 bits of (use1 * use2), unsigned.
            // mov rax, use1 ; mul use2 ; result high word in RDX.
            const MOperand& d = inst.operands[0];
            const MOperand& a = inst.operands[1];
            const MOperand& b = inst.operands[2];
            PhysReg ar = physOf(a, PhysReg::R11, /*isUse=*/true);
            enc_.emit([ar](std::uint8_t* p) { return fe64_MOV64rr(p, 0, fe(PhysReg::RAX), fe(ar)); });
            PhysReg br = physOf(b, PhysReg::R10, /*isUse=*/true);
            if (br == PhysReg::RAX || br == PhysReg::RDX) {
                enc_.emit([br](std::uint8_t* p) { return fe64_MOV64rr(p, 0, fe(PhysReg::R10), fe(br)); });
                br = PhysReg::R10;
            }
            enc_.emit([br](std::uint8_t* p) { return fe64_MUL64r(p, 0, fe(br)); });
            PhysReg dr = physOf(d, PhysReg::R10, /*isUse=*/false);
            enc_.emit([dr](std::uint8_t* p) { return fe64_MOV64rr(p, 0, fe(dr), fe(PhysReg::RDX)); });
            storeIfSpilled(d, dr);
            return true;
        }
        case MOpcode::Cmp: {
            PhysReg a = physOf(inst.operands[0], PhysReg::R10, true);
            PhysReg b = physOf(inst.operands[1], PhysReg::R11, true);
            enc_.emit([a, b](std::uint8_t* p) { return fe64_CMP64rr(p, 0, fe(a), fe(b)); });
            return true;
        }
        case MOpcode::Call: {
            // Caller-saved registers are assumed clobbered (recorded in
            // inst.clobbers and respected by the allocator). Emit the call.
            const std::string& target = inst.operands[0].symbol;
            enc_.callSymbol(target);
            return true;
        }
        case MOpcode::Syscall: {
            // `syscall` (0F 05): args already in rax/rdi/rsi/rdx/r10/r8/r9, result
            // in rax. Caller-saved clobbers recorded in inst.clobbers.
            enc_.emit([](std::uint8_t* p) { return fe64_SYSCALL(p, 0); });
            return true;
        }
        case MOpcode::CallImport: {
            // call qword ptr [rip + disp32]  (FF /2, ModRM mod=00 reg=2 rm=101)
            // -> indirect call through the DLL-import's IAT slot. operand0 = fn
            // name, operand1 = dll. The disp32 is patched by the PE writer via an
            // ImportCall32 relocation against the import symbol.
            const std::string& fn = inst.operands[0].symbol;
            const std::string& dll = inst.operands[1].symbol;
            auto& sec = enc_.code().currentCode();
            sec.bytes.push_back(0xFF);
            sec.bytes.push_back(0x15);
            std::uint64_t dispOff = sec.bytes.size();
            for (int i = 0; i < 4; ++i) sec.bytes.push_back(0);
            std::uint32_t sym = enc_.code().referenceImport(fn, dll);
            if (enc_.code().currentCodeName().empty()) {
                enc_.code().addRelocation(SectionKind::Text, dispOff, sym,
                                          RelocKind::ImportCall32, 0);
            } else {
                enc_.code().addRelocationInSection(enc_.code().currentCodeName(), dispOff,
                                                   sym, RelocKind::ImportCall32, 0);
            }
            return true;
        }
        case MOpcode::CallIndirect: {
            // call qword ptr [rbp + off]  (FF /2): indirect call through the target
            // address previously spilled to frameSlot0. Reading the target from
            // memory means it survives argument-register marshalling untouched.
            std::int32_t off = slotOffset(inst.operands[0].frameSlot);
            FeMem m = rbpMem(off);
            enc_.emit([m](std::uint8_t* p) { return fe64_CALLm(p, 0, m); });
            return true;
        }
        case MOpcode::Ret: {
            emitEpilogue(fn);
            return true;
        }
        case MOpcode::Fence: {
            enc_.emit([](std::uint8_t* p) { return fe64_MFENCE(p, 0); });
            return true;
        }
        case MOpcode::AtomicXAdd: {
            // lock xadd [base1 + imm2], val(usedef3); def0 = old value (fetched
            // into the value register by xadd). We route the address base through
            // R11 and the value through R10 (fixed scratch), then copy the fetched
            // value to the destination. `width` selects the operand size.
            const MOperand& d = inst.operands[0];
            PhysReg base = physOf(inst.operands[1], PhysReg::R11, /*isUse=*/true);
            std::int32_t off = static_cast<std::int32_t>(inst.operands[2].imm);
            PhysReg val = physOf(inst.operands[3], PhysReg::R10, /*isUse=*/true);
            // The value reg must be distinct from base; if both landed in the same
            // scratch (spill aliasing), relocate the addend into R10.
            if (val == base) {
                enc_.emit([val](std::uint8_t* p){ return fe64_MOV64rr(p, 0, fe(PhysReg::R10), fe(val)); });
                val = PhysReg::R10;
            }
            FeMem m = FE_MEM(fe(base), 0, FE_NOREG, off);
            const unsigned w = inst.width;
            enc_.emit([m, val, w](std::uint8_t* p) {
                switch (w) {
                    case 1: return fe64_LOCK_XADD8mr(p, 0, m, fe(val));
                    case 2: return fe64_LOCK_XADD16mr(p, 0, m, fe(val));
                    case 4: return fe64_LOCK_XADD32mr(p, 0, m, fe(val));
                    default: return fe64_LOCK_XADD64mr(p, 0, m, fe(val));
                }
            });
            // After xadd, `val` holds the previous memory value -> move to dest.
            PhysReg dr = physOf(d, PhysReg::R10, /*isUse=*/false);
            if (dr != val) {
                enc_.emit([dr, val](std::uint8_t* p){ return fe64_MOV64rr(p, 0, fe(dr), fe(val)); });
            }
            storeIfSpilled(d, dr);
            return true;
        }
        case MOpcode::AtomicCmpXchg: {
            // lock cmpxchg [base], desired. x86 pins the comparand in RAX. The
            // three inputs were spilled to frame slots by isel, so we load them
            // into fixed registers with no aliasing: expected->RAX, base->R11,
            // desired->R10. Then setz -> dest (1 on success). RAX is clobbered.
            const MOperand& d = inst.operands[0];
            std::int32_t expOff = slotOffset(inst.operands[1].frameSlot);
            std::int32_t ptrOff = slotOffset(inst.operands[2].frameSlot);
            std::int32_t desOff = slotOffset(inst.operands[3].frameSlot);
            enc_.emit([expOff](std::uint8_t* p){ return fe64_MOV64rm(p, 0, fe(PhysReg::RAX), rbpMem(expOff)); });
            enc_.emit([ptrOff](std::uint8_t* p){ return fe64_MOV64rm(p, 0, fe(PhysReg::R11), rbpMem(ptrOff)); });
            enc_.emit([desOff](std::uint8_t* p){ return fe64_MOV64rm(p, 0, fe(PhysReg::R10), rbpMem(desOff)); });
            FeMem m = FE_MEM(fe(PhysReg::R11), 0, FE_NOREG, 0);
            const unsigned w = inst.width;
            enc_.emit([m, w](std::uint8_t* p) {
                switch (w) {
                    case 1: return fe64_LOCK_CMPXCHG8mr(p, 0, m, fe(PhysReg::R10));
                    case 2: return fe64_LOCK_CMPXCHG16mr(p, 0, m, fe(PhysReg::R10));
                    case 4: return fe64_LOCK_CMPXCHG32mr(p, 0, m, fe(PhysReg::R10));
                    default: return fe64_LOCK_CMPXCHG64mr(p, 0, m, fe(PhysReg::R10));
                }
            });
            // ZF set => exchange happened. setz dest8 ; movzx dest64, dest8.
            PhysReg dr = physOf(d, PhysReg::R10, /*isUse=*/false);
            const FeRegGP r = fe(dr);
            enc_.emit([r](std::uint8_t* p){ return fe64_SETZ8r(p, 0, r); });
            enc_.emit([r](std::uint8_t* p){ return fe64_MOVZXr64r8(p, 0, r, r); });
            storeIfSpilled(d, dr);
            return true;
        }
        case MOpcode::AsmBlock: {
            // Assemble a raw `asm( ... )` block here rather than during selection:
            // a `$var` operand is a frame slot, and slots only acquire addresses
            // once the frame layout is fixed, which has happened by now.
            const std::uint32_t blockIdx =
                static_cast<std::uint32_t>(inst.operands[0].imm);
            if (blockIdx >= fn.asmBlocks.size()) {
                errorOut = "lowering: asm block index out of range";
                return false;
            }
            // Labels are resolved the same way the compiler's own branches are: a
            // branch to a label emits a rel32 placeholder, and the displacement is
            // patched once every label offset is known. That handles forward
            // references without needing to guess instruction sizes.
            std::map<std::string, std::uint64_t> labelOffsets;
            struct AsmFixup {
                std::uint64_t dispOffset;
                std::string target;
                int line;
            };
            std::vector<AsmFixup> fixups;

            const auto branchKind = [](Backend::AsmBranch b) {
                switch (b) {
                    case Backend::AsmBranch::Jmp: return Encoder::Branch::Jmp;
                    case Backend::AsmBranch::Je: return Encoder::Branch::Je;
                    case Backend::AsmBranch::Jne: return Encoder::Branch::Jne;
                    case Backend::AsmBranch::Jl: return Encoder::Branch::Jl;
                    case Backend::AsmBranch::Jle: return Encoder::Branch::Jle;
                    case Backend::AsmBranch::Jg: return Encoder::Branch::Jg;
                    case Backend::AsmBranch::Jge: return Encoder::Branch::Jge;
                    case Backend::AsmBranch::Jb: return Encoder::Branch::Jb;
                    case Backend::AsmBranch::Jbe: return Encoder::Branch::Jbe;
                    case Backend::AsmBranch::Ja: return Encoder::Branch::Ja;
                    case Backend::AsmBranch::Jae: return Encoder::Branch::Jae;
                    case Backend::AsmBranch::None: break;
                }
                return Encoder::Branch::Jmp;
            };

            for (const Backend::AsmInst& srcInst : fn.asmBlocks[blockIdx].insts) {
                if (!srcInst.labelDef.empty()) {
                    if (!labelOffsets.emplace(srcInst.labelDef, enc_.offset()).second) {
                        errorOut = "asm block: line " + std::to_string(srcInst.line) +
                                   ": label '" + srcInst.labelDef +
                                   "' is defined more than once";
                        return false;
                    }
                }
                if (srcInst.mnemonic.empty()) continue;  // label-only line

                const Backend::AsmBranch br = Backend::asmBranchOf(srcInst.mnemonic);
                if (br != Backend::AsmBranch::None && srcInst.ops.size() == 1 &&
                    srcInst.ops[0].kind == Backend::AsmOpKind::Label) {
                    const std::uint64_t dispOff = enc_.branchPlaceholder(branchKind(br));
                    fixups.push_back({dispOff, srcInst.ops[0].var, srcInst.line});
                    continue;
                }

                Backend::AsmInst resolved = srcInst;
                for (Backend::AsmOperand& op : resolved.ops) {
                    if (op.kind != Backend::AsmOpKind::Slot) continue;
                    // `$v` becomes [rbp + slotOffset], with any displacement
                    // written inside the brackets ("[$v + 4]") folded in.
                    op.kind = Backend::AsmOpKind::Mem;
                    op.imm += slotOffset(op.slot);
                    op.reg = static_cast<int>(PhysReg::RBP);
                }
                std::string asmErr;
                std::uint8_t bytes[16];
                const int n = Backend::encodeAsmInst(resolved, bytes, asmErr);
                if (n < 0) {
                    errorOut = "asm block: " + asmErr;
                    return false;
                }
                enc_.emit([&bytes, n](std::uint8_t* p) {
                    for (int b = 0; b < n; ++b) p[b] = bytes[b];
                    return static_cast<unsigned>(n);
                });
            }

            for (const AsmFixup& fx : fixups) {
                const auto it = labelOffsets.find(fx.target);
                if (it == labelOffsets.end()) {
                    errorOut = "asm block: line " + std::to_string(fx.line) +
                               ": no label '" + fx.target + "' in this block";
                    return false;
                }
                enc_.patchRel32(fx.dispOffset, it->second);
            }
            return true;
        }
        case MOpcode::AsmFixed: {
            // A recognized fixed inline-asm template selected by operand0.imm.
            switch (inst.operands[0].imm) {
                case 0: enc_.emit([](std::uint8_t* p){ return fe64_NOP(p, 0); }); break;
                case 1: enc_.emit([](std::uint8_t* p){ return fe64_SYSCALL(p, 0); }); break;
                case 2: enc_.emit([](std::uint8_t* p){ return fe64_INT3(p, 0); }); break;
                case 3: enc_.emit([](std::uint8_t* p){ return fe64_MFENCE(p, 0); }); break;
                case 4: enc_.emit([](std::uint8_t* p){ return fe64_UD2(p, 0); }); break;
                case 5: enc_.emit([](std::uint8_t* p){ return fe64_PAUSE(p, 0); }); break;
                case 6: enc_.emit([](std::uint8_t* p){ return fe64_CPUID(p, 0); }); break;
                case 7: enc_.emit([](std::uint8_t* p){ return fe64_HLT(p, 0); }); break;
                default: enc_.emit([](std::uint8_t* p){ return fe64_NOP(p, 0); }); break;
            }
            return true;
        }
        case MOpcode::Load: {
            // def0 = sext/zext [frameSlot1] to 64 bits (width/signed in inst).
            const MOperand& d = inst.operands[0];
            std::int32_t off = slotOffset(inst.operands[1].frameSlot);
            PhysReg dr = physOf(d, PhysReg::R10, /*isUse=*/false);
            emitLoadExt(enc_, fe(dr), rbpMem(off), inst.width, inst.isSigned);
            storeIfSpilled(d, dr);
            return true;
        }
        case MOpcode::Store: {
            // [frameSlot0] = low `width` bytes of use1
            std::int32_t off = slotOffset(inst.operands[0].frameSlot);
            PhysReg sr = physOf(inst.operands[1], PhysReg::R11, /*isUse=*/true);
            emitStoreNarrow(enc_, rbpMem(off), fe(sr), inst.width);
            return true;
        }
        case MOpcode::Ext: {
            // usedef0 = sext/zext of its own low `width` bytes to 64 bits.
            const MOperand& d = inst.operands[0];
            PhysReg dr = physOf(d, PhysReg::R10, /*isUse=*/true);
            emitRegExt(enc_, fe(dr), inst.width, inst.isSigned);
            storeIfSpilled(d, dr);
            return true;
        }
        case MOpcode::StoreOutgoing: {
            // [rsp + imm0] = use1 (outgoing stack argument)
            std::int32_t off = static_cast<std::int32_t>(inst.operands[0].imm);
            PhysReg sr = physOf(inst.operands[1], PhysReg::R11, /*isUse=*/true);
            enc_.emit([off, sr](std::uint8_t* p) {
                return fe64_MOV64mr(p, 0, rspMem(off), fe(sr));
            });
            return true;
        }
        case MOpcode::Lea: {
            // def0 = address-of symbol1 (RIP-relative); record a Rel32 reloc.
            const MOperand& d = inst.operands[0];
            const std::string& symName = inst.operands[1].symbol;
            PhysReg dr = physOf(d, PhysReg::R10, /*isUse=*/false);
            // LEA reg, [rip + disp32] : REX.W 8D /r with ModRM mod=00 rm=101.
            const std::uint8_t reg = regIndex(dr);
            std::uint8_t rex = 0x48 | ((reg >> 3) & 1) << 2;  // REX.W + REX.R
            auto& sec = enc_.code().currentCode();
            sec.bytes.push_back(rex);
            sec.bytes.push_back(0x8D);
            sec.bytes.push_back(static_cast<std::uint8_t>(0x05 | ((reg & 7) << 3)));
            std::uint64_t dispOff = sec.bytes.size();
            for (int i = 0; i < 4; ++i) sec.bytes.push_back(0);
            std::uint32_t sym = enc_.code().referenceExternal(symName);
            if (enc_.code().currentCodeName().empty()) {
                enc_.code().addRelocation(SectionKind::Text, dispOff, sym, RelocKind::RipData32, 0);
            } else {
                enc_.code().addRelocationInSection(enc_.code().currentCodeName(), dispOff,
                                                   sym, RelocKind::RipData32, 0);
            }
            storeIfSpilled(d, dr);
            return true;
        }
        case MOpcode::LeaSlot: {
            // def0 = address of frameSlot1  ->  lea reg, [rbp + off]
            const MOperand& d = inst.operands[0];
            std::int32_t off = slotOffset(inst.operands[1].frameSlot);
            PhysReg dr = physOf(d, PhysReg::R10, /*isUse=*/false);
            enc_.emit([dr, off](std::uint8_t* p) {
                return fe64_LEA64rm(p, 0, fe(dr), rbpMem(off));
            });
            storeIfSpilled(d, dr);
            return true;
        }
        case MOpcode::LeaDisp: {
            // def0 = base1 + imm2  ->  lea reg, [base + disp]
            const MOperand& d = inst.operands[0];
            PhysReg base = physOf(inst.operands[1], PhysReg::R11, /*isUse=*/true);
            std::int32_t disp = static_cast<std::int32_t>(inst.operands[2].imm);
            // dst defaults to R10; even if base reloaded into R10 (when base and
            // dst both spill, base uses R11 here) the lea reads base then writes dst.
            PhysReg dr = physOf(d, PhysReg::R10, /*isUse=*/false);
            FeMem m = FE_MEM(fe(base), 0, FE_NOREG, disp);
            enc_.emit([dr, m](std::uint8_t* p) {
                return fe64_LEA64rm(p, 0, fe(dr), m);
            });
            storeIfSpilled(d, dr);
            return true;
        }
        case MOpcode::LoadInd: {
            // def0 = sext/zext [base1 + imm2] to 64 bits (width/signed in inst).
            const MOperand& d = inst.operands[0];
            PhysReg base = physOf(inst.operands[1], PhysReg::R11, /*isUse=*/true);
            std::int32_t off = static_cast<std::int32_t>(inst.operands[2].imm);
            PhysReg dr = physOf(d, PhysReg::R10, /*isUse=*/false);
            FeMem m = FE_MEM(fe(base), 0, FE_NOREG, off);
            emitLoadExt(enc_, fe(dr), m, inst.width, inst.isSigned);
            storeIfSpilled(d, dr);
            return true;
        }
        case MOpcode::StoreInd: {
            // [base0 + imm1] = low `width` bytes of use2.
            PhysReg base = physOf(inst.operands[0], PhysReg::R10, /*isUse=*/true);
            std::int32_t off = static_cast<std::int32_t>(inst.operands[1].imm);
            PhysReg sr = physOf(inst.operands[2], PhysReg::R11, /*isUse=*/true);
            FeMem m = FE_MEM(fe(base), 0, FE_NOREG, off);
            emitStoreNarrow(enc_, m, fe(sr), inst.width);
            return true;
        }
        case MOpcode::LeaIndex: {
            // def0 = base1 + index2*scale + imm3  -> lea reg,[base+idx*sc+disp]
            const MOperand& d = inst.operands[0];
            PhysReg base = physOf(inst.operands[1], PhysReg::R10, /*isUse=*/true);
            PhysReg index = physOf(inst.operands[2], PhysReg::R11, /*isUse=*/true);
            std::int32_t disp = inst.operands.size() > 3
                                    ? static_cast<std::int32_t>(inst.operands[3].imm)
                                    : 0;
            // dst defaults to R10; safe even if base used R10, since the lea reads
            // base+index together and only then writes the destination.
            PhysReg dr = physOf(d, PhysReg::R10, /*isUse=*/false);
            unsigned char sc = static_cast<unsigned char>(inst.scale);
            FeMem m = FE_MEM(fe(base), sc, fe(index), disp);
            enc_.emit([dr, m](std::uint8_t* p) {
                return fe64_LEA64rm(p, 0, fe(dr), m);
            });
            storeIfSpilled(d, dr);
            return true;
        }
        case MOpcode::Jmp: {
            std::uint64_t disp = enc_.branchPlaceholder(Encoder::Branch::Jmp);
            fixups_.push_back({disp, inst.operands[0].label});
            return true;
        }
        case MOpcode::Jcc: {
            Encoder::Branch br;
            switch (inst.cond) {
                case Cond::EQ: br = Encoder::Branch::Je; break;
                case Cond::NE: br = Encoder::Branch::Jne; break;
                case Cond::LT: br = Encoder::Branch::Jl; break;
                case Cond::LE: br = Encoder::Branch::Jle; break;
                case Cond::GT: br = Encoder::Branch::Jg; break;
                case Cond::GE: br = Encoder::Branch::Jge; break;
                case Cond::ULT: br = Encoder::Branch::Jb; break;
                case Cond::ULE: br = Encoder::Branch::Jbe; break;
                case Cond::UGT: br = Encoder::Branch::Ja; break;
                case Cond::UGE: br = Encoder::Branch::Jae; break;
            }
            std::uint64_t disp = enc_.branchPlaceholder(br);
            fixups_.push_back({disp, inst.operands[0].label});
            return true;
        }
        case MOpcode::SetCC: {
            // def0 = (flags satisfy cond) ? 1 : 0, zero-extended to 64 bits.
            // Sequence: setcc reg8 ; movzx reg64, reg8 (so the full reg is 0/1).
            const MOperand& d = inst.operands[0];
            PhysReg dr = physOf(d, PhysReg::R10, /*isUse=*/false);
            const FeRegGP r = fe(dr);
            switch (inst.cond) {
                case Cond::EQ: enc_.emit([r](std::uint8_t* p){ return fe64_SETZ8r(p, 0, r); }); break;
                case Cond::NE: enc_.emit([r](std::uint8_t* p){ return fe64_SETNZ8r(p, 0, r); }); break;
                case Cond::LT: enc_.emit([r](std::uint8_t* p){ return fe64_SETL8r(p, 0, r); }); break;
                case Cond::LE: enc_.emit([r](std::uint8_t* p){ return fe64_SETLE8r(p, 0, r); }); break;
                case Cond::GT: enc_.emit([r](std::uint8_t* p){ return fe64_SETG8r(p, 0, r); }); break;
                case Cond::GE: enc_.emit([r](std::uint8_t* p){ return fe64_SETGE8r(p, 0, r); }); break;
                case Cond::ULT: enc_.emit([r](std::uint8_t* p){ return fe64_SETC8r(p, 0, r); }); break;   // below = CF
                case Cond::ULE: enc_.emit([r](std::uint8_t* p){ return fe64_SETBE8r(p, 0, r); }); break;
                case Cond::UGT: enc_.emit([r](std::uint8_t* p){ return fe64_SETA8r(p, 0, r); }); break;
                case Cond::UGE: enc_.emit([r](std::uint8_t* p){ return fe64_SETNC8r(p, 0, r); }); break;  // above-or-equal = !CF
            }
            // Zero-extend the byte result to the full 64-bit register.
            enc_.emit([r](std::uint8_t* p){ return fe64_MOVZXr64r8(p, 0, r, r); });
            storeIfSpilled(d, dr);
            return true;
        }

        // --- floating point (scalar single/double) --------------------------
        // The MInst `width` field selects precision: 8 = double (movsd/addsd/...),
        // 4 = single (movss/addss/...).
        case MOpcode::FLoad: {
            const MOperand& d = inst.operands[0];
            std::int32_t off = slotOffset(inst.operands[1].frameSlot);
            XmmReg dr = physOfXmm(d, XmmReg::XMM4, /*isUse=*/false);
            const bool f32 = inst.width == 4;
            if (inst.width == 16) {
                enc_.emit([dr, off](std::uint8_t* p){
                    return fe64_SSE_MOVDQUrm(p, 0, fx(dr), rbpMem(off)); });
                storeIfSpilledXmm(d, dr);
                return true;
            }
            if (inst.width == 2) {
                // f16 load: read 2 bytes -> xmm -> vcvtph2ps -> f32 in dr.
                enc_.emit([off](std::uint8_t* p){
                    return fe64_MOVZXr32m16(p, 0, fe(PhysReg::R11), rbpMem(off)); });
                enc_.emit([dr](std::uint8_t* p){
                    return fe64_SSE_MOVD_G2Xrr(p, 0, fx(dr), fe(PhysReg::R11)); });
                enc_.emit([dr](std::uint8_t* p){
                    return fe64_VCVTPH2PS128rr(p, 0, fx(dr), fx(dr)); });
                storeIfSpilledXmm(d, dr);
                return true;
            }
            enc_.emit([dr, off, f32](std::uint8_t* p){
                return f32 ? fe64_SSE_MOVSSrm(p, 0, fx(dr), rbpMem(off))
                           : fe64_SSE_MOVSDrm(p, 0, fx(dr), rbpMem(off)); });
            storeIfSpilledXmm(d, dr);
            return true;
        }
        case MOpcode::FStore: {
            std::int32_t off = slotOffset(inst.operands[0].frameSlot);
            const MOperand& s = inst.operands[1];
            XmmReg sr = physOfXmm(s, XmmReg::XMM5, /*isUse=*/true);
            const bool f32 = inst.width == 4;
            if (inst.width == 16) {
                enc_.emit([sr, off](std::uint8_t* p){
                    return fe64_SSE_MOVDQUmr(p, 0, rbpMem(off), fx(sr)); });
                return true;
            }
            if (inst.width == 2) {
                // f16 store: vcvtps2ph(f32 sr) -> xmm4 -> movd r11 -> store 2 bytes.
                enc_.emit([sr](std::uint8_t* p){
                    return fe64_VCVTPS2PH128rri(p, 0, fx(XmmReg::XMM4), fx(sr), 0); });
                enc_.emit([](std::uint8_t* p){
                    return fe64_SSE_MOVD_X2Grr(p, 0, fe(PhysReg::R11), fx(XmmReg::XMM4)); });
                enc_.emit([off](std::uint8_t* p){
                    return fe64_MOV16mr(p, 0, rbpMem(off), fe(PhysReg::R11)); });
                return true;
            }
            enc_.emit([sr, off, f32](std::uint8_t* p){
                return f32 ? fe64_SSE_MOVSSmr(p, 0, rbpMem(off), fx(sr))
                           : fe64_SSE_MOVSDmr(p, 0, rbpMem(off), fx(sr)); });
            return true;
        }
        case MOpcode::FStoreOutgoing: {
            // [rsp + imm0] = use1(xmm) : outgoing float stack argument.
            std::int32_t off = static_cast<std::int32_t>(inst.operands[0].imm);
            const MOperand& s = inst.operands[1];
            XmmReg sr = physOfXmm(s, XmmReg::XMM5, /*isUse=*/true);
            const bool f32 = inst.width == 4;
            if (inst.width == 16) {
                enc_.emit([sr, off](std::uint8_t* p){
                    return fe64_SSE_MOVDQUmr(p, 0, rspMem(off), fx(sr)); });
                return true;
            }
            enc_.emit([sr, off, f32](std::uint8_t* p){
                return f32 ? fe64_SSE_MOVSSmr(p, 0, rspMem(off), fx(sr))
                           : fe64_SSE_MOVSDmr(p, 0, rspMem(off), fx(sr)); });
            return true;
        }
        case MOpcode::FLoadInd: {
            const MOperand& d = inst.operands[0];
            PhysReg base = physOf(inst.operands[1], PhysReg::R11, /*isUse=*/true);
            std::int32_t disp = static_cast<std::int32_t>(inst.operands[2].imm);
            XmmReg dr = physOfXmm(d, XmmReg::XMM4, /*isUse=*/false);
            FeMem m = FE_MEM(fe(base), 0, FE_NOREG, disp);
            const bool f32 = inst.width == 4;
            if (inst.width == 16) {
                enc_.emit([dr, m](std::uint8_t* p){
                    return fe64_SSE_MOVDQUrm(p, 0, fx(dr), m); });
                storeIfSpilledXmm(d, dr);
                return true;
            }
            if (inst.width == 2) {
                // f16 indirect load. Read 2 bytes via the base register, expand.
                enc_.emit([m](std::uint8_t* p){
                    return fe64_MOVZXr32m16(p, 0, fe(PhysReg::R10), m); });
                enc_.emit([dr](std::uint8_t* p){
                    return fe64_SSE_MOVD_G2Xrr(p, 0, fx(dr), fe(PhysReg::R10)); });
                enc_.emit([dr](std::uint8_t* p){
                    return fe64_VCVTPH2PS128rr(p, 0, fx(dr), fx(dr)); });
                storeIfSpilledXmm(d, dr);
                return true;
            }
            enc_.emit([dr, m, f32](std::uint8_t* p){
                return f32 ? fe64_SSE_MOVSSrm(p, 0, fx(dr), m)
                           : fe64_SSE_MOVSDrm(p, 0, fx(dr), m); });
            storeIfSpilledXmm(d, dr);
            return true;
        }
        case MOpcode::FStoreInd: {
            PhysReg base = physOf(inst.operands[0], PhysReg::R11, /*isUse=*/true);
            std::int32_t disp = static_cast<std::int32_t>(inst.operands[1].imm);
            const MOperand& s = inst.operands[2];
            XmmReg sr = physOfXmm(s, XmmReg::XMM5, /*isUse=*/true);
            FeMem m = FE_MEM(fe(base), 0, FE_NOREG, disp);
            const bool f32 = inst.width == 4;
            if (inst.width == 16) {
                enc_.emit([sr, m](std::uint8_t* p){
                    return fe64_SSE_MOVDQUmr(p, 0, m, fx(sr)); });
                return true;
            }
            if (inst.width == 2) {
                // f16 indirect store. Pack f32 -> f16, write 2 bytes.
                enc_.emit([sr](std::uint8_t* p){
                    return fe64_VCVTPS2PH128rri(p, 0, fx(XmmReg::XMM4), fx(sr), 0); });
                enc_.emit([](std::uint8_t* p){
                    return fe64_SSE_MOVD_X2Grr(p, 0, fe(PhysReg::R10), fx(XmmReg::XMM4)); });
                enc_.emit([m](std::uint8_t* p){
                    return fe64_MOV16mr(p, 0, m, fe(PhysReg::R10)); });
                return true;
            }
            enc_.emit([sr, m, f32](std::uint8_t* p){
                return f32 ? fe64_SSE_MOVSSmr(p, 0, m, fx(sr))
                           : fe64_SSE_MOVSDmr(p, 0, m, fx(sr)); });
            return true;
        }
        case MOpcode::FMovRR: {
            const MOperand& d = inst.operands[0];
            const MOperand& s = inst.operands[1];
            XmmReg sr = physOfXmm(s, XmmReg::XMM5, /*isUse=*/true);
            XmmReg dr = physOfXmm(d, XmmReg::XMM4, /*isUse=*/false);
            const bool f32 = inst.width == 4;
            if (inst.width == 16) {
                enc_.emit([dr, sr](std::uint8_t* p){
                    return fe64_SSE_MOVDQUrr(p, 0, fx(dr), fx(sr)); });
                storeIfSpilledXmm(d, dr);
                return true;
            }
            enc_.emit([dr, sr, f32](std::uint8_t* p){
                return f32 ? fe64_SSE_MOVSSrr(p, 0, fx(dr), fx(sr))
                           : fe64_SSE_MOVSDrr(p, 0, fx(dr), fx(sr)); });
            storeIfSpilledXmm(d, dr);
            return true;
        }
        case MOpcode::FConst: {
            // Materialize the float bit-pattern via a GP scratch, then movq/movd.
            // For f32 the low 32 bits of imm hold the single-precision pattern.
            const MOperand& d = inst.operands[0];
            std::int64_t bits = inst.operands[1].imm;
            const bool f32 = inst.width == 4;
            XmmReg dr = physOfXmm(d, XmmReg::XMM4, /*isUse=*/false);
            if (inst.width == 2) {
                // f16 constant: imm holds the 16-bit half pattern; expand to f32
                // in the register so it can be computed on.
                enc_.emit([bits](std::uint8_t* p){
                    return fe64_MOV32ri(p, 0, fe(PhysReg::R11),
                                        static_cast<int32_t>(bits & 0xFFFF)); });
                enc_.emit([dr](std::uint8_t* p){
                    return fe64_SSE_MOVD_G2Xrr(p, 0, fx(dr), fe(PhysReg::R11)); });
                enc_.emit([dr](std::uint8_t* p){
                    return fe64_VCVTPH2PS128rr(p, 0, fx(dr), fx(dr)); });
                storeIfSpilledXmm(d, dr);
                return true;
            }
            enc_.emit([bits](std::uint8_t* p){ return fe64_MOV64ri(p, 0, fe(PhysReg::R11), bits); });
            enc_.emit([dr, f32](std::uint8_t* p){
                return f32 ? fe64_SSE_MOVD_G2Xrr(p, 0, fx(dr), fe(PhysReg::R11))
                           : fe64_SSE_MOVQ_G2Xrr(p, 0, fx(dr), fe(PhysReg::R11)); });
            storeIfSpilledXmm(d, dr);
            return true;
        }
        case MOpcode::FAdd:
        case MOpcode::FSub:
        case MOpcode::FMul:
        case MOpcode::FDiv: {
            const MOperand& d = inst.operands[0];  // usedef
            const MOperand& s = inst.operands[1];  // use
            XmmReg dr = physOfXmm(d, XmmReg::XMM4, /*isUse=*/true);
            XmmReg sr = physOfXmm(s, XmmReg::XMM5, /*isUse=*/true);
            const bool f32 = inst.width == 4;
            switch (inst.op) {
                case MOpcode::FAdd:
                    enc_.emit([dr, sr, f32](std::uint8_t* p){
                        return f32 ? fe64_SSE_ADDSSrr(p, 0, fx(dr), fx(sr))
                                   : fe64_SSE_ADDSDrr(p, 0, fx(dr), fx(sr)); }); break;
                case MOpcode::FSub:
                    enc_.emit([dr, sr, f32](std::uint8_t* p){
                        return f32 ? fe64_SSE_SUBSSrr(p, 0, fx(dr), fx(sr))
                                   : fe64_SSE_SUBSDrr(p, 0, fx(dr), fx(sr)); }); break;
                case MOpcode::FMul:
                    enc_.emit([dr, sr, f32](std::uint8_t* p){
                        return f32 ? fe64_SSE_MULSSrr(p, 0, fx(dr), fx(sr))
                                   : fe64_SSE_MULSDrr(p, 0, fx(dr), fx(sr)); }); break;
                default:  // FDiv
                    enc_.emit([dr, sr, f32](std::uint8_t* p){
                        return f32 ? fe64_SSE_DIVSSrr(p, 0, fx(dr), fx(sr))
                                   : fe64_SSE_DIVSDrr(p, 0, fx(dr), fx(sr)); }); break;
            }
            storeIfSpilledXmm(d, dr);
            return true;
        }
        case MOpcode::FNeg: {
            // Flip the sign bit: xor with the precision's sign mask via a scratch
            // xmm (0x8000000000000000 for double, 0x80000000 for single).
            const MOperand& d = inst.operands[0];  // usedef
            XmmReg dr = physOfXmm(d, XmmReg::XMM4, /*isUse=*/true);
            const bool f32 = inst.width == 4;
            const std::int64_t mask = f32 ? static_cast<std::int64_t>(0x80000000ULL)
                                          : static_cast<std::int64_t>(0x8000000000000000ULL);
            enc_.emit([mask](std::uint8_t* p){
                return fe64_MOV64ri(p, 0, fe(PhysReg::R11), mask); });
            enc_.emit([f32](std::uint8_t* p){
                return f32 ? fe64_SSE_MOVD_G2Xrr(p, 0, fx(XmmReg::XMM5), fe(PhysReg::R11))
                           : fe64_SSE_MOVQ_G2Xrr(p, 0, fx(XmmReg::XMM5), fe(PhysReg::R11)); });
            enc_.emit([dr](std::uint8_t* p){ return fe64_SSE_PXORrr(p, 0, fx(dr), fx(XmmReg::XMM5)); });
            storeIfSpilledXmm(d, dr);
            return true;
        }
        case MOpcode::FCmp: {
            // ucomisd/ucomiss use0, use1  (sets ZF/PF/CF for the SetCC that follows).
            const MOperand& a = inst.operands[0];
            const MOperand& b = inst.operands[1];
            XmmReg ar = physOfXmm(a, XmmReg::XMM4, /*isUse=*/true);
            XmmReg br = physOfXmm(b, XmmReg::XMM5, /*isUse=*/true);
            const bool f32 = inst.width == 4;
            enc_.emit([ar, br, f32](std::uint8_t* p){
                return f32 ? fe64_SSE_UCOMISSrr(p, 0, fx(ar), fx(br))
                           : fe64_SSE_UCOMISDrr(p, 0, fx(ar), fx(br)); });
            return true;
        }
        case MOpcode::CvtI2F: {
            // def0(xmm) = (float) use1(gpr).  width selects dest precision:
            // cvtsi2sd (f64) or cvtsi2ss (f32), 64-bit GP source.
            const MOperand& d = inst.operands[0];
            const MOperand& s = inst.operands[1];
            PhysReg sr = physOf(s, PhysReg::R11, /*isUse=*/true);
            XmmReg dr = physOfXmm(d, XmmReg::XMM4, /*isUse=*/false);
            const bool f32 = inst.width == 4;
            enc_.emit([dr, sr, f32](std::uint8_t* p){
                return f32 ? fe64_SSE_CVTSI2SS64rr(p, 0, fx(dr), fe(sr))
                           : fe64_SSE_CVTSI2SD64rr(p, 0, fx(dr), fe(sr)); });
            storeIfSpilledXmm(d, dr);
            return true;
        }
        case MOpcode::CvtF2I: {
            // def0(gpr) = (i64) use1(xmm), truncating.  width selects SOURCE
            // precision: cvttsd2si (f64) or cvttss2si (f32).
            const MOperand& d = inst.operands[0];
            const MOperand& s = inst.operands[1];
            XmmReg sr = physOfXmm(s, XmmReg::XMM5, /*isUse=*/true);
            PhysReg dr = physOf(d, PhysReg::R10, /*isUse=*/false);
            const bool f32 = inst.width == 4;
            enc_.emit([dr, sr, f32](std::uint8_t* p){
                return f32 ? fe64_SSE_CVTTSS2SI64rr(p, 0, fe(dr), fx(sr))
                           : fe64_SSE_CVTTSD2SI64rr(p, 0, fe(dr), fx(sr)); });
            storeIfSpilled(d, dr);
            return true;
        }
        case MOpcode::CvtF2F: {
            // def0(xmm) = convert use1(xmm) between precisions. width selects the
            // DESTINATION: width==8 -> cvtss2sd (f32->f64), width==4 -> cvtsd2ss.
            const MOperand& d = inst.operands[0];
            const MOperand& s = inst.operands[1];
            XmmReg sr = physOfXmm(s, XmmReg::XMM5, /*isUse=*/true);
            XmmReg dr = physOfXmm(d, XmmReg::XMM4, /*isUse=*/false);
            const bool toF32 = inst.width == 4;
            enc_.emit([dr, sr, toF32](std::uint8_t* p){
                return toF32 ? fe64_SSE_CVTSD2SSrr(p, 0, fx(dr), fx(sr))
                             : fe64_SSE_CVTSS2SDrr(p, 0, fx(dr), fx(sr)); });
            storeIfSpilledXmm(d, dr);
            return true;
        }
        case MOpcode::CvtF16ToF32: {
            // def0(xmm:f32) = vcvtph2ps use1(xmm with f16 in low 16 bits).
            const MOperand& d = inst.operands[0];
            const MOperand& s = inst.operands[1];
            XmmReg sr = physOfXmm(s, XmmReg::XMM5, /*isUse=*/true);
            XmmReg dr = physOfXmm(d, XmmReg::XMM4, /*isUse=*/false);
            enc_.emit([dr, sr](std::uint8_t* p){
                return fe64_VCVTPH2PS128rr(p, 0, fx(dr), fx(sr)); });
            storeIfSpilledXmm(d, dr);
            return true;
        }
        case MOpcode::CvtF32ToF16: {
            // def0(xmm: f16 in low 16 bits) = vcvtps2ph use1(xmm:f32), imm=0
            // (round to nearest even).
            const MOperand& d = inst.operands[0];
            const MOperand& s = inst.operands[1];
            XmmReg sr = physOfXmm(s, XmmReg::XMM5, /*isUse=*/true);
            XmmReg dr = physOfXmm(d, XmmReg::XMM4, /*isUse=*/false);
            enc_.emit([dr, sr](std::uint8_t* p){
                return fe64_VCVTPS2PH128rri(p, 0, fx(dr), fx(sr), 0); });
            storeIfSpilledXmm(d, dr);
            return true;
        }
        case MOpcode::StoreXmmLo16: {
            // [rbp+off] = low 16 bits of use1(xmm). Spills a raw incoming
            // packed-half argument register into a local slot, no conversion.
            std::int32_t off = slotOffset(inst.operands[0].frameSlot);
            const MOperand& s = inst.operands[1];
            XmmReg sr = physOfXmm(s, XmmReg::XMM5, /*isUse=*/true);
            enc_.emit([sr](std::uint8_t* p){
                return fe64_SSE_MOVD_X2Grr(p, 0, fe(PhysReg::R11), fx(sr)); });
            enc_.emit([off](std::uint8_t* p){
                return fe64_MOV16mr(p, 0, rbpMem(off), fe(PhysReg::R11)); });
            return true;
        }
        case MOpcode::FMovToGpr: {
            const MOperand& d = inst.operands[0];
            const MOperand& s = inst.operands[1];
            XmmReg sr = physOfXmm(s, XmmReg::XMM5, /*isUse=*/true);
            PhysReg dr = physOf(d, PhysReg::R10, /*isUse=*/false);
            enc_.emit([dr, sr](std::uint8_t* p){ return fe64_SSE_MOVQ_X2Grr(p, 0, fe(dr), fx(sr)); });
            storeIfSpilled(d, dr);
            return true;
        }
        case MOpcode::FMovFromGpr: {
            const MOperand& d = inst.operands[0];
            const MOperand& s = inst.operands[1];
            PhysReg sr = physOf(s, PhysReg::R11, /*isUse=*/true);
            XmmReg dr = physOfXmm(d, XmmReg::XMM4, /*isUse=*/false);
            enc_.emit([dr, sr](std::uint8_t* p){ return fe64_SSE_MOVQ_G2Xrr(p, 0, fx(dr), fe(sr)); });
            storeIfSpilledXmm(d, dr);
            return true;
        }
        case MOpcode::PXorRR: {
            // usedef0(xmm) ^= use1(xmm)   (pxor, 128-bit)
            const MOperand& d = inst.operands[0];
            const MOperand& s = inst.operands[1];
            XmmReg dr = physOfXmm(d, XmmReg::XMM4, /*isUse=*/true);
            XmmReg sr = physOfXmm(s, XmmReg::XMM5, /*isUse=*/true);
            enc_.emit([dr, sr](std::uint8_t* p){ return fe64_SSE_PXORrr(p, 0, fx(dr), fx(sr)); });
            storeIfSpilledXmm(d, dr);
            return true;
        }
        case MOpcode::AesEncRR: {
            // usedef0(xmm) = one AES round of xmm0 using round key use1(xmm)
            const MOperand& d = inst.operands[0];
            const MOperand& s = inst.operands[1];
            XmmReg dr = physOfXmm(d, XmmReg::XMM4, /*isUse=*/true);
            XmmReg sr = physOfXmm(s, XmmReg::XMM5, /*isUse=*/true);
            enc_.emit([dr, sr](std::uint8_t* p){ return fe64_AESENCrr(p, 0, fx(dr), fx(sr)); });
            storeIfSpilledXmm(d, dr);
            return true;
        }
    }
    errorOut = "lowering: unknown opcode";
    return false;
}

}  // namespace Backend



