#include <codegen/codegen.hpp>
#include <codegen/codegen_internal.hpp>

#include <cstdio>
#include <mutex>
#include <system_error>

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm-c/Target.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Triple.h>


namespace CodegenInternal {

namespace {
std::unordered_map<const CodeGenerator*, GeneratorState> g_states;
std::mutex g_statesMutex;
}

GeneratorState& stateFor(const CodeGenerator* gen) {
    std::lock_guard<std::mutex> lock(g_statesMutex);
    return g_states[gen];
}

void releaseState(const CodeGenerator* gen) {
    std::lock_guard<std::mutex> lock(g_statesMutex);
    g_states.erase(gen);
}

bool isGenericFunction(const AST::FunctionDeclaration& decl) {
    return !decl.genericParams.empty();
}

bool callHasGenericArgs(const AST::FunctionCallExpr& call) {
    return !call.genericArgs.empty();
}

}


llvm::Type* LLVMTypeFactory::createVoid() {
    return llvm::Type::getVoidTy(context_);
}

llvm::IntegerType* LLVMTypeFactory::createInt(unsigned bitWidth) {
    if (bitWidth == 0) {
        bitWidth = 32;
    }
    return llvm::IntegerType::get(context_, bitWidth);
}

llvm::Type* LLVMTypeFactory::createFloat(unsigned bitWidth) {
    switch (bitWidth) {
        case 16:
            return llvm::Type::getHalfTy(context_);
        case 32:
            return llvm::Type::getFloatTy(context_);
        case 64:
            return llvm::Type::getDoubleTy(context_);
        case 128:
            return llvm::Type::getFP128Ty(context_);
        default:
            return llvm::Type::getDoubleTy(context_);
    }
}

llvm::PointerType* LLVMTypeFactory::createPointer() {
    return llvm::PointerType::get(context_, 0);
}

llvm::ConstantInt* LLVMTypeFactory::createConstInt(unsigned bitWidth, uint64_t value) {
    return llvm::ConstantInt::get(createInt(bitWidth), value, false);
}

llvm::Constant* LLVMTypeFactory::createNullPointer() {
    return llvm::ConstantPointerNull::get(createPointer());
}

llvm::Type* LLVMTypeFactory::fromEcxType(Types::TypeRef type) {
    if (!type) {
        return createVoid();
    }
    switch (type->kind) {
        case Types::Kind::Void:
        case Types::Kind::Error:
            return createVoid();
        case Types::Kind::Bool:
            return createInt(1);
        case Types::Kind::Int:
            return createInt(type->bitWidth ? static_cast<unsigned>(type->bitWidth) : 32);
        case Types::Kind::Float:
            return createFloat(type->bitWidth ? static_cast<unsigned>(type->bitWidth) : 64);
        case Types::Kind::Text:
        case Types::Kind::Pointer:
        case Types::Kind::Slice:
        case Types::Kind::Class:
            return createPointer();
        case Types::Kind::Array: {
            llvm::Type* elem = fromEcxType(type->element);
            uint64_t count = type->arrayLength > 0
                                 ? static_cast<uint64_t>(type->arrayLength)
                                 : 0;
            return llvm::ArrayType::get(elem, count);
        }
        case Types::Kind::Enum:
            return createInt(type->bitWidth ? static_cast<unsigned>(type->bitWidth) : 32);
        case Types::Kind::Struct:
            return createPointer();
        default:
            return createPointer();
    }
}


CodeGenerator::CodeGenerator(const Config::CompilerConfig& config,
                             Types::TypeContext& types,
                             ErrorReporting::ErrorReporter* reporter)
    : config_(config), types_(types), reporter_(reporter) {
    context = std::make_unique<llvm::LLVMContext>();
    targetSpec_ = config.target;
    runtimeOptions_ = config.runtimeOptions();

    moduleCtx.moduleName = "main";
    module = std::make_unique<llvm::Module>(moduleCtx.moduleName, *context);

    typeFactoryStorage_ = std::make_unique<LLVMTypeFactory>(*context, targetSpec_);
    typeFactory = typeFactoryStorage_.get();

    builder = std::make_unique<llvm::IRBuilder<>>(*context);
}

CodeGenerator::~CodeGenerator() {
    CodegenInternal::releaseState(this);
}


void CodeGenerator::reportCodegenError(const std::string& code, const std::string& message) {
    if (reporter_) {
        ErrorReporting::SourceLocation loc;
        reporter_->error(code, message, loc);
        return;
    }
    std::fprintf(stderr, "codegen error [%s]: %s\n", code.c_str(), message.c_str());
}


void CodeGenerator::pushScope() {
    valueScopes_.emplace_back();
}

void CodeGenerator::popScope() {
    if (!valueScopes_.empty()) {
        valueScopes_.pop_back();
    }
}

void CodeGenerator::declareLocal(const std::string& name, llvm::Value* address,
                                 Types::TypeRef type) {
    if (valueScopes_.empty()) {
        pushScope();
    }
    ValueSlot slot;
    slot.address = address;
    slot.type = type;
    slot.isPointerToValue = true;
    valueScopes_.back()[name] = slot;
}

