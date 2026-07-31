#include <extra/builtins.hpp>

namespace Builtins {

namespace {

constexpr BuiltinSpec kSpecs[] = {
    {Builtin::Malloc,       "malloc",       1,  2, true,  true},
    {Builtin::Free,         "free",         1,  3, true,  true},
    {Builtin::Realloc,      "realloc",      2,  4, true,  true},
    {Builtin::Memset,       "memset",       3,  3, true,  true},
    {Builtin::Memcpy,       "memcpy",       3,  3, true,  true},
    {Builtin::Panic,        "panic",        1,  1, false, true},
    // Resolved and removed by the comptime pass before sema, so it is never
    // "implemented" in the ordinary sense: reaching sema means it was used
    // outside a compile-time condition.
    {Builtin::TargetIs,     "targetIs",     1,  1, false, false},
    {Builtin::Utf16,        "utf16",        1,  1, false, true},
    {Builtin::Hash,         "hash",         1,  1, false, true},
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

