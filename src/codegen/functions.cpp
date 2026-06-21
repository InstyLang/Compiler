#include <codegen/codegen.hpp>
#include <codegen/codegen_internal.hpp>

#include <llvm/IR/Attributes.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>

namespace {

const AST::Attribute* findAttribute(const AST::FunctionDeclaration& decl,
                                    const std::string& name) {
    for (const auto& attr : decl.attributes) {
        if (attr.name == name) {
            return &attr;
        }
    }
    return nullptr;
}

llvm::CallingConv::ID convFromName(const std::string& conv) {
    if (conv == "fastcc" || conv == "fast") {
        return llvm::CallingConv::Fast;
    }
    if (conv == "coldcc" || conv == "cold") {
        return llvm::CallingConv::Cold;
    }
    return llvm::CallingConv::C;
}

}

llvm::Function* CodeGenerator::getOrDeclareFunction(const Sema::FunctionInfo& info) {
    const std::string& symbol =
        info.mangledName.empty() ? info.name : info.mangledName;

    if (auto it = functions_.find(symbol); it != functions_.end()) {
        return it->second;
    }
    if (llvm::Function* existing = module->getFunction(symbol)) {
        functions_[symbol] = existing;
        return existing;
    }

    std::vector<llvm::Type*> paramTys;
    paramTys.reserve(info.paramTypes.size());
    for (Types::TypeRef p : info.paramTypes) {
        paramTys.push_back(lowerType(p));
    }

    llvm::Type* retTy = lowerType(info.returnType);
    llvm::FunctionType* fnTy = llvm::FunctionType::get(retTy, paramTys, false);

    // Private-by-default: a function defined in this module that is not
    // `export`ed (and is not `main`) gets internal linkage so it is hidden from
    // other translation units. Imported/external declarations and exported
    // definitions keep external linkage so the linker can unify them.
    llvm::Function::LinkageTypes linkage = llvm::Function::ExternalLinkage;
    if (!info.isExternal && !info.isExported && info.name != "main") {
        linkage = llvm::Function::InternalLinkage;
    }

    llvm::Function* fn = llvm::Function::Create(
        fnTy, linkage, symbol, module.get());

    unsigned idx = 0;
    for (auto& arg : fn->args()) {
        if (idx < info.paramNames.size()) {
            arg.setName(info.paramNames[idx]);
        }
        ++idx;
    }

    if (info.decl) {
        const AST::FunctionDeclaration& decl = *info.decl;

        if (findAttribute(decl, "naked")) {
            fn->addFnAttr(llvm::Attribute::Naked);
            fn->addFnAttr(llvm::Attribute::NoInline);
        }
        if (runtimeOptions_.freestanding || targetSpec_.isFreestandingExecutable()) {
            fn->addFnAttr(llvm::Attribute::NoUnwind);
        }
        if (const AST::Attribute* conv = findAttribute(decl, "conv")) {
            fn->setCallingConv(convFromName(conv->value));
        }
        if (const AST::Attribute* section = findAttribute(decl, "section")) {
            if (!section->value.empty()) {
                fn->setSection(section->value);
            }
        }
        if (const AST::Attribute* nameAttr = findAttribute(decl, "name")) {
            if (!nameAttr->value.empty()) {
                fn->setName(nameAttr->value);
            }
        }
    }

    functions_[symbol] = fn;
    if (!info.name.empty() && functions_.find(info.name) == functions_.end()) {
        functions_[info.name] = fn;
    }
    return fn;
}

void CodeGenerator::declareFunctions(const std::shared_ptr<AST::ProgramRoot>& program) {
    (void)program;
    if (!sema_) {
        return;
    }
    for (const auto& info : sema_->functions) {
        getOrDeclareFunction(info);
    }
}

void CodeGenerator::generateFunctionBody(const AST::FunctionDeclaration& decl,
                                         const Sema::FunctionInfo& info) {
    if (!decl.hasBody || info.isExternal) {
        return;
    }

    llvm::Function* fn = getOrDeclareFunction(info);
    if (!fn || !fn->empty()) {
        return;
    }

    llvm::Function* savedFn = currentFunction_;
    Types::TypeRef savedRet = currentReturnType_;
    bool savedUnsafe = unsafeContext_;

    currentFunction_ = fn;
    currentReturnType_ = info.returnType;
    unsafeContext_ = info.isUnsafe;

    llvm::BasicBlock* entry = llvm::BasicBlock::Create(*context, "entry", fn);
    builder->SetInsertPoint(entry);

    pushScope();

    bool isNaked = false;
    for (const auto& attr : decl.attributes) {
        if (attr.name == "naked") {
            isNaked = true;
            break;
        }
    }

    if (!isNaked) {
        unsigned idx = 0;
        for (auto& arg : fn->args()) {
            Types::TypeRef paramType =
                idx < info.paramTypes.size() ? info.paramTypes[idx] : nullptr;
            std::string paramName =
                idx < info.paramNames.size() ? info.paramNames[idx] : ("arg" + std::to_string(idx));

            llvm::AllocaInst* slot = builder->CreateAlloca(arg.getType(), nullptr, paramName);
            builder->CreateStore(&arg, slot);
            declareLocal(paramName, slot, paramType);
            ++idx;
        }
    }

    generateBlock(decl.body);

    llvm::BasicBlock* current = builder->GetInsertBlock();
    if (current && !current->getTerminator()) {
        llvm::Type* retTy = fn->getReturnType();
        if (retTy->isVoidTy()) {
            builder->CreateRetVoid();
        } else if (retTy->isIntegerTy()) {
            builder->CreateRet(llvm::ConstantInt::get(retTy, 0));
        } else if (retTy->isFloatingPointTy()) {
            builder->CreateRet(llvm::ConstantFP::get(retTy, 0.0));
        } else if (retTy->isPointerTy()) {
            builder->CreateRet(typeFactory->createNullPointer());
        } else {
            builder->CreateRet(llvm::Constant::getNullValue(retTy));
        }
    }

    popScope();

    currentFunction_ = savedFn;
    currentReturnType_ = savedRet;
    unsafeContext_ = savedUnsafe;
}
