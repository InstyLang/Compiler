#pragma once


#include <string>
#include <unordered_map>
#include <vector>

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Type.h>

#include <extra/ast.hpp>
#include <extra/type_system.hpp>
#include <sema/sema.hpp>

namespace llvm {
class Module;
class Value;
}

class CodeGenerator;
class LLVMTypeFactory;

namespace CodegenInternal {

struct StructLayout {
    llvm::StructType* llvmType = nullptr;
    std::vector<std::pair<std::string, Types::TypeRef>> fields;

    int indexOf(const std::string& fieldName) const {
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (fields[i].first == fieldName) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    Types::TypeRef fieldType(const std::string& fieldName) const {
        for (const auto& f : fields) {
            if (f.first == fieldName) {
                return f.second;
            }
        }
        return nullptr;
    }
};

struct GeneratorState {
    std::unordered_map<std::string, StructLayout> structs;
    std::unordered_map<std::string, llvm::Type*> enumUnderlying;
    std::unordered_map<std::string, std::pair<long long, Types::TypeRef>> enumConstants;
    std::unordered_map<std::string, std::pair<llvm::Value*, Types::TypeRef>> globals;
    std::unordered_map<std::string, Types::TypeRef> activeSubst;
    std::string activeClassTemplate;
    std::string activeClassInstance;
};

GeneratorState& stateFor(const CodeGenerator* gen);
void releaseState(const CodeGenerator* gen);

bool isGenericFunction(const AST::FunctionDeclaration& decl);

bool callHasGenericArgs(const AST::FunctionCallExpr& call);

unsigned declareImportedExternals(llvm::Module& module, LLVMTypeFactory& factory,
                                  const std::vector<Sema::FunctionInfo>& imported);

int structFieldIndex(const CodeGenerator* gen, const std::string& structName,
                     const std::string& fieldName);
llvm::StructType* structLLVMType(const CodeGenerator* gen, const std::string& structName);
Types::TypeRef structFieldType(const CodeGenerator* gen, const std::string& structName,
                               const std::string& fieldName);

}
