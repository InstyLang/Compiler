#include <codegen/codegen.hpp>
#include <codegen/codegen_internal.hpp>

#include <parser/parser.hpp>
#include <sema/sema.hpp>
#include <utilities/errors.hpp>

#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/Casting.h>

#include <memory>
#include <string>


namespace {

const char* kCoreSource = R"Insty(
module core

fun [name(__ins_memcpy)] memcpy(u8* dest, u8* src, u64 size) -> void {
    unsafe {
        u64 i = 0
        while i < size {
            dest[i] = src[i]
            i = i + 1
        }
    }
}

fun [name(__ins_memset)] memset(u8* ptr, u8 value, u64 size) -> void {
    unsafe {
        u64 i = 0
        while i < size {
            ptr[i] = value
            i = i + 1
        }
    }
}

// --- string interpolation formatters -------------------------------------
// Cursor convention: append into `buf` starting at `off`, never writing at or
// past `cap`, returning the new offset (may equal cap once full).

// Copy up to (cap-off) of the first `n` bytes of `src` into buf at off.
fun [name(__ins_fmt_raw)] fmt_raw(u8* buf, i64 cap, i64 off, u8* src, i64 n) -> i64 {
    unsafe {
        i64 i = 0
        i64 o = off
        while i < n {
            if o >= cap {
                return o
            }
            buf[o] = src[i]
            i = i + 1
            o = o + 1
        }
        return o
    }
}

// Append the decimal representation of `n` (signed when is_signed is true).
fun [name(__ins_fmt_int)] fmt_int(u8* buf, i64 cap, i64 off, i64 n, bool is_signed) -> i64 {
    unsafe {
        u8[32] scratch
        u8* digits = cast<u8*>(&scratch)
        i64 v = n
        bool neg = false
        if is_signed {
            if v < 0 {
                neg = true
                v = 0 - v
            }
        }
        // Generate digits (reversed). At least one digit (handles 0).
        i64 di = 0
        bool more = true
        while more {
            i64 q = cast<i64>(cast<u64>(v) / 10)
            i64 r = cast<i64>(cast<u64>(v) - cast<u64>(q) * 10)
            digits[di] = cast<u8>(48 + r)
            di = di + 1
            v = q
            if v == 0 {
                more = false
            }
        }
        i64 o = off
        if neg {
            if o < cap {
                buf[o] = 45
                o = o + 1
            }
        }
        // Emit digits in forward order (reverse of scratch).
        i64 k = di
        while k > 0 {
            if o >= cap {
                return o
            }
            k = k - 1
            buf[o] = digits[k]
            o = o + 1
        }
        return o
    }
}

// Append `v` with a fixed 6 fractional decimal places.
fun [name(__ins_fmt_float)] fmt_float(u8* buf, i64 cap, i64 off, f64 v) -> i64 {
    unsafe {
        f64 a = v
        i64 o = off
        if a < 0.0 {
            a = 0.0 - a
            if o < cap {
                buf[o] = 45
                o = o + 1
            }
        }
        i64 ipart = cast<i64>(a)
        o = fmt_int(buf, cap, o, ipart, false)
        if o < cap {
            buf[o] = 46
            o = o + 1
        }
        f64 frac = a - cast<f64>(ipart)
        i64 scaled = cast<i64>(frac * 1000000.0 + 0.5)
        // Emit exactly 6 fractional digits with leading zeros.
        i64 divisor = 100000
        while divisor > 0 {
            if o >= cap {
                return o
            }
            i64 dgt = cast<i64>(cast<u64>(scaled) / cast<u64>(divisor))
            i64 rem = cast<i64>(cast<u64>(scaled) - cast<u64>(dgt) * cast<u64>(divisor))
            buf[o] = cast<u8>(48 + dgt)
            o = o + 1
            scaled = rem
            divisor = cast<i64>(cast<u64>(divisor) / 10)
        }
        return o
    }
}

// Finalize: on overflow stamp a ".." marker, then NUL-terminate within bounds.
fun [name(__ins_fmt_finish)] fmt_finish(u8* buf, i64 cap, i64 off) -> void {
    unsafe {
        if off >= cap {
            buf[cap - 1] = 46
            buf[cap - 2] = 46
            buf[cap] = 0
            return
        }
        buf[off] = 0
    }
}

// --- allocator ------------------------------------------------------------
// Two backends select the actual __ins_alloc body at codegen time:
//   core_alloc_brk   - Linux, sbrk-style bump allocator via @syscall(brk)
//   core_alloc_mmap  - InstantOS, mmap via raw asm with its register convention
// free/realloc are target-agnostic. realloc calls the generic __ins_alloc
// (declared below, defined by whichever backend the compiler emits) plus
// __ins_memcpy / __ins_free.

// Generic allocate symbol; the body is provided by the selected backend.
fun [name(__ins_alloc)] alloc(u64 size, u64 align) -> u8*

// Linux brk bump allocator. brk syscall is number 12. asm<i64> performs the
// syscall and yields rax; the standard x86_64 convention is rax=num, rdi=arg0.
fun [name(core_alloc_brk)] alloc_brk(u64 size, u64 align) -> u8* {
    unsafe {
        u64 eff_size = size
        if eff_size == 0 {
            eff_size = 1
        }
        u64 eff_align = align
        if eff_align < 16 {
            eff_align = 16
        }
        // Round eff_size up to a multiple of eff_align without bitwise-not.
        u64 aligned = ((eff_size + eff_align - 1) / eff_align) * eff_align

        // Query the current program break: brk(0).
        i64 old_break = asm<i64>("syscall", "={rax},{rax},{rdi},~{rcx},~{r11},~{memory}", 12, 0)
        u64 ub = cast<u64>(old_break)
        u64 new_break = ub + aligned
        if new_break < ub {
            return cast<u8*>(0)
        }
        i64 ret = asm<i64>("syscall", "={rax},{rax},{rdi},~{rcx},~{r11},~{memory}", 12, cast<i64>(new_break))
        if cast<u64>(ret) == new_break {
            return cast<u8*>(ub)
        }
        return cast<u8*>(0)
    }
}

// InstantOS mmap allocator. Raw syscall with the InstantOS register convention
// (rax=num, rbx=addr, r10=len, rdx=prot); syscall number 12.
fun [name(core_alloc_mmap)] alloc_mmap(u64 size, u64 align) -> u8* {
    unsafe {
        u64 eff_size = size
        if eff_size == 0 {
            eff_size = 1
        }
        i64 r = asm<i64>("syscall", "={rax},{rax},{rbx},{r10},{rdx},~{rcx},~{r11},~{memory}", 12, 0, cast<i64>(eff_size), 0)
        if cast<u64>(r) == cast<u64>(0 - 1) {
            return cast<u8*>(0)
        }
        return cast<u8*>(r)
    }
}

// Free. The current backends are bump/mmap allocators, so free only preserves
// the API shape (no per-block reclamation).
fun [name(__ins_free)] free(u8* ptr, u64 size, u64 align) -> void {
    return
}

// Reallocate: allocate a new block, copy the (new_size) bytes, free the old.
fun [name(__ins_realloc)] realloc(u8* ptr, u64 new_size, u64 align) -> u8* {
    unsafe {
        if cast<u64>(ptr) == 0 {
            return alloc(new_size, align)
        }
        u8* fresh = alloc(new_size, align)
        if cast<u64>(fresh) == 0 {
            return cast<u8*>(0)
        }
        memcpy(fresh, ptr, new_size)
        free(ptr, new_size, align)
        return fresh
    }
}
)Insty";

