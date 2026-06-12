#include <codegen/codegen.hpp>
#include <codegen/codegen_internal.hpp>

#include <llvm/IR/DerivedTypes.h>


namespace CodegenInternal {

int structFieldIndex(const CodeGenerator* gen, const std::string& structName,
                     const std::string& fieldName) {
    GeneratorState& state = stateFor(gen);
    auto it = state.structs.find(structName);
    if (it == state.structs.end()) {
        return -1;
    }
    return it->second.indexOf(fieldName);
}

llvm::StructType* structLLVMType(const CodeGenerator* gen, const std::string& structName) {
    GeneratorState& state = stateFor(gen);
    auto it = state.structs.find(structName);
    if (it == state.structs.end()) {
        return nullptr;
    }
    return it->second.llvmType;
}

Types::TypeRef structFieldType(const CodeGenerator* gen, const std::string& structName,
                               const std::string& fieldName) {
    GeneratorState& state = stateFor(gen);
    auto it = state.structs.find(structName);
    if (it == state.structs.end()) {
        return nullptr;
    }
    return it->second.fieldType(fieldName);
}

}
