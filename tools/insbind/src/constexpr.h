// insbind: parser-level constant-expression evaluation (C11 6.6).
//
// Used where the declaration grammar embeds compile-time integers: array
// bounds, enumerator values, bitfield widths, _Alignas arguments. Full C
// operator precedence, signed 64-bit arithmetic (array bounds and enum values
// that exceed it are diagnosed, not wrapped).
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "token.h"

namespace insbind {

// Evaluates a constant expression starting at tokens[*pos], advancing *pos
// past the consumed tokens. Identifiers resolve through `names` when given
// (enumerator constants); anything else is "not a constant". Returns false
// and sets `err` on malformed input or division by zero.
bool evalConstExpr(
    const std::vector<Token>& tokens, std::size_t* pos, std::int64_t* out,
    std::string* err,
    const std::unordered_map<std::string, std::int64_t>* names = nullptr);

} // namespace insbind