const CodeGenerator::ValueSlot* CodeGenerator::lookupLocal(const std::string& name) const {
    for (auto it = valueScopes_.rbegin(); it != valueScopes_.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) {
            return &found->second;
        }
    }
    return nullptr;
}


namespace {
std::once_flag g_initTargets;
void ensureTargetsInitialized() {
    std::call_once(g_initTargets, [] {
        LLVMInitializeX86TargetInfo();
        LLVMInitializeX86Target();
        LLVMInitializeX86TargetMC();
        LLVMInitializeX86AsmParser();
        LLVMInitializeX86AsmPrinter();
        LLVMInitializeAArch64TargetInfo();
        LLVMInitializeAArch64Target();
        LLVMInitializeAArch64TargetMC();
        LLVMInitializeAArch64AsmParser();
        LLVMInitializeAArch64AsmPrinter();
    });
}

llvm::TargetMachine* buildTargetMachine(const Targeting::TargetSpec& spec,
                                        std::string& errorOut) {
    ensureTargetsInitialized();

    const std::string triple = spec.llvmTriple.empty()
                                   ? std::string("x86_64-pc-linux-gnu")
                                   : spec.llvmTriple;
    llvm::Triple tt(triple);

    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(tt, errorOut);
    if (!target) {
        return nullptr;
    }

    llvm::TargetOptions options;

    std::optional<llvm::Reloc::Model> reloc;
    if (spec.freestandingExecutable || spec.isEfi || spec.isInstantOS) {
        reloc = llvm::Reloc::Static;
    } else {
        reloc = llvm::Reloc::PIC_;
    }

    llvm::TargetMachine* tm = target->createTargetMachine(
        tt, "generic", "", options, reloc, std::nullopt,
        llvm::CodeGenOptLevel::Default);
    return tm;
}
}

std::string CodeGenerator::targetDataLayout() const {
    std::string error;
    llvm::TargetMachine* tm = buildTargetMachine(targetSpec_, error);
    if (!tm) {
        return std::string();
    }
    std::string layout = tm->createDataLayout().getStringRepresentation();
    delete tm;
    return layout;
}

void CodeGenerator::setupTargetMachine() {
    const std::string triple = targetSpec_.llvmTriple.empty()
                                   ? std::string("x86_64-pc-linux-gnu")
                                   : targetSpec_.llvmTriple;
    module->setTargetTriple(llvm::Triple(triple));

    std::string error;
    llvm::TargetMachine* tm = buildTargetMachine(targetSpec_, error);
    if (tm) {
        module->setDataLayout(tm->createDataLayout());
        delete tm;
    }
}


bool CodeGenerator::verify(std::string& errorOut) {
    std::string buffer;
    llvm::raw_string_ostream os(buffer);
    bool broken = llvm::verifyModule(*module, &os);
    os.flush();
    errorOut = buffer;
    return !broken;
}

std::string CodeGenerator::emitLLVM() {
    std::string buffer;
    llvm::raw_string_ostream os(buffer);
    module->print(os, nullptr);
    os.flush();
    return buffer;
}

bool CodeGenerator::emitObject(const std::string& path) {
    setupTargetMachine();

    std::string error;
    llvm::TargetMachine* tm = buildTargetMachine(targetSpec_, error);
    if (!tm) {
        reportCodegenError("E40010", "failed to create target machine: " + error);
        return false;
    }

    module->setDataLayout(tm->createDataLayout());

    std::error_code ec;
    llvm::raw_fd_ostream dest(path, ec, llvm::sys::fs::OF_None);
    if (ec) {
        reportCodegenError("E40011", "could not open object output '" + path +
                                         "': " + ec.message());
        delete tm;
        return false;
    }

    llvm::legacy::PassManager pass;
    if (tm->addPassesToEmitFile(pass, dest, nullptr,
                                llvm::CodeGenFileType::ObjectFile)) {
        reportCodegenError("E40012", "target machine cannot emit an object file");
        delete tm;
        return false;
    }

    pass.run(*module);
    dest.flush();
    delete tm;
    return true;
}


