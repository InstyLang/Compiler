#pragma once


#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Types {

enum class Kind {
    Void,
    Bool,
    Int,
    Float,
    Text,
    Pointer,
    Array,
    Slice,
    Struct,
    Enum,
    Class,
    Function,
    Generic,
    Error
};

struct Type;
using TypeRef = const Type*;

struct Type {
    Kind kind = Kind::Error;

    int bitWidth = 0;
    bool isSigned = true;

    TypeRef element = nullptr;
    bool isVolatile = false;

    int64_t arrayLength = 0;

    std::string name;

    std::vector<TypeRef> params;
    TypeRef returnType = nullptr;

    bool isInteger() const { return kind == Kind::Int; }
    bool isFloat() const { return kind == Kind::Float; }
    bool isNumeric() const { return kind == Kind::Int || kind == Kind::Float; }
    bool isPointerLike() const {
        return kind == Kind::Pointer || kind == Kind::Text;
    }
    bool isError() const { return kind == Kind::Error; }
    bool isVoid() const { return kind == Kind::Void; }
};

class TypeContext {
public:
    TypeContext();

    // A process-unique, never-reused identity for this context instance.
    // Used by caches that hold TypeRefs (raw const Type*) owned by this
    // context so they can detect a destroyed-and-recreated context that
    // happens to reuse the same address. Keying such caches on the raw
    // `TypeContext*` is unsafe because the address can be reused.
    uint64_t id() const { return id_; }

    TypeRef voidType() const { return &void_; }
    TypeRef boolType() const { return &bool_; }
    TypeRef textType() const { return &text_; }
    TypeRef errorType() const { return &error_; }

    TypeRef intType(int bitWidth, bool isSigned);
    TypeRef floatType(int bitWidth);
    TypeRef pointerType(TypeRef element, bool isVolatile = false);
    TypeRef arrayType(TypeRef element, int64_t length);
    TypeRef sliceType(TypeRef element);
    TypeRef functionType(const std::vector<TypeRef>& params, TypeRef returnType);
    TypeRef namedType(Kind kind, const std::string& name);

    TypeRef fromString(const std::string& spelling);

    std::string toString(TypeRef type) const;

    static bool equals(TypeRef a, TypeRef b);

    void registerNamed(const std::string& name, Kind kind);

    // Records the layout of a C-style enum: it occupies its declared underlying
    // type (`enum C : i32` is four bytes, not eight). The width has to live on the
    // interned Type itself, because the backend's sizeOf/alignOf/widthOf and
    // scalarSizeAlign for globals all read `bitWidth` -- if any of them answered
    // differently, the same enum would get one size as a field, another as an
    // array element, and another as a global.
    void registerEnumUnderlying(const std::string& name, int bitWidth, bool isSigned);

private:
    Type void_;
    Type bool_;
    Type text_;
    Type error_;

    uint64_t id_;

    std::vector<std::unique_ptr<Type>> pool_;
    std::vector<std::pair<std::string, Kind>> named_;

    TypeRef intern(Type prototype);
};

bool isPrimitiveSpelling(const std::string& name);

}
