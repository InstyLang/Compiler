#include <codegen/codegen.hpp>
#include <codegen/codegen_internal.hpp>

#include <llvm/IR/DerivedTypes.h>


llvm::Type* CodeGenerator::lowerType(Types::TypeRef type) {
    if (!type) {
        return typeFactory->createVoid();
    }

    switch (type->kind) {
        case Types::Kind::Void:
        case Types::Kind::Error:
            return typeFactory->createVoid();

        case Types::Kind::Bool:
            return typeFactory->createInt(1);

        case Types::Kind::Int:
            return typeFactory->createInt(
                type->bitWidth ? static_cast<unsigned>(type->bitWidth) : 32);

        case Types::Kind::Float:
            return typeFactory->createFloat(
                type->bitWidth ? static_cast<unsigned>(type->bitWidth) : 64);

        case Types::Kind::Text:
            return typeFactory->createPointer();

        case Types::Kind::Pointer:
            return typeFactory->createPointer();

        case Types::Kind::Slice:
            return typeFactory->createPointer();

        case Types::Kind::Array: {
            llvm::Type* elem = lowerType(type->element);
            uint64_t count =
                type->arrayLength > 0 ? static_cast<uint64_t>(type->arrayLength) : 0;
            return llvm::ArrayType::get(elem, count);
        }

        case Types::Kind::Enum: {
            auto& state = CodegenInternal::stateFor(this);
            auto it = state.enumUnderlying.find(type->name);
            if (it != state.enumUnderlying.end()) {
                return it->second;
            }
            return typeFactory->createInt(
                type->bitWidth ? static_cast<unsigned>(type->bitWidth) : 32);
        }

        case Types::Kind::Struct: {
            auto& state = CodegenInternal::stateFor(this);
            auto it = state.structs.find(type->name);
            if (it != state.structs.end() && it->second.llvmType) {
                return it->second.llvmType;
            }
            if (!type->name.empty()) {
                std::string llvmName = "struct." + type->name;
                if (llvm::StructType* existing =
                        llvm::StructType::getTypeByName(*context, llvmName)) {
                    return existing;
                }
                return llvm::StructType::create(*context, llvmName);
            }
            return typeFactory->createPointer();
        }

        case Types::Kind::Class: {
            auto& state = CodegenInternal::stateFor(this);
            auto it = state.structs.find(type->name);
            if (it != state.structs.end() && it->second.llvmType) {
                return it->second.llvmType;
            }
            if (!type->name.empty()) {
                std::string llvmName = "struct." + type->name;
                if (llvm::StructType* existing =
                        llvm::StructType::getTypeByName(*context, llvmName)) {
                    return existing;
                }
                return llvm::StructType::create(*context, llvmName);
            }
            return typeFactory->createPointer();
        }

        case Types::Kind::Function:
            return typeFactory->createPointer();

        case Types::Kind::Generic:
            return typeFactory->createPointer();
    }

    return typeFactory->createPointer();
}