bool CodeGenerator::generate(const std::shared_ptr<AST::ProgramRoot>& program,
                             const Sema::SemaResult& sema) {
    sema_ = &sema;

    moduleCtx.moduleName = !sema.moduleName.empty() ? sema.moduleName
                           : (program ? program->moduleName : std::string("main"));
    if (moduleCtx.moduleName.empty()) {
        bool hasMain = false;
        for (const auto& info : sema.functions) {
            if (!info.isExternal && info.name == "main") {
                hasMain = true;
                break;
            }
        }
        moduleCtx.moduleName = hasMain ? "main" : "module";
    }
    module->setModuleIdentifier(moduleCtx.moduleName);
    module->setSourceFileName(moduleCtx.moduleName);

    setupTargetMachine();

    auto& state = CodegenInternal::stateFor(this);
    for (const auto& s : sema.structs) {
        std::vector<llvm::Type*> fieldTys;
        CodegenInternal::StructLayout layout;
        for (const auto& field : s.fields) {
            llvm::Type* lowered = lowerType(field.second);
            fieldTys.push_back(lowered);
            layout.fields.emplace_back(field.first, field.second);
        }
        llvm::StructType* st =
            llvm::StructType::create(*context, fieldTys, "struct." + s.name, s.packed);
        layout.llvmType = st;
        state.structs[s.name] = std::move(layout);
    }
    for (const auto& e : sema.enums) {
        llvm::Type* underlying = e.underlying ? lowerType(e.underlying)
                                              : typeFactory->createInt(32);
        state.enumUnderlying[e.name] = underlying;
        for (const auto& [variantName, value] : e.variants) {
            state.enumConstants[variantName] = {value, e.underlying};
        }
    }

    declareFunctions(program);

    for (const auto& inst : sema.genericInstantiations) {
        if (inst.mangledName.empty() || module->getFunction(inst.mangledName)) {
            continue;
        }
        std::vector<llvm::Type*> paramTys;
        paramTys.reserve(inst.paramTypes.size());
        for (Types::TypeRef p : inst.paramTypes) {
            paramTys.push_back(lowerType(p));
        }
        llvm::Type* retTy = lowerType(inst.returnType);
        llvm::FunctionType* fnTy = llvm::FunctionType::get(retTy, paramTys, false);
        llvm::Function* fn = llvm::Function::Create(
            fnTy, llvm::Function::ExternalLinkage, inst.mangledName, module.get());
        unsigned aidx = 0;
        for (auto& arg : fn->args()) {
            if (aidx < inst.paramNames.size()) {
                arg.setName(inst.paramNames[aidx]);
            }
            ++aidx;
        }
    }

    if (program) {
        std::unordered_map<std::string, AST::VariableDeclarationExpr*> initByName;
        for (const auto& node : program->body) {
            if (!node) {
                continue;
            }
            if (node->nodeType() == AST::NodeType::VariableDeclaration) {
                auto* vd = static_cast<AST::VariableDeclarationExpr*>(node.get());
                initByName[vd->identifier] = vd;
            }
        }

        for (const auto& g : sema.globals) {
            if (!g.type || g.type->isError()) {
                continue;
            }
            llvm::Type* ty = lowerType(g.type);
            llvm::Constant* init = llvm::Constant::getNullValue(ty);
            auto it = initByName.find(g.name);
            if (it != initByName.end() && it->second->initialValue &&
                ty->isIntegerTy()) {
                if (auto lit = AST::ast_cast<AST::IntegerLiteral>(it->second->initialValue)) {
                    init = llvm::ConstantInt::get(ty, static_cast<uint64_t>(lit->value),
                                                  g.type->isSigned);
                }
            }
            auto* gv = new llvm::GlobalVariable(
                *module, ty, g.isConst, llvm::GlobalValue::InternalLinkage, init, g.name);
            gv->setAlignment(llvm::MaybeAlign());
            state.globals[g.name] = {gv, g.type};
        }
    }

    for (const auto& info : sema.functions) {
        if (info.isExternal || !info.decl || !info.decl->hasBody) {
            continue;
        }
        if (CodegenInternal::isGenericFunction(*info.decl)) {
            continue;
        }
        generateFunctionBody(*info.decl, info);
    }

    for (const auto& inst : sema.genericInstantiations) {
        generateGenericInstantiation(inst);
    }

    if (program) {
        for (const auto& node : program->body) {
            if (!node || node->nodeType() != AST::NodeType::ClassDeclaration) {
                continue;
            }
            auto* cls = static_cast<AST::ClassDeclaration*>(node.get());
            const std::string& className = cls->name;

            if (!cls->genericParams.empty()) {
                continue;
            }

            for (auto& method : cls->methods) {
                std::vector<std::string> paramTypeSpellings;
                for (const auto& p : method.parameters) {
                    paramTypeSpellings.push_back(p.type);
                }
                std::string mangled = Sema::mangleClassMember(
                    className, method.name, method.isConstructor, method.isOperator,
                    method.operatorSymbol, paramTypeSpellings);

                const Sema::FunctionInfo* info = nullptr;
                for (const auto& fn : sema.functions) {
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

                llvm::BasicBlock* entry =
                    llvm::BasicBlock::Create(*context, "entry", fn);
                builder->SetInsertPoint(entry);

                pushScope();

                unsigned idx = 0;
                for (auto& arg : fn->args()) {
                    Types::TypeRef paramType =
                        idx < info->paramTypes.size() ? info->paramTypes[idx] : nullptr;
                    std::string paramName =
                        idx < info->paramNames.size()
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
        }
    }

    for (const auto& gci : sema.genericClassInstantiations) {
        generateGenericClassInstantiation(gci);
    }

    generateRuntimeFunctions();

    if (targetSpec_.isEfi && module->getFunction("main")) {
        generateChkstkFunction();
    }

    std::string verifyError;
    if (!verify(verifyError)) {
        reportCodegenError("E40000", "module verification failed: " + verifyError);
        return false;
    }

    return true;
}
