#include <extra/type_system.hpp>

#include <algorithm>
#include <cctype>

namespace Types {

namespace {

bool parseInteger(const std::string& s, int64_t& out) {
    if (s.empty()) {
        return false;
    }
    try {
        size_t consumed = 0;
        out = std::stoll(s, &consumed, 10);
        return consumed == s.size();
    } catch (...) {
        return false;
    }
}

}

bool isPrimitiveSpelling(const std::string& name) {
    static const char* prims[] = {
        "void", "bool", "text",
        "i8", "i16", "i32", "i64", "i128",
        "u8", "u16", "u32", "u64", "u128",
        "f16", "f32", "f64", "f128"
    };
    for (const char* p : prims) {
        if (name == p) {
            return true;
        }
    }
    return false;
}

TypeContext::TypeContext() {
    void_.kind = Kind::Void;
    bool_.kind = Kind::Bool;
    bool_.bitWidth = 1;
    text_.kind = Kind::Text;
    error_.kind = Kind::Error;
}

TypeRef TypeContext::intern(Type prototype) {
    for (const auto& existing : pool_) {
        if (equals(existing.get(), &prototype)) {
            return existing.get();
        }
    }
    pool_.push_back(std::make_unique<Type>(std::move(prototype)));
    return pool_.back().get();
}

TypeRef TypeContext::intType(int bitWidth, bool isSigned) {
    Type t;
    t.kind = Kind::Int;
    t.bitWidth = bitWidth;
    t.isSigned = isSigned;
    return intern(t);
}

TypeRef TypeContext::floatType(int bitWidth) {
    Type t;
    t.kind = Kind::Float;
    t.bitWidth = bitWidth;
    return intern(t);
}

TypeRef TypeContext::pointerType(TypeRef element, bool isVolatile) {
    Type t;
    t.kind = Kind::Pointer;
    t.element = element;
    t.isVolatile = isVolatile;
    return intern(t);
}

TypeRef TypeContext::arrayType(TypeRef element, int64_t length) {
    Type t;
    t.kind = Kind::Array;
    t.element = element;
    t.arrayLength = length;
    return intern(t);
}

TypeRef TypeContext::sliceType(TypeRef element) {
    Type t;
    t.kind = Kind::Slice;
    t.element = element;
    return intern(t);
}

TypeRef TypeContext::namedType(Kind kind, const std::string& name) {
    Type t;
    t.kind = kind;
    t.name = name;
    return intern(t);
}

void TypeContext::registerNamed(const std::string& name, Kind kind) {
    for (auto& entry : named_) {
        if (entry.first == name) {
            entry.second = kind;
            return;
        }
    }
    named_.emplace_back(name, kind);
}

TypeRef TypeContext::fromString(const std::string& spelling) {
    std::string s;
    s.reserve(spelling.size());
    for (char ch : spelling) {
        if (!std::isspace(static_cast<unsigned char>(ch)) || s.empty() ||
            (!s.empty() && s.back() != ' ')) {
            s.push_back(std::isspace(static_cast<unsigned char>(ch)) ? ' ' : ch);
        }
    }
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    while (!s.empty() && s.back() == ' ') s.pop_back();

    bool isVolatile = false;
    const std::string volatilePrefix = "volatile ";
    if (s.compare(0, volatilePrefix.size(), volatilePrefix) == 0) {
        isVolatile = true;
        s = s.substr(volatilePrefix.size());
        while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    }

    if (!s.empty() && s.back() == '*') {
        s.pop_back();
        while (!s.empty() && s.back() == ' ') s.pop_back();
        TypeRef inner = fromString(s);
        return pointerType(inner, isVolatile);
    }

    if (!s.empty() && s.back() == ']') {
        size_t open = s.rfind('[');
        if (open != std::string::npos) {
            std::string base = s.substr(0, open);
            std::string sizeText = s.substr(open + 1, s.size() - open - 2);
            while (!base.empty() && base.back() == ' ') base.pop_back();
            while (!sizeText.empty() && sizeText.front() == ' ') sizeText.erase(sizeText.begin());
            while (!sizeText.empty() && sizeText.back() == ' ') sizeText.pop_back();
            TypeRef inner = fromString(base);
            if (sizeText.empty()) {
                return sliceType(inner);
            }
            int64_t length = 0;
            if (parseInteger(sizeText, length)) {
                return arrayType(inner, length);
            }
            return errorType();
        }
    }

    size_t angle = s.find('<');
    if (angle != std::string::npos) {
        std::string base = s.substr(0, angle);
        for (const auto& entry : named_) {
            if (entry.first == base) {
                return namedType(entry.second, s);
            }
        }
        return namedType(Kind::Struct, s);
    }

    if (s == "void") return voidType();
    if (s == "bool") return boolType();
    if (s == "text") return textType();
    if (s == "i8") return intType(8, true);
    if (s == "i16") return intType(16, true);
    if (s == "i32") return intType(32, true);
    if (s == "i64") return intType(64, true);
    if (s == "i128") return intType(128, true);
    if (s == "u8") return intType(8, false);
    if (s == "u16") return intType(16, false);
    if (s == "u32") return intType(32, false);
    if (s == "u64") return intType(64, false);
    if (s == "u128") return intType(128, false);
    if (s == "f16") return floatType(16);
    if (s == "f32") return floatType(32);
    if (s == "f64") return floatType(64);
    if (s == "f128") return floatType(128);

    for (const auto& entry : named_) {
        if (entry.first == s) {
            return namedType(entry.second, s);
        }
    }

    if (s.size() <= 2 && !s.empty() && std::isupper(static_cast<unsigned char>(s[0]))) {
        return namedType(Kind::Generic, s);
    }

    return errorType();
}

std::string TypeContext::toString(TypeRef type) const {
    if (!type) {
        return "<null>";
    }
    switch (type->kind) {
        case Kind::Void: return "void";
        case Kind::Bool: return "bool";
        case Kind::Text: return "text";
        case Kind::Int: return (type->isSigned ? "i" : "u") + std::to_string(type->bitWidth);
        case Kind::Float: return "f" + std::to_string(type->bitWidth);
        case Kind::Pointer: return toString(type->element) + "*";
        case Kind::Array: return toString(type->element) + "[" + std::to_string(type->arrayLength) + "]";
        case Kind::Slice: return toString(type->element) + "[]";
        case Kind::Struct:
        case Kind::Enum:
        case Kind::Class:
        case Kind::Generic: return type->name;
        case Kind::Function: return "fn";
        case Kind::Error: return "<error>";
    }
    return "<?>";
}

bool TypeContext::equals(TypeRef a, TypeRef b) {
    if (a == b) {
        return true;
    }
    if (!a || !b) {
        return false;
    }
    if (a->kind != b->kind) {
        return false;
    }
    switch (a->kind) {
        case Kind::Int:
            return a->bitWidth == b->bitWidth && a->isSigned == b->isSigned;
        case Kind::Float:
            return a->bitWidth == b->bitWidth;
        case Kind::Pointer:
            return a->isVolatile == b->isVolatile && equals(a->element, b->element);
        case Kind::Slice:
            return equals(a->element, b->element);
        case Kind::Array:
            return a->arrayLength == b->arrayLength && equals(a->element, b->element);
        case Kind::Struct:
        case Kind::Enum:
        case Kind::Class:
        case Kind::Generic:
            return a->name == b->name;
        default:
            return true;
    }
}

}
