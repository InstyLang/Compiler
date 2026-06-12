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
        return kind == Kind::Pointer || kind == Kind::Text || kind == Kind::Slice;
    }
    bool isError() const { return kind == Kind::Error; }
    bool isVoid() const { return kind == Kind::Void; }
};

class TypeContext {
public:
    TypeContext();

    TypeRef voidType() const { return &void_; }
    TypeRef boolType() const { return &bool_; }
    TypeRef textType() const { return &text_; }
    TypeRef errorType() const { return &error_; }

    TypeRef intType(int bitWidth, bool isSigned);
    TypeRef floatType(int bitWidth);
    TypeRef pointerType(TypeRef element, bool isVolatile = false);
    TypeRef arrayType(TypeRef element, int64_t length);
    TypeRef sliceType(TypeRef element);
    TypeRef namedType(Kind kind, const std::string& name);

    TypeRef fromString(const std::string& spelling);

    std::string toString(TypeRef type) const;

    static bool equals(TypeRef a, TypeRef b);

    void registerNamed(const std::string& name, Kind kind);

private:
    Type void_;
    Type bool_;
    Type text_;
    Type error_;

    std::vector<std::unique_ptr<Type>> pool_;
    std::vector<std::pair<std::string, Kind>> named_;

    TypeRef intern(Type prototype);
};

bool isPrimitiveSpelling(const std::string& name);

}
