#pragma once

// Shared helpers for laying out module-level global variables.
//
// Both backends need the same two answers about a global: how much storage it
// occupies, and whether its initializer folds to a constant that can be baked
// into the emitted image. The x86-64 path uses them to fill .data/.bss; the wasm
// path uses them to build data segments. They live here so the two agree by
// construction -- a global initialized to `1 << 12` must land on the same bytes
// whichever backend runs.

#include <cstdint>

#include <extra/ast.hpp>
#include <extra/type_system.hpp>

namespace Backend {

struct SizeAlign {
    std::uint64_t size = 8;
    unsigned align = 8;
};

// Byte size and alignment of a global's type. Mirrors the selector's scalar /
// pointer / array rules; structs fall back to the 8/8 default. Pointers and text
// are 8 bytes and slices 16, matching the selector's 64-bit address model.
SizeAlign scalarSizeAlign(Types::TypeRef type);

// Folds a scalar integer initializer at compile time. Handles the forms that
// appear in real initializers: integer and bool literals, unary `+ - ! ~`, the
// binary arithmetic/bitwise operators, and shifts. Returns true and writes the
// value when the expression is constant; false for anything else, which the
// caller should zero-initialize instead.
//
// This is what lets `i32 HANDLE = 0 - 11` or `u32 MASK = 1 << 12` hold the right
// bytes rather than becoming zero.
bool evalConstInt(const AST::ExprAST* expr, __int128& out);

}  // namespace Backend
