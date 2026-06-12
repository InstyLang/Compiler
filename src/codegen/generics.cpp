#include <codegen/codegen.hpp>
#include <codegen/codegen_internal.hpp>

#include <cctype>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>


namespace CodegenInternal {

bool isGenericCall(const AST::FunctionCallExpr& call) {
    return !call.genericArgs.empty();
}

bool isGenericDeclaration(const AST::FunctionDeclaration& decl) {
    return !decl.genericParams.empty();
}

bool isGenericStruct(const AST::StructDeclaration& decl) {
    return !decl.genericParams.empty();
}

bool isGenericClass(const AST::ClassDeclaration& decl) {
    return !decl.genericParams.empty();
}

bool signatureMentionsGenerics(const AST::FunctionDeclaration& decl) {
    if (decl.genericParams.empty()) {
        return false;
    }
    for (const auto& param : decl.parameters) {
        for (const auto& g : decl.genericParams) {
            if (param.type == g || param.type.find(g + "[") != std::string::npos ||
                param.type.find("<" + g) != std::string::npos) {
                return true;
            }
        }
    }
    for (const auto& g : decl.genericParams) {
        if (decl.returnType == g) {
            return true;
        }
    }
    return false;
}

}


Types::TypeRef CodeGenerator::resolveCodegenType(Types::TypeRef type) {
    if (!type) {
        return type;
    }
    auto& state = CodegenInternal::stateFor(this);
    if (state.activeSubst.empty()) {
        return type;
    }
    if (type->kind == Types::Kind::Generic) {
        auto it = state.activeSubst.find(type->name);
        if (it != state.activeSubst.end() && it->second) {
            return it->second;
        }
        return type;
    }
    if (type->kind == Types::Kind::Pointer && type->element) {
        Types::TypeRef elem = resolveCodegenType(type->element);
        if (elem != type->element) {
            return types_.pointerType(elem, type->isVolatile);
        }
    } else if (type->kind == Types::Kind::Array && type->element) {
        Types::TypeRef elem = resolveCodegenType(type->element);
        if (elem != type->element) {
            return types_.arrayType(elem, type->arrayLength);
        }
    } else if (type->kind == Types::Kind::Slice && type->element) {
        Types::TypeRef elem = resolveCodegenType(type->element);
        if (elem != type->element) {
            return types_.sliceType(elem);
        }
    }
    return type;
}

void CodeGenerator::generateGenericInstantiation(
    const Sema::GenericInstantiation& inst) {
    if (!inst.templateDecl) {
        return;
    }
    const AST::FunctionDeclaration& decl = *inst.templateDecl;

    const std::string& symbol = inst.mangledName;
    llvm::Function* fn = module->getFunction(symbol);
    if (!fn) {
        std::vector<llvm::Type*> paramTys;
        paramTys.reserve(inst.paramTypes.size());
        for (Types::TypeRef p : inst.paramTypes) {
            paramTys.push_back(lowerType(p));
        }
        llvm::Type* retTy = lowerType(inst.returnType);
        llvm::FunctionType* fnTy = llvm::FunctionType::get(retTy, paramTys, false);
        fn = llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage, symbol,
                                    module.get());
        unsigned idx = 0;
        for (auto& arg : fn->args()) {
            if (idx < inst.paramNames.size()) {
                arg.setName(inst.paramNames[idx]);
            }
            ++idx;
        }
    }
    functions_[symbol] = fn;
    if (!fn->empty()) {
        return;
    }

    auto& state = CodegenInternal::stateFor(this);
    std::unordered_map<std::string, Types::TypeRef> savedSubst = state.activeSubst;
    state.activeSubst.clear();
    for (size_t i = 0; i < decl.genericParams.size() && i < inst.typeArgs.size();
         ++i) {
        state.activeSubst[decl.genericParams[i]] =
            types_.fromString(inst.typeArgs[i]);
    }

    llvm::Function* savedFn = currentFunction_;
    Types::TypeRef savedRet = currentReturnType_;
    bool savedUnsafe = unsafeContext_;

    currentFunction_ = fn;
    currentReturnType_ = inst.returnType;
    unsafeContext_ = false;
    for (const auto& attr : decl.attributes) {
        if (attr.name == "unsafe" && (attr.value == "on" || attr.value.empty())) {
            unsafeContext_ = true;
        }
    }

    llvm::BasicBlock* entry = llvm::BasicBlock::Create(*context, "entry", fn);
    builder->SetInsertPoint(entry);

    pushScope();

    unsigned idx = 0;
    for (auto& arg : fn->args()) {
        Types::TypeRef paramType =
            idx < inst.paramTypes.size() ? inst.paramTypes[idx] : nullptr;
        std::string paramName = idx < inst.paramNames.size()
                                    ? inst.paramNames[idx]
                                    : ("arg" + std::to_string(idx));
        llvm::AllocaInst* slot =
            builder->CreateAlloca(arg.getType(), nullptr, paramName);
        builder->CreateStore(&arg, slot);
        declareLocal(paramName, slot, paramType);
        ++idx;
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
    state.activeSubst = savedSubst;
}

