#pragma once


#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <extra/ast.hpp>
#include <extra/type_system.hpp>
#include <sema/sema.hpp>
#include <utilities/config.hpp>
#include <utilities/errors.hpp>
#include <utilities/runtime.hpp>
#include <utilities/target.hpp>

class LLVMTypeFactory {
public:
    LLVMTypeFactory(llvm::LLVMContext& context, const Targeting::TargetSpec& target)
        : context_(context), target_(target) {}

    llvm::Type* createVoid();
    llvm::IntegerType* createInt(unsigned bitWidth);
    llvm::Type* createFloat(unsigned bitWidth);
    llvm::PointerType* createPointer();
    llvm::ConstantInt* createConstInt(unsigned bitWidth, uint64_t value);
    llvm::Constant* createNullPointer();

    llvm::Type* fromEcxType(Types::TypeRef type);

private:
    llvm::LLVMContext& context_;
    const Targeting::TargetSpec& target_;
};

struct ModuleContext {
    std::string moduleName = "main";
    std::vector<std::shared_ptr<void>> cimportParsers;
};

class CodeGenerator {
public:
    CodeGenerator(const Config::CompilerConfig& config, Types::TypeContext& types,
                  ErrorReporting::ErrorReporter* reporter);
    ~CodeGenerator();

    bool generate(const std::shared_ptr<AST::ProgramRoot>& program, const Sema::SemaResult& sema);

    bool verify(std::string& errorOut);
    std::string emitLLVM();
    bool emitObject(const std::string& path);

    llvm::Module* getModule() { return module.get(); }

    void generateRuntimeFunctions();
    void generateCoreRuntimeFunctions();
    void generateAllocatorRuntimeFunctions();
    void generateHostedStdRuntimeFunctions();
    void generatePlatformRuntimeFunctions();

    llvm::Function* generateMemcpyFunction();
    llvm::Function* generateMemsetFunction();
    llvm::Function* emitCoreFunction(const std::string& coreSymbol,
                                     const std::string& asSymbol = "");
    llvm::Function* generateMallocFunction();
    llvm::Function* generateFreeFunction();
    llvm::Function* generateReallocFunction();
    llvm::Function* generateChkstkFunction();
    llvm::Function* generateStartFunction();
    llvm::Function* generateInstantOSStartFunction();

    void reportCodegenError(const std::string& code, const std::string& message);

    std::unique_ptr<llvm::LLVMContext> context;
    std::unique_ptr<llvm::Module> module;
    std::unique_ptr<llvm::IRBuilder<>> builder;
    LLVMTypeFactory* typeFactory = nullptr;

    Runtime::Options runtimeOptions_;
    Targeting::TargetSpec targetSpec_;
    ModuleContext moduleCtx;

private:
    const Config::CompilerConfig& config_;
    Types::TypeContext& types_;
    ErrorReporting::ErrorReporter* reporter_;
    const Sema::SemaResult* sema_ = nullptr;
    std::unique_ptr<LLVMTypeFactory> typeFactoryStorage_;

    struct ValueSlot {
        llvm::Value* address = nullptr;
        Types::TypeRef type = nullptr;
        bool isPointerToValue = true;
    };
    std::vector<std::map<std::string, ValueSlot>> valueScopes_;
    std::map<std::string, llvm::Function*> functions_;
    std::map<std::string, llvm::GlobalVariable*> stringConstants_;
    std::map<std::string, llvm::GlobalVariable*> wideStringConstants_;

    bool unsafeContext_ = false;
    llvm::Function* currentFunction_ = nullptr;
    Types::TypeRef currentReturnType_ = nullptr;
    llvm::BasicBlock* currentLoopBreak_ = nullptr;
    llvm::BasicBlock* currentLoopContinue_ = nullptr;

    void pushScope();
    void popScope();
    void declareLocal(const std::string& name, llvm::Value* address, Types::TypeRef type);
    const ValueSlot* lookupLocal(const std::string& name) const;

    void declareFunctions(const std::shared_ptr<AST::ProgramRoot>& program);
    void generateFunctionBody(const AST::FunctionDeclaration& decl, const Sema::FunctionInfo& info);
    llvm::Function* getOrDeclareFunction(const Sema::FunctionInfo& info);

    void generateGenericInstantiation(const Sema::GenericInstantiation& inst);
    void generateGenericClassInstantiation(const Sema::GenericClassInstantiation& inst);
    Types::TypeRef resolveCodegenType(Types::TypeRef type);

    void generateStatement(const AST::NodePtr& stmt);
    void generateBlock(const AST::NodeList& body);
    bool blockTerminates(const AST::NodeList& body) const;

    llvm::Value* generateExpression(const AST::NodePtr& expr);
    llvm::Value* generateLValue(const AST::NodePtr& expr, Types::TypeRef& outType);
    llvm::Value* generateCall(const AST::FunctionCallExpr& call);
    llvm::Value* generateIntrinsicCall(const AST::FunctionCallExpr& call,
                                       const std::string& name, bool& handled);
    llvm::Value* coerce(llvm::Value* value, Types::TypeRef from, Types::TypeRef to);
    llvm::Value* coerceIntTo(llvm::Value* value, unsigned bits);
    llvm::Value* tryClassOperator(const AST::NodePtr& lhs, const AST::NodePtr& rhs,
                                  const std::string& op);

    llvm::Value* getStringConstant(const std::string& text);
    llvm::Value* getWideStringConstant(const std::string& text);

    llvm::Value* generateInterpolation(const AST::StringLiteral& literal);
    llvm::Value* emitFormatValue(llvm::Value* buf, llvm::Value* cap,
                                 llvm::Value* off, llvm::Value* value,
                                 Types::TypeRef type, unsigned recursionDepth);
    llvm::Value* emitFormatAggregate(llvm::Value* buf, llvm::Value* cap,
                                     llvm::Value* off, llvm::Value* value,
                                     Types::TypeRef type, unsigned recursionDepth);
    llvm::Value* materializeAddr(llvm::Value* value, Types::TypeRef type);
    void generateFormatRuntimeFunctions();
    llvm::Function* fmtRaw_();
    llvm::Function* fmtInt_();
    llvm::Function* fmtFloat_();
    llvm::Function* fmtFinish_();

    llvm::Type* lowerType(Types::TypeRef type);

    llvm::Value* generateBuiltinCall(const AST::BuiltinCallExpr& call);

    void setupTargetMachine();
    std::string targetDataLayout() const;
};
