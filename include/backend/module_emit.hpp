#pragma once

// Module-level driver for the custom x86-64 backend.
//
// Bridges semantic-analysis output to an emitted relocatable object file. Given
// a SemaResult, it selects every analyzed function with a body, runs register
// allocation and lowering for each into a single MachineCode, then serializes
// that to a COFF or ELF object via the corresponding writer.
//
// This is the integration point used by the compiler driver.

#include <set>
#include <string>
#include <vector>

#include <backend/abi.hpp>
#include <extra/ast.hpp>
#include <sema/sema.hpp>
#include <utilities/target.hpp>

namespace Backend {

// The object container format to emit.
//
// Wasm is not an object container in the same sense as the others: a wasm
// module is self-contained, so no linker step follows and the backend writes
// the final artifact directly (as it already does for Pe). It is also the only
// non-x86-64 format, so it bypasses register allocation and lowering entirely.
enum class ObjectFormat { Coff, Elf, Pe, MachO, Wasm };

// Which startup shim, if any, to synthesize and use as the program entry point.
// The shim calls `main` and terminates the process (so a program need not call
// ExitProcess / exit itself, and main's return value becomes the exit status):
//   None : no shim; the provided entrySymbol (typically "main") is the entry.
//   Pe   : Win64 entry "_start": call main; ExitProcess(main()'s i32 return).
//   Elf  : Linux SysV entry "_start": call main; exit(main()&0xFF) via syscall.
//   Efi  : UEFI entry "efi_main"(handle, systemTable): call main; return status.
//   Wasi : WASI command entry, an exported "_start" (()->()): call main; then
//          proc_exit(main()&0xFF). Unlike the others this is not built as x86
//          Machine IR -- the wasm emitter synthesizes it directly.
enum class EntryShim { None, Pe, Elf, Efi, Wasi };

// Emits all body-bearing functions in `sema` to a relocatable object at
// `outPath`, using the given ABI and object format. On failure returns false
// and fills `errorOut` with a diagnostic (the first function that failed to
// select/lower, or an I/O error).
//
// For ObjectFormat::Pe a full runnable executable is written instead of a
// relocatable object; `entrySymbol` names the image entry point (e.g. "main").
//
// `program` (optional) is the parsed module root; when provided, module-level
// global variables are emitted into .data/.bss (integer-literal initializers are
// folded into .data, everything else is zero-initialized in .bss). Their AST
// initializers are read from `program->body` because SemaResult::GlobalInfo does
// not carry them.
//
// `runtimeModule` (optional) is an analyzed module providing internal runtime
// helper functions (e.g. the __ins_fmt_* string formatters used by string
// interpolation). When a selected function references such a helper, its body is
// looked up here and selected+lowered into the same object as an internal
// (Local) symbol. Pass the parsed+analyzed core runtime SemaResult.
//
// `shim` selects an optional startup shim to synthesize (see EntryShim). When not
// None and a `main` function exists, the shim is emitted and becomes the program
// entry point (overriding `entrySymbol` for the PE writer).
//
// `optLevel` (0..3) drives Machine-IR optimization passes run on each selected
// function before register allocation. 0 disables them; 1+ enables peephole,
// dead-code elimination, and branch simplification.
//
// `preferHostedEntry` requests hosted-executable startup: when set, an
// EntryShim::Elf `_start` is NOT synthesized, because the program will be linked
// against a C runtime (crt1.o) whose `_start` calls `main` via __libc_start_main.
// The driver sets this once any module in the build requires a shared library.
// It is also implied for this module if any of its own referenced externs carry
// a `lib(...)` directive.
//
// `requiredLibsOut` (optional): the union of `lib(...)` names of every extern
// referenced while lowering this module is appended here, for the driver to
// forward to the linker as `-l<name>`.
//
// `boundsCheck`: when true, slice/array indexing and sub-slicing emit runtime
// range checks that trap (ud2) on an out-of-range access.
//
// `target` (optional) is the resolved compilation target, borrowed from the
// driver and required to outlive the call. Historically the driver collapsed the
// whole TargetSpec into the handful of scalars above (abi/format/shim/
// instantOsSyscalls/aesHash); non-x86 backends need more than that, so the spec
// is now passed through as well. New target-dependent behaviour should read it
// from here rather than growing another parameter.
bool emitModuleObject(const Sema::SemaResult& sema, Abi abi, ObjectFormat format,
                      const std::string& outPath, std::string& errorOut,
                      const std::string& entrySymbol = "main",
                      const AST::ProgramRoot* program = nullptr,
                      const Sema::SemaResult* runtimeModule = nullptr,
                      EntryShim shim = EntryShim::None,
                      int optLevel = 0,
                      bool instantOsSyscalls = false,
                      bool preferHostedEntry = false,
                      std::vector<std::string>* requiredLibsOut = nullptr,
                      bool boundsCheck = false,
                      bool aesHash = false,
                      const Targeting::TargetSpec* target = nullptr);

}  // namespace Backend