void CodeGenerator::generateGenericClassInstantiation(
    const Sema::GenericClassInstantiation& inst) {
    if (!inst.templateDecl || !sema_) {
        return;
    }
    const AST::ClassDeclaration& tmpl = *inst.templateDecl;
    const std::string& mangledClass = inst.mangledName;

    auto substituteSpelling = [](const std::string& spelling,
                                 const std::string& paramName,
                                 const std::string& concrete) -> std::string {
        std::string prefix;
        std::string s = spelling;
        const std::string vol = "volatile ";
        if (s.compare(0, vol.size(), vol) == 0) {
            prefix = vol;
            s = s.substr(vol.size());
        }
        while (!s.empty() && s.front() == ' ') {
            prefix += ' ';
            s.erase(s.begin());
        }
        size_t i = 0;
        while (i < s.size() &&
               (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_')) {
            ++i;
        }
        std::string token = s.substr(0, i);
        if (token != paramName) {
            return spelling;
        }
        return prefix + concrete + s.substr(i);
    };

    auto& state = CodegenInternal::stateFor(this);
    std::unordered_map<std::string, Types::TypeRef> savedSubst = state.activeSubst;
    std::string savedClassTemplate = state.activeClassTemplate;
    std::string savedClassInstance = state.activeClassInstance;
    state.activeSubst.clear();
    state.activeClassTemplate = tmpl.name;
    state.activeClassInstance = mangledClass;
    for (size_t i = 0;
         i < tmpl.genericParams.size() && i < inst.typeArgs.size(); ++i) {
        state.activeSubst[tmpl.genericParams[i]] =
            types_.fromString(inst.typeArgs[i]);
    }

    for (auto& method : tmpl.methods) {
        std::vector<std::string> paramTypeSpellings;
        for (const auto& p : method.parameters) {
            std::string s = p.type;
            for (size_t i = 0;
                 i < tmpl.genericParams.size() && i < inst.typeArgs.size(); ++i) {
                s = substituteSpelling(s, tmpl.genericParams[i], inst.typeArgs[i]);
            }
            paramTypeSpellings.push_back(s);
        }
        std::string mangled = Sema::mangleClassMember(
            mangledClass, method.name, method.isConstructor, method.isOperator,
            method.operatorSymbol, paramTypeSpellings);

        const Sema::FunctionInfo* info = nullptr;
        for (const auto& fn : sema_->functions) {
            if (fn.mangledName == mangled) {
                info = &fn;
                break;
            }
        }
        if (!info) {
            continue;
        }

        llvm::Function* fn = getOrDeclareFunction(*info);
        if (!fn || !fn->empty()) {
            continue;
        }

        llvm::Function* savedFn = currentFunction_;
        Types::TypeRef savedRet = currentReturnType_;
        bool savedUnsafe = unsafeContext_;

        currentFunction_ = fn;
        currentReturnType_ = info->returnType;
        unsafeContext_ = false;

        llvm::BasicBlock* entry = llvm::BasicBlock::Create(*context, "entry", fn);
        builder->SetInsertPoint(entry);

        pushScope();

        unsigned idx = 0;
        for (auto& arg : fn->args()) {
            Types::TypeRef paramType =
                idx < info->paramTypes.size() ? info->paramTypes[idx] : nullptr;
            std::string paramName = idx < info->paramNames.size()
                                        ? info->paramNames[idx]
                                        : ("arg" + std::to_string(idx));
            llvm::AllocaInst* slot =
                builder->CreateAlloca(arg.getType(), nullptr, paramName);
            builder->CreateStore(&arg, slot);
            declareLocal(paramName, slot, paramType);
            ++idx;
        }

        generateBlock(method.body);

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

    state.activeSubst = savedSubst;
    state.activeClassTemplate = savedClassTemplate;
    state.activeClassInstance = savedClassInstance;
}
