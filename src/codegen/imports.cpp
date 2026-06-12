#include <codegen/codegen.hpp>
#include <codegen/codegen_internal.hpp>

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>


namespace CodegenInternal {

unsigned declareImportedExternals(llvm::Module& module, LLVMTypeFactory& factory,
                                  const std::vector<Sema::FunctionInfo>& imported) {
    unsigned created = 0;
    for (const Sema::FunctionInfo& info : imported) {
        const std::string& symbol =
            info.mangledName.empty() ? info.name : info.mangledName;
        if (symbol.empty()) {
            continue;
        }
        if (module.getFunction(symbol)) {
            continue;
        }

        std::vector<llvm::Type*> paramTys;
        paramTys.reserve(info.paramTypes.size());
        for (Types::TypeRef p : info.paramTypes) {
            paramTys.push_back(factory.fromEcxType(p));
        }
        llvm::Type* retTy = factory.fromEcxType(info.returnType);
        llvm::FunctionType* fnTy = llvm::FunctionType::get(retTy, paramTys, false);

        llvm::Function* fn = llvm::Function::Create(
            fnTy, llvm::Function::ExternalLinkage, symbol, &module);

        unsigned idx = 0;
        for (auto& arg : fn->args()) {
            if (idx < info.paramNames.size()) {
                arg.setName(info.paramNames[idx]);
            }
            ++idx;
        }
        ++created;
    }
    return created;
}

}