struct CoreModule {
    std::shared_ptr<AST::ProgramRoot> ast;
    Sema::SemaResult sema;
    bool ok = false;
};

}

namespace {

std::unordered_map<Types::TypeContext*, std::unique_ptr<CoreModule>> g_coreCache;

CoreModule* getCoreModule(Types::TypeContext& types) {
    auto it = g_coreCache.find(&types);
    if (it != g_coreCache.end()) {
        return it->second.get();
    }
    auto cm = std::make_unique<CoreModule>();

    std::string source = kCoreSource;
    ErrorReporting::ErrorReporter scratch(source, "<core>");
    Parser parser;
    std::string mutableSource = source;
    cm->ast = parser.produceAST(mutableSource);
    if (cm->ast && !scratch.hasError()) {
        Sema::Analyzer analyzer(types, &scratch);
        cm->sema = analyzer.analyze(cm->ast, {});
        cm->ok = cm->sema.ok && !scratch.hasError();
    }

    CoreModule* raw = cm.get();
    g_coreCache[&types] = std::move(cm);
    return raw;
}

}

llvm::Function* CodeGenerator::emitCoreFunction(const std::string& coreSymbol,
                                                const std::string& asSymbol) {
    const std::string& finalSymbol = asSymbol.empty() ? coreSymbol : asSymbol;

    if (llvm::Function* existing = module->getFunction(finalSymbol)) {
        if (!existing->empty()) {
            return existing;
        }
    }

    CoreModule* core = getCoreModule(types_);
    if (!core || !core->ok) {
        return nullptr;
    }

    const Sema::FunctionInfo* target = nullptr;
    for (const auto& fn : core->sema.functions) {
        const std::string& sym = fn.mangledName.empty() ? fn.name : fn.mangledName;
        if (sym == coreSymbol) {
            target = &fn;
            break;
        }
    }
    if (!target || !target->decl) {
        return nullptr;
    }

    llvm::IRBuilderBase::InsertPoint savedIP = builder->saveIP();
    const Sema::SemaResult* savedSema = sema_;
    sema_ = &core->sema;
    generateFunctionBody(*target->decl, *target);
    sema_ = savedSema;
    builder->restoreIP(savedIP);

    llvm::Function* fn = module->getFunction(coreSymbol);
    if (fn && finalSymbol != coreSymbol) {
        fn->setName(finalSymbol);
    }
    if (fn) {
        fn->setLinkage(llvm::Function::InternalLinkage);
    }

    if (fn) {
        for (llvm::BasicBlock& bb : *fn) {
            for (llvm::Instruction& inst : bb) {
                auto* call = llvm::dyn_cast<llvm::CallInst>(&inst);
                if (!call) continue;
                llvm::Function* callee = call->getCalledFunction();
                if (!callee || !callee->empty()) continue;
                const std::string& name = callee->getName().str();
                bool isCore = name.rfind("__ins_", 0) == 0 ||
                              name.rfind("core_", 0) == 0;
                if (isCore && name != finalSymbol) {
                    emitCoreFunction(name);
                }
            }
        }
    }
    return fn;
}

void CodeGenerator::generateCoreRuntimeFunctions() {
}

llvm::Function* CodeGenerator::generateMemcpyFunction() {
    return emitCoreFunction("__ins_memcpy");
}

llvm::Function* CodeGenerator::generateMemsetFunction() {
    return emitCoreFunction("__ins_memset");
}
