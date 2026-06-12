#include <extra/builtins.hpp>

namespace Builtins {

namespace {

constexpr BuiltinSpec kSpecs[] = {
    {Builtin::Syscall,      "syscall",      1, -1, true,  true},
    {Builtin::Strlen,       "strlen",       1,  1, false, true},
    {Builtin::Sizeof,       "sizeof",       1,  1, false, true},
    {Builtin::Alignof,      "alignof",      1,  1, false, false},
    {Builtin::Malloc,       "malloc",       1,  2, true,  true},
    {Builtin::Free,         "free",         1,  3, true,  true},
    {Builtin::Realloc,      "realloc",      2,  3, true,  true},
    {Builtin::Memset,       "memset",       3,  3, true,  true},
    {Builtin::Memcpy,       "memcpy",       3,  3, true,  true},
    {Builtin::Panic,        "panic",        1,  1, false, true},
    {Builtin::Print,        "print",        1, -1, false, false},
    {Builtin::Println,      "println",      1, -1, false, false},
    {Builtin::ReadFile,     "readFile",     1,  1, false, false},
    {Builtin::System,       "system",       1,  1, false, false},
    {Builtin::GetCurrentOS, "getCurrentOS", 0,  0, false, false},
    {Builtin::Typeof,       "typeof",       1,  1, false, false},
    {Builtin::Offsetof,     "offsetof",     2,  2, false, false},
    {Builtin::Bitcast,      "bitcast",      1,  1, true,  false},
    {Builtin::IntToPtr,     "inttoptr",     1,  1, true,  false},
    {Builtin::PtrToInt,     "ptrtoint",     1,  1, true,  false},
    {Builtin::Utf16,        "utf16",        1,  1, false, true},
    {Builtin::Unknown,      "",             0, -1, false, false},
};

}

Builtin lookup(const std::string& name) {
    for (const auto& spec : kSpecs) {
        if (spec.id != Builtin::Unknown && name == spec.name) {
            return spec.id;
        }
    }
    return Builtin::Unknown;
}

const BuiltinSpec& spec(Builtin id) {
    for (const auto& s : kSpecs) {
        if (s.id == id) {
            return s;
        }
    }
    return kSpecs[sizeof(kSpecs) / sizeof(kSpecs[0]) - 1];
}

bool isBuiltinName(const std::string& name) {
    return lookup(name) != Builtin::Unknown;
}

}
