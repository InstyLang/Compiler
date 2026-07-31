#pragma once


#include <string>
#include <vector>

namespace Builtins {

enum class Builtin {
    Malloc,
    Free,
    Realloc,
    Memset,
    Memcpy,
    Panic,
    // Compile-time target predicate. Only meaningful inside a #if condition,
    // where the comptime pass folds it away; see compiler/comptime.hpp.
    TargetIs,
    Utf16,
    Hash,
    Unknown
};

struct BuiltinSpec {
    Builtin id = Builtin::Unknown;
    const char* name = "";
    int minArgs = 0;
    int maxArgs = -1;
    bool requiresUnsafe = false;
    bool implemented = false;
};

Builtin lookup(const std::string& name);
const BuiltinSpec& spec(Builtin id);
bool isBuiltinName(const std::string& name);

}

