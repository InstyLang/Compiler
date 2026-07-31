#pragma once

// WebAssembly code generation: Machine IR -> a complete .wasm module.
//
// This is the wasm counterpart of module_emit.cpp's x86-64 path. It reuses the
// front half of the backend verbatim -- instruction selection and the Machine-IR
// optimizer -- and replaces the back half. Where the x86 path runs
// LinearScanAllocator + Lowering to produce machine bytes, this consumes the
// same post-optimization MFunction, still in virtual-register form, and emits
// wasm instructions.
//
// How the register machine is mapped onto wasm
// --------------------------------------------
// wasm has no registers, no flags and no addressable native stack, so the two
// models look incompatible. They reconcile cleanly if *every* storage location
// the Machine IR names becomes a wasm local:
//
//   virtual register  -> a local: i64 for the GPR class, and i64 for XMM too
//                        (see Floating point below)
//   physical register -> a local, seeded at entry for the ABI argument
//                        registers and read back at `Ret` for the return
//                        register. Modelling these lets the selector's existing
//                        ABI conventions work unchanged.
//   frame slot        -> a local when the slot's address is never taken,
//                        otherwise a byte offset into a real stack frame (see
//                        Memory below). A store through a pointer has to be
//                        visible to every other access, which a local cannot
//                        provide.
//
// wasm engines allocate their own registers, so the redundancy this introduces
// costs nothing in the compiled result.
//
// Memory
// ------
// Linear memory is laid out as:
//
//   0        .. 1024      reserved, so a null dereference hits an address no
//                         object owns (wasm does not fault on 0, so this is a
//                         debugging aid, not a guarantee)
//   1024     .. dataEnd   static data: string literals then module globals.
//                         Initialized bytes form one data segment; zero-
//                         initialized globals are address space only, since
//                         wasm zeroes memory for us -- .bss without a .bss.
//   dataEnd  .. stackTop  shadow stack, growing down from stackTop
//   stackTop ..           heap, bump-allocated upward
//
// The heap needs its own allocator: the native lowerings of @malloc/@free reach
// for mmap or HeapAlloc, and wasm has neither. The backend synthesizes
// __wasm_alloc/__wasm_free directly as wasm (they need memory.grow and a mutable
// global, which the Machine IR cannot express) and the selector calls them under
// its `wasmTarget` flag. Freeing the most recent allocation rolls the cursor
// back, so an allocate/free loop runs in constant memory; anything else leaks
// until the module is discarded.
//
// A mutable i32 global holds the stack pointer. A function needing addressable
// storage subtracts its frame size on entry and restores the pointer at every
// return. Frame slot addresses are `framePtr + offset`, and because wasm memory
// accesses carry a static offset immediate, the offset usually costs nothing.
//
// Addresses stay 64-bit in locals and are narrowed with i32.wrap_i64 at each
// access, per the pointer-width note above.
//
// Pointer width
// -------------
// The selector's address model is 64-bit throughout (`widthOf` reports 8 bytes
// for pointers, text and slices). Rather than change that, the wasm path keeps
// 64-bit values in i64 locals and narrows to i32 only where wasm demands it --
// at the ABI boundary now, and at memory accesses in a later phase. This is
// correct because a wasm32 address never exceeds 2^32.
//
// Control flow
// ------------
// The Machine IR is an unstructured CFG: basic blocks ending in `Jmp`/`Jcc` to
// arbitrary labels. wasm has no such branch -- only `block`/`loop`/`if` scopes
// with `br` to a *relative depth*, which can exit a scope but never jump into
// one.
//
// The two are bridged with a dispatch loop: one `loop` containing one `block`
// scope per basic block, entered through a `br_table` on a state variable
// holding the index of the block to run next. An inter-block branch becomes
// "set the state, branch back to the dispatcher".
//
// This is chosen for correctness over quality. It handles every CFG, including
// the irreducible ones a structured reconstruction (a relooper) has to
// special-case, and it does not care that the selector builds CFGs at 65
// different sites. The cost is real: loops are not expressed as wasm `loop`s, so
// an engine cannot recognise them as loops, and every edge round-trips through
// the dispatcher. Recovering structure for the reducible cases -- which is all
// of them in practice, since Insty has no `goto` -- is a later optimization, and
// this implementation is the reference to differential-test it against.
//
// x86 compares set EFLAGS, read by a following Jcc/SetCC. wasm comparisons
// produce values, so `Cmp` emits nothing and is folded into its consumer.
//
// Calls
// -----
// wasm passes arguments on the operand stack, but by the time a `Call` is reached
// the selector has already distributed them into ABI argument registers. Since
// those registers are locals here, the emitter replays the selector's register
// assignment to read each argument back in parameter order, narrowing to the
// callee's declared type. The result comes back off the stack into the local for
// the return register, or is dropped if the caller ignored it.
//
// Function indices for every definition are reserved before any body is
// translated, which is what lets a call reference a callee that has not been
// emitted yet -- including mutual recursion. Imports must be declared first,
// because they occupy the low indices of the function index space.
//
// Calling through a function pointer needs more than an index, because wasm has
// no code addresses: a callee is reached by index or through a function table,
// and call_indirect checks the callee's type at run time. Every address-taken
// function therefore gets a table slot holding a thunk with a uniform all-i64
// signature, which converts to the real one and calls it. A single shared
// signature per arity is what makes one call site able to reach any of them.
// `&fn` yields the table slot, so it needs no knowledge of the target's types.
//
// The Machine IR does not record an indirect call's signature -- x86 does not
// need one -- so the selector attaches the minimum required to rebuild it: a
// result flag and, per argument, which register file carried it.
//
// Arguments past the register files are spilled by the selector to the caller's
// outgoing area, and wasm has to follow the same layout even though it passes
// everything as parameters. The outgoing area sits at the bottom of the
// shadow-stack frame at exactly the offsets StoreOutgoing uses, and on the callee
// side each incoming frame slot becomes a local seeded from the matching
// parameter. Its recorded frame offset is what pairs it with the right one.
//
// The set of functions emitted mirrors the x86 path: top-level functions,
// monomorphized generic instantiations, class methods/constructors/operators
// (whose bodies hang off the AST, not the FunctionInfo), generic class
// instantiations, and the runtime helpers selection asks for.
//
// Aggregates
// ----------
// Structs, classes and slices passed or returned by value reuse the selector's
// System V eightbyte classification verbatim, rather than the by-pointer
// convention a wasm C toolchain would use. That is sound precisely because the
// emitted module is self-contained -- there is no linker and no other object to
// agree with, so the convention only has to be internally consistent. Reusing
// the selector's own classification makes agreement structural rather than
// something to keep in sync:
//
//   aggregate in registers   one wasm parameter per eightbyte, so a 16-byte
//                            struct or a slice becomes two i64 parameters
//   aggregate in memory      one i64 parameter holding a hidden pointer
//   returned in memory       a hidden pointer prepended to the parameters, which
//                            the callee hands back
//   returned in registers    one result per eightbyte; two eightbytes uses
//                            wasm's multi-value returns (RAX then RDX)
//
// Both the caller and the callee derive their view from the same descriptor, so a
// signature cannot drift between the two sides.
//
// Floating point
// --------------
// XMM-class locals are typed i64 and hold *raw bits*, not a value. This mirrors
// the hardware: an XMM register is a bag of bits and the instruction's width
// decides whether they are read as an f32 (in the low half, as movss leaves them)
// or an f64. Each floating-point instruction therefore reinterprets on the way in
// and back on the way out; the engine folds those away, since they are bitcasts.
//
// The alternative -- typing XMM locals f64 -- would silently promote f32
// arithmetic to double precision and round only at the end. That is observable:
// summing 0.1f a hundred times and scaling by 1e6 gives 10000002 with per-step
// f32 rounding and 10000000 without. Both backends produce 10000002.
//
// Two consequences of reusing the selector's ABI show up here: floating-point
// arguments travel in the SSE argument registers, whose cursor advances
// independently of the integer one on System V, and an aggregate eightbyte marked
// SSE travels in an XMM register sized f32 when it holds at most four bytes.
//
// float-to-int conversion uses the *saturating* truncations. wasm's plain
// i64.trunc_f64_s traps on NaN or out-of-range, whereas x86 cvttsd2si yields the
// "integer indefinite" value; saturating never traps, which is closer to the
// native behaviour.
//
// f16 has no wasm counterpart, so the core runtime's software
// __ins_f16_to_f32 / __ins_f32_to_f16 stand in for F16C. Note that x86 folds the
// conversion into the memory access itself: a width of 2 on any F* load or store
// means "packed half in memory, f32 in the register", so each of those needs a
// helper call here. Packing rounds to nearest, ties to even, matching vcvtps2ph
// with its immediate at 0. An f16 constant is expanded at compile time instead.
//
// 128-bit integers ride the same register-pair path as a two-eightbyte aggregate
// (a pair of parameters, RAX:RDX for returns). Their widening multiply is
// synthesized, since wasm has no equivalent of x86's `mul`; division and modulo
// already go through the runtime's by-pointer helpers.
//
// The standard library
// --------------------
// An `extern fun [dll("name")]` becomes a wasm import from module "name", which
// is how libs/wasi/ binds the WASI preview1 interface -- the same mechanism
// windows::* uses for DLL imports.
//
// std::io and std::fs are portable: each selects its platform with a `#if`, so
// the same source builds against raw Linux syscalls, WASI, or Win32. wasi::io and
// wasi::fs remain as the direct, wasm-only bindings.
//
// Note that WASI is capability based: a module cannot name an arbitrary path.
// The host grants a directory (`wasmtime run --dir .`) and paths resolve relative
// to that preopen, so a file-I/O program that runs natively will fail on wasm
// unless the host was asked to grant it something. That is a difference in
// privilege, not in generated code.
//
// Exports and dead code
// ---------------------
// With no linker, nothing else would ever drop a function that is never called,
// so the backend eliminates unreachable ones itself. That needs an export policy,
// because an exported function is a root:
//
//   memory      always, since WASI hosts expect it
//   _start      for a WASI command
//   the program's own functions -- those defined in the module being compiled
//
// Insty's `export` keyword is deliberately *not* the criterion. It means "visible
// to importing modules", which is a different question: every function in
// libs/wasi is `export`, so using it here would pin the whole standard library
// into every module. The useful line is between the program and the libraries it
// pulled in, and name mangling already draws it -- a function defined in module M
// carries the symbol mangleFunction(M, name).
//
// Reachability then runs from those roots over the call graph, with one wrinkle:
// f16 conversion is an opcode rather than a call, so its dependency on the
// software helpers is invisible to a call-graph walk and is added explicitly.
// Data is laid out after elimination, so a dropped function's string constants go
// with it.
//
// In practice a program that imports all of wasi::io and wasi::fs and calls one
// function comes out at about 800 bytes with five functions, against roughly
// thirty without.
//
// Exit status
// -----------
// A WASI host only accepts a proc_exit status in [0, 126), so a program returning
// more than 125 from `main` cannot report it. Nothing is silently rewritten -- the
// host rejects the call and reports the error. Native Linux is lossy here too
// (its _start masks the result to & 0xFF).
//
// Scope
// -----
// Everything the language can express, except the constructs listed below. That
// covers integer and floating-point code (f16/f32/f64), 128-bit integers, control
// flow, linear memory, the heap, aggregates, direct and indirect calls, and
// arguments past the register files.
//
// Rejected, each with an explanation of the constraint rather than an internal
// opcode name:
//
//   @syscall, @print, @println   a wasm module reaches its host through imports,
//                                not a trap instruction; use wasi::sys, or
//                                std::io / std::fs which dispatch with `#if`
//   inline assembly              no textual instruction stream exists
//   atomics and fences           need the threads proposal
//   [naked(on)]                  a body is structured instructions, not bytes
//   [section("...")]             code is addressed by index; no named sections
//   [interrupt(on)], conv(...)   no interrupts, exactly one calling convention
//   --multiboot2, --raw-binary,  all describe a native image
//   --linker-script, -c
//
// Nothing is accepted and quietly dropped. Several of these used to be, which is
// the worse outcome: the user gets a module that does not do what they asked
// without being told.
//
// `volatile` is the one deliberate exception. It lowers to a plain access, because
// wasm has no memory-mapped hardware for it to matter to and no way to express the
// qualifier; the value read or written is still correct, so this is a lost
// optimization barrier rather than a wrong result.

#include <string>

#include <extra/ast.hpp>
#include <sema/sema.hpp>
#include <utilities/target.hpp>

namespace Backend::Wasm {

struct EmitOptions {
    // The resolved target. Borrowed; must outlive the call.
    const Targeting::TargetSpec* target = nullptr;
    // Parsed module root, for module-level globals (not yet emitted).
    const AST::ProgramRoot* program = nullptr;
    // Analyzed runtime-helper module (not yet used; helpers need call support).
    const Sema::SemaResult* runtimeModule = nullptr;
    int optLevel = 0;
    bool boundsCheck = false;
};

// Selects, lowers and serializes every body-bearing function in `sema` to a
// complete WebAssembly module at `outPath`. Returns false and fills `errorOut`
// on the first function that cannot be translated.
bool emitWasmModule(const Sema::SemaResult& sema, const EmitOptions& options,
                    const std::string& outPath, std::string& errorOut);

}  // namespace Backend::Wasm
