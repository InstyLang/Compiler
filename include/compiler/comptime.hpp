#pragma once

// Compile-time conditional resolution.
//
// `#if` / `#else if` / `#else` select between blocks of source based on the
// compilation target. This pass runs immediately after parsing and splices the
// taken branch into its parent statement list, deleting the rest.
//
// Doing it before semantic analysis is the whole point. A branch that is not
// taken is never type-checked and never reaches code generation, so it may
// contain constructs the target cannot express at all -- a raw syscall on
// wasm, say, or an `extern` from a DLL that only exists on Windows. Any scheme
// that decided later, or at run time, would still have to compile both sides.
//
// Conditions are restricted to what can be answered without a program: the
// `@targetIs("...")` predicate, boolean literals, and `!`, `&&`, `||`. Anything
// else is an error rather than being silently treated as false.
//
//     #if @targetIs("wasm") {
//         wasi.write_bytes(1, ptr, len)
//     } #else if @targetIs("windows") {
//         windows.write_console(ptr, len)
//     } #else {
//         unsafe { syscall.sys3(1, 1, cast<i64>(ptr), len) }
//     }
//
// A branch holds top-level declarations as well as statements, so it can select
// between imports, functions, structs, classes and enums. Conditional imports are
// the reason that matters: an import pulls in a dependency, and one that only
// exists -- or only compiles -- on a single target must not be resolved on the
// others. The driver therefore runs this pass over its dependency scan too, before
// reading the import list, and imports inside a taken branch are added to the
// module's imports as they are spliced in.
//
// `@targetIs` matches the target's architecture, operating system, or CLI name,
// plus a few family aliases (see the implementation). Matching is
// case-insensitive.

#include <string>

#include <extra/ast.hpp>
#include <utilities/errors.hpp>
#include <utilities/target.hpp>

namespace Comptime {

// Resolves every compile-time conditional in `program` against `target`,
// replacing each with the statements of its taken branch. Returns false and
// fills `errorOut` if a condition is not answerable at compile time.
bool resolve(AST::ProgramRoot& program, const Targeting::TargetSpec& target,
             ErrorReporting::ErrorReporter* reporter, std::string& errorOut);

// Whether `name` describes `target`. Exposed for testing.
bool targetMatches(const Targeting::TargetSpec& target, const std::string& name);

}  // namespace Comptime

