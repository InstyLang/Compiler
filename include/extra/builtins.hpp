#pragma once


#include <string>
#include <vector>

namespace Builtins {

enum class Builtin {
    Syscall,
    Strlen,
    Sizeof,
    Alignof,
    Malloc,
    Free,
    Realloc,
    Memset,
    Memcpy,
    Panic,
    Print,
    Println,
    ReadFile,
    System,
    GetCurrentOS,
    Typeof,
    Offsetof,
    Bitcast,
    IntToPtr,
    PtrToInt,
    Utf16,
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
