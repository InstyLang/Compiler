#include <backend/module_emit.hpp>

#include <cstring>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

#include <backend/coff_writer.hpp>
#include <backend/const_eval.hpp>
#include <backend/elf_writer.hpp>
#include <backend/macho_writer.hpp>
#include <backend/pe_writer.hpp>
#include <backend/isel.hpp>
#include <backend/lower.hpp>
#include <backend/machine_code.hpp>
#include <backend/machine_ir.hpp>
#include <backend/mir_opt.hpp>
#include <sema/checker.hpp>
#include <backend/regalloc.hpp>
#include <backend/wasm_emit.hpp>
#include <utilities/string_hash.hpp>

namespace Backend {

namespace {

// Emits module-level global variables into .data (constant integer initializers,
// folded into raw bytes) or .bss (everything else: zero-initialized). Each global
// gets a data symbol (Global binding when exported, else Local). Mirrors the LLVM
// path (core.cpp:386-419): integer-constant initializers are honored; floats/
// strings/structs/arrays/non-constant expressions are zero-initialized.
void emitGlobals(const Sema::SemaResult& sema, const AST::ProgramRoot* program,
                 MachineCode& code) {
    if (sema.globals.empty()) return;

    // Map global name -> its AST initializer (SemaResult::GlobalInfo drops it).
    std::unordered_map<std::string, const AST::VariableDeclarationExpr*> initByName;
    if (program) {
        for (const auto& node : program->body) {
            if (!node || node->nodeType() != AST::NodeType::VariableDeclaration) continue;
            const auto* vd = static_cast<const AST::VariableDeclarationExpr*>(node.get());
            initByName[vd->identifier] = vd;
        }
    }

    for (const auto& g : sema.globals) {
        if (!g.type || g.type->isError()) continue;
        SizeAlign sa = scalarSizeAlign(g.type);
        if (sa.size == 0) sa.size = 1;
        const SymbolBinding binding =
            g.isExported ? SymbolBinding::Global : SymbolBinding::Local;

        // Fold a constant integer initializer of an integer/pointer/bool scalar
        // into .data; anything else is zero-initialized in .bss.
        __int128 constValue = 0;
        bool haveConst = false;
        auto it = initByName.find(g.name);
        const bool scalarInt = g.type->kind == Types::Kind::Int ||
                               g.type->kind == Types::Kind::Bool ||
                               g.type->kind == Types::Kind::Pointer;
        if (scalarInt && it != initByName.end() && it->second->initialValue) {
            haveConst = evalConstInt(it->second->initialValue.get(), constValue);
        }

        if (haveConst) {
            Section& data = code.data;
            if (data.alignment < sa.align) data.alignment = sa.align;
            // Align the section cursor for this symbol.
            while (data.bytes.size() % sa.align != 0) data.bytes.push_back(0);
            std::uint64_t off = data.bytes.size();
            // Emit the initializer little-endian, honoring up to 16 bytes so
            // 128-bit (i128/u128) globals keep their high word.
            unsigned __int128 raw = static_cast<unsigned __int128>(constValue);
            for (unsigned b = 0; b < sa.size && b < 16; ++b) {
                data.bytes.push_back(static_cast<std::uint8_t>((raw >> (8 * b)) & 0xFF));
            }
            for (unsigned b = 16; b < sa.size; ++b) data.bytes.push_back(0);
            code.defineSymbol(g.name, SectionKind::Data, off, binding,
                              /*isFunction=*/false);
        } else {
            Section& bss = code.bss;
            if (bss.alignment < sa.align) bss.alignment = sa.align;
            while (bss.bssSize % sa.align != 0) ++bss.bssSize;
            std::uint64_t off = bss.bssSize;
            bss.bssSize += sa.size;
            code.defineSymbol(g.name, SectionKind::Bss, off, binding,
                              /*isFunction=*/false);
        }
    }
}

// Builds a startup-shim MFunction named `shimName` that calls `main` and then
// terminates the process, for one of the three flavours below. The shim is
// hand-built as Machine IR and run through the normal register-allocate + lower
// pipeline, so it reuses Call/CallImport/Syscall lowering, import tables and
// relocations. `mainParamCount` is main's arity (only EFI forwards args).
//
//  - Pe   : Win64 entry. call main; ExitProcess(main()'s i32 return). Reserves
//           shadow space for both calls. Never returns.
//  - Elf  : Linux SysV entry. Align rsp to 16; call main; exit(main()&0xFF) via
//           syscall 60. Never returns.
//  - Efi  : UEFI entry efi_main(handle, systemTable). Forward both args to main
//           when it takes 2 params, else call main(); return its i64 status.
enum class ShimKind { Pe, Elf, Efi };

std::string functionSymbol(const Sema::FunctionInfo& info) {
    return info.mangledName.empty() ? info.name : info.mangledName;
}

std::unique_ptr<MFunction> buildEntryShim(const std::string& shimName,
                                          const std::string& mainSymbol,
                                          std::size_t mainParamCount,
                                          ShimKind kind, const AbiInfo& abi) {
    auto fn = std::make_unique<MFunction>(shimName, abi.abi);
    fn->addBlock();  // entry block (index 0); Lowering emits the prologue.

    auto emit = [&](MInst inst) { fn->block(0).insts.push_back(std::move(inst)); };

    if (kind == ShimKind::Efi) {
        // efi_main(handle in arg0, systemTable in arg1). Forward to main if it
        // takes two parameters; otherwise call main(). Reserve Win64 shadow
        // space for the call.
        fn->noteOutgoingArgBytes(static_cast<std::int64_t>(abi.shadowSpace));
        if (mainParamCount >= 2) {
            // Capture the two incoming arg registers, then place them back as the
            // outgoing args (a no-op move that keeps the model uniform and lets
            // regalloc see the live range across the call setup).
            VReg h = fn->newVReg();
            VReg st = fn->newVReg();
            emit({MOpcode::MovRR, {MOperand::defVReg(h), MOperand::usePhys(abi.intArgRegs[0])}});
            emit({MOpcode::MovRR, {MOperand::defVReg(st), MOperand::usePhys(abi.intArgRegs[1])}});
            emit({MOpcode::MovRR, {MOperand::defPhys(abi.intArgRegs[0]), MOperand::useVReg(h)}});
            emit({MOpcode::MovRR, {MOperand::defPhys(abi.intArgRegs[1]), MOperand::useVReg(st)}});
        }
        MInst call{MOpcode::Call, {MOperand::sym(mainSymbol)}};
        call.clobbers = abi.callerSaved;
        emit(call);
        // main's i64 status is already in the return register (RAX); just return.
        emit({MOpcode::Ret, {}});
        return fn;
    }

    if (kind == ShimKind::Pe) {
        // Win64: call main(); ExitProcess(eax). Reserve shadow space (used by both
        // the call to main and the ExitProcess import call).
        fn->noteOutgoingArgBytes(static_cast<std::int64_t>(abi.shadowSpace));
        MInst call{MOpcode::Call, {MOperand::sym(mainSymbol)}};
        call.clobbers = abi.callerSaved;
        emit(call);
        // ExitProcess(exit_code): move main's return (RAX) into the first arg reg.
        VReg rc = fn->newVReg();
        emit({MOpcode::MovRR, {MOperand::defVReg(rc), MOperand::usePhys(abi.intReturnReg)}});
        emit({MOpcode::MovRR, {MOperand::defPhys(abi.intArgRegs[0]), MOperand::useVReg(rc)}});
        MInst exitc{MOpcode::CallImport,
                    {MOperand::sym("ExitProcess"), MOperand::sym("kernel32.dll")}};
        exitc.clobbers = abi.callerSaved;
        emit(exitc);
        // ExitProcess never returns; a trailing ret is unreachable but harmless.
        emit({MOpcode::Ret, {}});
        return fn;
    }

    // ShimKind::Elf -- Linux SysV _start.
    fn->noteOutgoingArgBytes(0);
    MInst call{MOpcode::Call, {MOperand::sym(mainSymbol)}};
    call.clobbers = abi.callerSaved;
    emit(call);
    // exit_code = main() & 0xFF (the kernel only uses the low 8 bits anyway).
    VReg rc = fn->newVReg();
    emit({MOpcode::MovRR, {MOperand::defVReg(rc), MOperand::usePhys(abi.intReturnReg)}});
    VReg mask = fn->newVReg();
    emit({MOpcode::MovRI, {MOperand::defVReg(mask), MOperand::immediate(0xFF)}});
    emit({MOpcode::And, {MOperand::useDefVReg(rc), MOperand::useVReg(mask)}});
    emit({MOpcode::MovRR, {MOperand::defPhys(PhysReg::RDI), MOperand::useVReg(rc)}});
    // syscall number 60 = exit, in RAX.
    VReg num = fn->newVReg();
    emit({MOpcode::MovRI, {MOperand::defVReg(num), MOperand::immediate(60)}});
    emit({MOpcode::MovRR, {MOperand::defPhys(PhysReg::RAX), MOperand::useVReg(num)}});
    MInst sys{MOpcode::Syscall, {}};
    sys.clobbers = abi.callerSaved;
    emit(sys);
    emit({MOpcode::Ret, {}});
    return fn;
}

// Hand-built AES string-hash runtime helper (hosted x86-64 fast path):
//   __ins_hash_bytes_aes(u8* ptr, u64 len) -> u64
// Kept bit-for-bit identical to Hashing::aesStringHash (include/utilities/
// string_hash.hpp), which is used to fold string-literal hashes at compile time.
// The state lives entirely in pinned XMM/GP registers (no vregs -> regalloc is a
// no-op and the 128-bit-spill hazard is avoided). See string_hash.hpp for the
// algorithm. Uses only caller-saved registers on both SysV and Win64
// (GP: RAX/RCX/RDX/R8/R9/R10/R11; XMM: XMM0-2), so no callee-saves are needed.
std::unique_ptr<MFunction> buildAesHashFn(const AbiInfo& abi) {
    using X = XmmReg;
    using R = PhysReg;
    auto fn = std::make_unique<MFunction>("__ins_hash_bytes_aes", abi.abi);
    fn->linkage = MFunction::Linkage::Internal;  // private copy per object; no clash

    std::uint32_t b0 = fn->addBlock("entry");
    std::uint32_t bHdr = fn->addBlock();
    std::uint32_t bBody = fn->addBlock();
    std::uint32_t bTailSetup = fn->addBlock();
    std::uint32_t bTailHdr = fn->addBlock();
    std::uint32_t bTailBody = fn->addBlock();
    std::uint32_t bFinal = fn->addBlock();

    auto E = [&](std::uint32_t blk, MInst inst) {
        fn->block(blk).insts.push_back(std::move(inst));
    };
    // 16-byte tail scratch buffer.
    std::uint32_t bufSlot = fn->addFrameSlot(16, 16, /*isSpill=*/false);
    // SEED constant in .rodata (same 16 bytes as Hashing::kAesSeed).
    fn->addRawConstant("__ins_aes_seed",
                       std::string(reinterpret_cast<const char*>(Hashing::kAesSeed), 16),
                       /*align=*/16);

    auto fload16 = [](R base, XmmReg dst) {
        MInst m{MOpcode::FLoadInd,
                {MOperand::defPhysXmm(dst), MOperand::usePhys(base), MOperand::immediate(0)}};
        m.width = 16; m.isSigned = false; return m;
    };

    // --- entry: seed state = (len in low 64) ^ SEED; end = ptr + len ----------
    E(b0, {MOpcode::MovRR, {MOperand::defPhys(R::R8), MOperand::usePhys(abi.intArgRegs[0])}});  // cur = ptr
    E(b0, {MOpcode::MovRR, {MOperand::defPhys(R::R9), MOperand::usePhys(abi.intArgRegs[1])}});  // len
    E(b0, {MOpcode::Lea, {MOperand::defPhys(R::R11), MOperand::sym("__ins_aes_seed")}});
    E(b0, fload16(R::R11, X::XMM2));                                    // XMM2 = SEED
    E(b0, {MOpcode::MovRR, {MOperand::defPhys(R::R10), MOperand::usePhys(R::R8)}});             // end = ptr
    E(b0, {MOpcode::Add, {MOperand::usePhys(R::R10), MOperand::usePhys(R::R9)}});               // end += len
    E(b0, {MOpcode::FMovFromGpr, {MOperand::defPhysXmm(X::XMM0), MOperand::usePhys(R::R9)}});   // xmm0 = len
    E(b0, {MOpcode::PXorRR, {MOperand::usePhysXmm(X::XMM0), MOperand::usePhysXmm(X::XMM2)}});   // state ^= SEED
    E(b0, {MOpcode::MovRI, {MOperand::defPhys(R::R9), MOperand::immediate(16)}});               // R9 = 16
    E(b0, {MOpcode::Jmp, {MOperand::lbl(bHdr)}});

    // --- block loop: while (end - cur) >= 16 ----------------------------------
    E(bHdr, {MOpcode::MovRR, {MOperand::defPhys(R::R11), MOperand::usePhys(R::R10)}});   // tmp = end
    E(bHdr, {MOpcode::Sub, {MOperand::usePhys(R::R11), MOperand::usePhys(R::R8)}});      // tmp = remaining
    E(bHdr, {MOpcode::Cmp, {MOperand::usePhys(R::R11), MOperand::usePhys(R::R9)}});      // remaining ? 16
    { MInst j{MOpcode::Jcc, {MOperand::lbl(bTailSetup)}}; j.cond = Cond::ULT; E(bHdr, j); }
    E(bHdr, {MOpcode::Jmp, {MOperand::lbl(bBody)}});

    E(bBody, fload16(R::R8, X::XMM1));                                                   // block = [cur]
    E(bBody, {MOpcode::AesEncRR, {MOperand::usePhysXmm(X::XMM0), MOperand::usePhysXmm(X::XMM1)}});
    E(bBody, {MOpcode::Add, {MOperand::usePhys(R::R8), MOperand::usePhys(R::R9)}});      // cur += 16
    E(bBody, {MOpcode::Jmp, {MOperand::lbl(bHdr)}});

    // --- tail setup: zero the 16-byte buffer; RDX = 1 (byte-copy stride) -------
    E(bTailSetup, {MOpcode::LeaSlot, {MOperand::defPhys(R::RAX), MOperand::slot(bufSlot)}});   // dstp = &buf
    E(bTailSetup, {MOpcode::MovRI, {MOperand::defPhys(R::RCX), MOperand::immediate(0)}});
    { MInst s{MOpcode::StoreInd, {MOperand::usePhys(R::RAX), MOperand::immediate(0), MOperand::usePhys(R::RCX)}}; s.width = 8; E(bTailSetup, s); }
    { MInst s{MOpcode::StoreInd, {MOperand::usePhys(R::RAX), MOperand::immediate(8), MOperand::usePhys(R::RCX)}}; s.width = 8; E(bTailSetup, s); }
    E(bTailSetup, {MOpcode::MovRI, {MOperand::defPhys(R::RDX), MOperand::immediate(1)}});
    E(bTailSetup, {MOpcode::Jmp, {MOperand::lbl(bTailHdr)}});

    // --- tail copy loop: copy bytes [cur, end) into buf; dstp advances in RAX --
    E(bTailHdr, {MOpcode::Cmp, {MOperand::usePhys(R::R8), MOperand::usePhys(R::R10)}});  // srcp ? end
    { MInst j{MOpcode::Jcc, {MOperand::lbl(bFinal)}}; j.cond = Cond::EQ; E(bTailHdr, j); }
    E(bTailHdr, {MOpcode::Jmp, {MOperand::lbl(bTailBody)}});

    { MInst ld{MOpcode::LoadInd, {MOperand::defPhys(R::R11), MOperand::usePhys(R::R8), MOperand::immediate(0)}}; ld.width = 1; ld.isSigned = false; E(bTailBody, ld); }
    { MInst st{MOpcode::StoreInd, {MOperand::usePhys(R::RAX), MOperand::immediate(0), MOperand::usePhys(R::R11)}}; st.width = 1; E(bTailBody, st); }
    E(bTailBody, {MOpcode::Add, {MOperand::usePhys(R::R8), MOperand::usePhys(R::RDX)}});  // srcp++
    E(bTailBody, {MOpcode::Add, {MOperand::usePhys(R::RAX), MOperand::usePhys(R::RDX)}}); // dstp++
    E(bTailBody, {MOpcode::Jmp, {MOperand::lbl(bTailHdr)}});

    // --- finalize: aesenc(state, tail); aesenc(state, SEED) x2; return low64 ---
    E(bFinal, {MOpcode::LeaSlot, {MOperand::defPhys(R::R11), MOperand::slot(bufSlot)}});  // &buf
    E(bFinal, fload16(R::R11, X::XMM1));                                                  // tail = buf
    E(bFinal, {MOpcode::AesEncRR, {MOperand::usePhysXmm(X::XMM0), MOperand::usePhysXmm(X::XMM1)}});
    E(bFinal, {MOpcode::AesEncRR, {MOperand::usePhysXmm(X::XMM0), MOperand::usePhysXmm(X::XMM2)}});
    E(bFinal, {MOpcode::AesEncRR, {MOperand::usePhysXmm(X::XMM0), MOperand::usePhysXmm(X::XMM2)}});
    E(bFinal, {MOpcode::FMovToGpr, {MOperand::defPhys(abi.intReturnReg), MOperand::usePhysXmm(X::XMM0)}});
    E(bFinal, {MOpcode::Ret, {}});
    return fn;
}

// Register-allocates and lowers one MFunction, appending its body to `code`.
//
// This is the single seam between the target-neutral and x86-64-specific halves
// of the backend. Everything above it (instruction selection, mir_opt) produces
// Machine IR in virtual-register form; LinearScanAllocator and Lowering below it
// are x86-64 only. A second backend -- see ObjectFormat::Wasm -- diverges here,
// consuming the same post-optimization MFunction and mapping virtual registers
// to wasm locals instead of physical registers.
//
// `optimize` is false for hand-built MFunctions (the entry shims and the AES
// hash helper), whose pinned physical registers must survive verbatim.
bool emitFunctionBody(MFunction& fn, MachineCode& code, const AbiInfo& abiInfo,
                      int optLevel, bool optimize, std::string& err) {
    if (optimize) {
        // Run while the function is still in virtual-register form so the
        // allocator sees less pressure.
        optimizeFunction(fn, optLevel);
    }
    LinearScanAllocator ra(abiInfo);
    Allocation alloc = ra.run(fn);
    Lowering low(code, abiInfo);
    return low.emit(fn, alloc, err);
}

}  // namespace

bool emitModuleObject(const Sema::SemaResult& sema, Abi abi, ObjectFormat format,
                      const std::string& outPath, std::string& errorOut,
                      const std::string& entrySymbol,
                      const AST::ProgramRoot* program,
                      const Sema::SemaResult* runtimeModule,
                      EntryShim shim, int optLevel, bool instantOsSyscalls,
                      bool preferHostedEntry,
                      std::vector<std::string>* requiredLibsOut,
                      bool boundsCheck, bool aesHash,
                      const Targeting::TargetSpec* target) {
    // WebAssembly diverges before instruction selection. Everything below --
    // register allocation, x86 lowering, the ELF/COFF/Mach-O writers -- assumes
    // an x86-64 machine, so the wasm path runs its own selection loop and emits
    // a complete, self-contained module (no linker step, as with whole-program
    // PE). See wasm_emit.hpp.
    if (format == ObjectFormat::Wasm) {
        Wasm::EmitOptions wasmOptions;
        wasmOptions.target = target;
        wasmOptions.program = program;
        wasmOptions.runtimeModule = runtimeModule;
        wasmOptions.optLevel = optLevel;
        wasmOptions.boundsCheck = boundsCheck;
        return Wasm::emitWasmModule(sema, wasmOptions, outPath, errorOut);
    }

    const AbiInfo abiInfo = makeAbi(abi);
    InstructionSelector isel(sema, abi, instantOsSyscalls, boundsCheck, aesHash);
    MachineCode code;

    // Module-level globals first: they define data symbols that function bodies
    // reference by name (a RIP-relative Lea/Load/Store against the symbol).
    emitGlobals(sema, program, code);

    // Selects, register-allocates and lowers one function into `code`, appending
    // its body to .text. Returns false (and fills `errorOut`) on failure.
    std::set<std::string> emittedSymbols;
    unsigned emitted = 0;
    auto selectAndLower = [&](InstructionSelector& sel,
                              const Sema::FunctionInfo& info) -> bool {
        const std::string symbol = functionSymbol(info);
        if (!emittedSymbols.insert(symbol).second) {
            return true;
        }
        std::string err;
        auto fn = sel.select(info, err);
        if (!fn) {
            errorOut = "backend: failed to select '" +
                       (info.name.empty() ? info.mangledName : info.name) + "': " + err;
            return false;
        }
        // A monomorphized generic instantiation may be emitted by several modules
        // that use the same instance; give it weak linkage so the linker folds the
        // duplicate definitions instead of erroring.
        if (info.isGenericInstance) {
            fn->linkage = MFunction::Linkage::LinkOnce;
        }
        if (!emitFunctionBody(*fn, code, abiInfo, optLevel, /*optimize=*/true, err)) {
            errorOut = "backend: failed to lower '" +
                       (info.name.empty() ? info.mangledName : info.name) + "': " + err;
            return false;
        }
        ++emitted;
        return true;
    };

    // Select + allocate + lower every analyzed function that has a body. Each
    // function appends to the shared MachineCode (its symbol is defined at the
    // current end of .text; forward/mutual calls reference symbols by name and
    // are promoted in place when the callee is later lowered).
    for (const auto& info : sema.functions) {
        if (!info.decl || !info.decl->hasBody || info.isExternal) continue;
        if (!selectAndLower(isel, info)) return false;
    }

    // Generic function instantiations are checked by sema but are not appended
    // to sema.functions: they reuse the template declaration body with concrete
    // param/return types and a concrete symbol name. Emit those bodies now so
    // direct calls such as id<i32>(...) resolve in final PE/ELF/Mach-O output.
    for (const auto& inst : sema.genericInstantiations) {
        if (!inst.templateDecl || !inst.templateDecl->hasBody) continue;
        Sema::FunctionInfo info;
        info.name = inst.mangledName;
        info.mangledName = inst.mangledName;
        info.paramTypes = inst.paramTypes;
        info.paramNames = inst.paramNames;
        info.returnType = inst.returnType;
        // This instantiation's own copy of the body: its nodes carry the types
        // sema recorded for this instantiation.
        info.decl = inst.decl ? inst.decl.get() : inst.templateDecl;
        info.isExternal = false;
        info.isExported = false;
        info.isUnsafe = false;
        info.isGenericInstance = true;  // weak: may be emitted by several modules
        for (const auto& attr : inst.templateDecl->attributes) {
            if (attr.name == "unsafe" && (attr.value.empty() || attr.value == "on")) {
                info.isUnsafe = true;
                break;
            }
        }
        if (!selectAndLower(isel, info)) return false;
    }

    // Class methods/constructors/operators are not free functions: their
    // FunctionInfo.decl is null (so they are skipped above) and their body lives
    // on the AST::Method, reachable only by walking the class declarations. Pair
    // each method with its resolved FunctionInfo (by mangled name) and select it
    // with `this` as the implicit first parameter.
    if (program) {
        // mangledName -> FunctionInfo for fast pairing.
        std::unordered_map<std::string, const Sema::FunctionInfo*> byMangled;
        for (const auto& fi : sema.functions) {
            const std::string& key = fi.mangledName.empty() ? fi.name : fi.mangledName;
            if (!key.empty()) byMangled[key] = &fi;
        }
        // Lower a pre-built MFunction (method bodies don't go through sel.select).
        auto lowerFn = [&](std::unique_ptr<MFunction> fn, const std::string& what) -> bool {
            if (!fn) return false;
            if (!emittedSymbols.insert(what).second) {
                return true;
            }
            std::string err;
            if (!emitFunctionBody(*fn, code, abiInfo, optLevel, /*optimize=*/true, err)) {
                errorOut = "backend: failed to lower '" + what + "': " + err;
                return false;
            }
            ++emitted;
            return true;
        };
        for (const auto& node : program->body) {
            if (!node || node->nodeType() != AST::NodeType::ClassDeclaration) continue;
            const auto& cls = static_cast<const AST::ClassDeclaration&>(*node);
            if (!cls.genericParams.empty()) continue;  // generic templates aren't emitted
            for (const auto& method : cls.methods) {
                // Recompute the mangled name exactly as sema did, to find the
                // matching FunctionInfo (which carries paramTypes incl. `this`).
                std::vector<std::string> paramSpellings;
                for (const auto& p : method.parameters) paramSpellings.push_back(p.type);
                const std::string mangled = Sema::mangleClassMember(
                    cls.name, method.name, method.isConstructor, method.isOperator,
                    method.operatorSymbol, paramSpellings);
                auto it = byMangled.find(mangled);
                if (it == byMangled.end()) continue;  // not analyzed (e.g. unused generic)
                std::string err;
                auto fn = isel.selectMethod(method, *it->second, err);
                if (!fn) {
                    errorOut = "backend: failed to select method '" + mangled + "': " + err;
                    return false;
                }
                if (!lowerFn(std::move(fn), mangled)) return false;
            }
        }

        // Generic class instantiations also use template AST method bodies, but
        // sema creates concrete FunctionInfo records with concrete class names,
        // this pointers, parameter types and member symbols. Pair those concrete
        // infos back to the template methods and emit one body per instantiation.
        for (const auto& inst : sema.genericClassInstantiations) {
            if (!inst.templateDecl) continue;
            const auto& cls = *inst.templateDecl;

            std::unordered_map<std::string, std::string> subst;
            for (std::size_t i = 0; i < cls.genericParams.size() &&
                                    i < inst.typeArgs.size();
                 ++i) {
                subst[cls.genericParams[i]] = inst.typeArgs[i];
            }
            auto substituteAll = [&](const std::string& spelling) {
                std::string out = spelling;
                for (const auto& param : cls.genericParams) {
                    auto it = subst.find(param);
                    if (it != subst.end()) {
                        out = Sema::Checker::substituteSpelling(out, param, it->second);
                    }
                }
                return out;
            };

            // Emit this instantiation's own copies of the bodies, not the
            // template's: sema recorded each copy's types separately, so these
            // are the nodes whose `exprTypes` describe *this* instantiation.
            for (const auto& methodOwner : inst.methods) {
                if (!methodOwner) continue;
                const AST::Method& method = *methodOwner;
                std::vector<std::string> paramSpellings;
                for (const auto& p : method.parameters) {
                    paramSpellings.push_back(substituteAll(p.type));
                }
                const std::string mangled = Sema::mangleClassMember(
                    inst.mangledName, method.name, method.isConstructor,
                    method.isOperator, method.operatorSymbol, paramSpellings);
                if (emittedSymbols.find(mangled) != emittedSymbols.end()) {
                    continue;
                }
                auto it = byMangled.find(mangled);
                if (it == byMangled.end()) {
                    continue;
                }
                std::string err;
                auto fn = isel.selectMethod(method, *it->second, err);
                if (!fn) {
                    errorOut = "backend: failed to select generic method '" +
                               mangled + "': " + err;
                    return false;
                }
                // Weak: the same instantiated method may be emitted by every
                // module that uses this generic class instance.
                fn->linkage = MFunction::Linkage::LinkOnce;
                if (!lowerFn(std::move(fn), mangled)) return false;
            }
        }
    }

    // Inject any runtime helpers referenced during selection (e.g. the
    // __ins_fmt_* string formatters used by string interpolation). The selector
    // records each referenced symbol in requestedRuntime(); we look it up by
    // mangled name in `runtimeModule`, select+lower it into the same object, and
    // repeat until the worklist drains (helpers may reference each other, e.g.
    // fmt_float -> fmt_int). A separate selector is bound to the runtime module's
    // SemaResult so the helper bodies resolve against their own types/functions.
    if (!isel.requestedRuntime().empty()) {
        if (!runtimeModule) {
            errorOut = "backend: string interpolation requires the runtime module, "
                       "but none was provided to the backend";
            return false;
        }
        // mangledName -> FunctionInfo for every body-bearing runtime helper.
        std::unordered_map<std::string, const Sema::FunctionInfo*> runtimeByName;
        for (const auto& info : runtimeModule->functions) {
            if (!info.decl || !info.decl->hasBody) continue;
            const std::string& key =
                info.mangledName.empty() ? info.name : info.mangledName;
            runtimeByName[key] = &info;
        }

        InstructionSelector runtimeSel(*runtimeModule, abi, instantOsSyscalls,
                                       boundsCheck, aesHash);
        std::set<std::string> done;
        // Seed the worklist with the main-module requests.
        std::vector<std::string> work(isel.requestedRuntime().begin(),
                                      isel.requestedRuntime().end());
        while (!work.empty()) {
            std::string sym = work.back();
            work.pop_back();
            if (!done.insert(sym).second) continue;  // already emitted

            // The AES string-hash helper is hand-built (it needs SSE/AESENC that
            // the Insty runtime source cannot express), not looked up in the
            // runtime module. Build, allocate (no-op: pinned physregs only), and
            // lower it directly into this object. It has no transitive deps.
            if (sym == "__ins_hash_bytes_aes") {
                auto fn = buildAesHashFn(abiInfo);
                if (!emitFunctionBody(*fn, code, abiInfo, optLevel,
                                      /*optimize=*/false, errorOut)) {
                    return false;
                }
                continue;
            }

            auto it = runtimeByName.find(sym);
            if (it == runtimeByName.end()) {
                errorOut = "backend: required runtime helper '" + sym +
                           "' not found in the runtime module";
                return false;
            }
            if (!selectAndLower(runtimeSel, *it->second)) return false;

            // Pick up any transitive helper references discovered while lowering
            // this helper.
            for (const auto& dep : runtimeSel.requestedRuntime()) {
                if (done.find(dep) == done.end()) work.push_back(dep);
            }
        }
    }

    // A module may legitimately produce no code of its own -- e.g. one that only
    // declares generic templates (their concrete instantiations are emitted into
    // the modules that use them). Such a module still yields a valid (empty)
    // object so the driver can link it uniformly.
    (void)emitted;

    // Hosted dynamic Linux link: the program is linked against a C runtime whose
    // crt1.o `_start` calls `main` through __libc_start_main, so we must NOT emit
    // our own ELF `_start` shim (it would collide with crt1.o's and skip libc
    // initialization). This is requested either explicitly by the driver (some
    // other module in the build pulled in a shared library) or implied when this
    // module itself referenced a `lib(...)`-tagged extern.
    const bool hostedDynamic = preferHostedEntry || !isel.requiredLibs().empty();
    EntryShim effectiveShim = shim;
    if (effectiveShim == EntryShim::Elf && hostedDynamic) {
        effectiveShim = EntryShim::None;
    }

    // Synthesize the requested startup shim (if any). It calls `main` and
    // terminates the process; its symbol becomes the program entry point. Only
    // emitted when a `main` function exists in this module.
    // EntryShim::Wasi is deliberately excluded: it is not x86 Machine IR, so the
    // wasm emitter synthesizes it rather than buildEntryShim. Leaving it out of
    // this condition keeps it from falling through to the Elf shim below.
    std::string effectiveEntry = entrySymbol;
    if (effectiveShim != EntryShim::None && effectiveShim != EntryShim::Wasi) {
        const Sema::FunctionInfo* mainInfo = nullptr;
        for (const auto& info : sema.functions) {
            if (info.name == "main" && info.decl && info.decl->hasBody) {
                mainInfo = &info;
                break;
            }
        }
        if (mainInfo) {
            const std::string mainSymbol =
                mainInfo->mangledName.empty() ? mainInfo->name : mainInfo->mangledName;
            const char* shimName = (effectiveShim == EntryShim::Efi) ? "efi_main" : "_start";
            ShimKind kind = (effectiveShim == EntryShim::Pe)   ? ShimKind::Pe
                            : (effectiveShim == EntryShim::Efi) ? ShimKind::Efi
                                                                : ShimKind::Elf;
            auto shimFn = buildEntryShim(shimName, mainSymbol,
                                         mainInfo->paramTypes.size(), kind, abiInfo);
            std::string err;
            if (!emitFunctionBody(*shimFn, code, abiInfo, optLevel,
                                  /*optimize=*/false, err)) {
                errorOut = "backend: failed to lower startup shim '" +
                           std::string(shimName) + "': " + err;
                return false;
            }
            effectiveEntry = shimName;
        }
    }

    // Report the shared libraries referenced externs asked to be linked against
    // (via `lib(...)`), so the driver can add them to the ELF link line.
    if (requiredLibsOut) {
        for (const auto& lib : isel.requiredLibs()) {
            requiredLibsOut->push_back(lib);
        }
    }

    bool ok = false;
    if (format == ObjectFormat::Pe) {
        const std::uint16_t subsystem = (shim == EntryShim::Efi)
                                            ? PeWriter::kSubsystemEfiApplication
                                            : PeWriter::kSubsystemConsole;
        ok = PeWriter::write(code, outPath, effectiveEntry, errorOut, subsystem);
    } else if (format == ObjectFormat::Coff) {
        ok = CoffWriter::write(code, outPath, errorOut);
    } else if (format == ObjectFormat::MachO) {
        ok = MachOWriter::write(code, outPath, errorOut);
    } else if (format == ObjectFormat::Elf) {
        ok = ElfWriter::write(code, outPath, errorOut);
    } else {
        // Unreachable: ObjectFormat::Wasm returns early above. Explicit so that
        // adding a container format cannot silently emit an ELF object.
        errorOut = "backend: no writer for the requested object format";
        return false;
    }
    if (!ok && errorOut.empty()) {
        errorOut = "backend: object writer failed";
    }
    return ok;
}

}  // namespace Backend

