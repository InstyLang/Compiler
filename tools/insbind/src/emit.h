// insbind: the emitter (C model -> Insty source).
//
// Applies the FFI mapping spec:
//   scalars      -> i8..u64/f32/f64/bool/void
//   const char*  -> text;  char* -> u8*;  void* stays;  T** chains kept
//   records      -> struct (declared order = C layout; [align(N)] honored)
//   unions       -> aligned u8 blob struct + per-member cast accessors
//   bitfields    -> storage-unit fields + shift/mask accessors
//   enums        -> enum : <computed underlying> with explicit values
//   functions    -> extern fun [dll("x")]/[lib("x")]; fn pointers -> u64
//   variadic and static-inline functions, extern globals, function-like
//   macros      -> skipped with a note (shim territory)
//   main-file object-like macros with constant values -> accessor functions
//     (Insty module globals do not cross `import`, so constants are functions)
//
// --abi-check additionally emits a self-verifying .ins program asserting
// .insize and field offsets against this tool's layout engine.
#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "layout.h"
#include "model.h"
#include "token.h"

namespace insbind {

struct EmitOptions {
    std::string moduleName;
    std::string linkName;   // dll("...") on Windows targets, lib("...") else
    bool linkIsDll = true;
    bool abiCheck = false;
    LayoutOptions layout;
    // Main-file object-like macros (from PreprocessResult::objectMacros).
    const std::vector<std::pair<std::string, std::vector<Token>>>* macros =
        nullptr;
};

struct EmitResult {
    std::string bindings;      // the .ins module text
    std::string abiCheck;      // optional self-check .ins program
    std::vector<std::string> warnings; // skipped constructs, one per line
};

EmitResult emit(const ParseResult& model, const EmitOptions& opts);

} // namespace insbind
