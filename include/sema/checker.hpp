#pragma once


#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <extra/ast.hpp>
#include <extra/type_system.hpp>
#include <sema/sema.hpp>
#include <utilities/errors.hpp>

namespace Sema {

struct Scope {
    std::map<std::string, Types::TypeRef> vars;
};

class Checker {
public:
    Checker(Types::TypeContext& types, ErrorReporting::ErrorReporter* reporter,
            SemaResult& result);

    void run(const std::shared_ptr<AST::ProgramRoot>& program,
             const std::vector<FunctionInfo>& importedFunctions,
             const std::vector<StructInfo>& importedStructs);

    void declarePrepass(const std::shared_ptr<AST::ProgramRoot>& program);
    void declareStruct(AST::StructDeclaration* node);
    void declareEnum(AST::EnumDeclaration* node);
    void declareClass(AST::ClassDeclaration* node);
    void checkClassMethod(const std::string& className, Types::TypeRef classPtr,
                          AST::Method& method);
    void declareGlobal(AST::VariableDeclarationExpr* node);
    void declareFunction(AST::FunctionDeclaration* node);
    std::string computeMangledName(AST::FunctionDeclaration* node, bool& outIsExtern);

    static std::string substituteSpelling(const std::string& spelling,
                                          const std::string& paramName,
                                          const std::string& concreteSpelling);
    void checkInstantiationBody(const GenericInstantiation& inst);

    std::string instantiateGenericClass(const std::string& templateName,
                                        const std::vector<std::string>& typeArgs,
                                        const AST::ExprAST* at);
    struct PendingGenericMethod {
        std::string className;
        Types::TypeRef classPtr = nullptr;
        AST::Method* method = nullptr;
        std::map<std::string, Types::TypeRef> subst;
    };
    void checkGenericClassMethod(const PendingGenericMethod& pm);

    void checkFunction(const FunctionInfo& info);
    void checkBlock(const AST::NodeList& body);
    void checkStatement(const AST::NodePtr& node);
    Types::TypeRef checkExpr(const AST::NodePtr& node);

    void checkVarDecl(AST::VariableDeclarationExpr* node);
    void checkAssignment(AST::AssignmentExpr* node);
    void checkIf(AST::IfStatement* node);
    void checkWhile(AST::WhileLoop* node);
    void checkLoop(AST::InfiniteLoop* node);
    void checkWhen(AST::WhenStatement* node);
    void checkSwitch(AST::SwitchStatement* node);
    void checkReturn(AST::ReturnStatement* node);
    void checkUnsafe(AST::UnsafeBlock* node);

    Types::TypeRef checkIdentifier(AST::IdentifierExpr* node);
    Types::TypeRef checkUnary(AST::UnaryExpr* node);
    Types::TypeRef checkBinary(AST::BinaryOperationExpr* node);
    Types::TypeRef checkEquality(AST::EqualityCheckExpr* node);
    Types::TypeRef checkLogical(AST::LogicalOperationExpr* node);
    Types::TypeRef checkShift(AST::ShiftOperationExpr* node);
    Types::TypeRef checkCall(AST::FunctionCallExpr* node);
    bool checkIntrinsicCall(AST::FunctionCallExpr* node, const std::string& name,
                            Types::TypeRef& out);
    Types::TypeRef checkBuiltin(AST::BuiltinCallExpr* node);
    Types::TypeRef checkCast(AST::CastExpr* node);
    Types::TypeRef checkAddressOf(AST::AddressOfExpr* node);
    Types::TypeRef checkDeref(AST::DereferenceExpr* node);
    Types::TypeRef checkMember(AST::MemberAccessExpr* node);
    Types::TypeRef checkIndex(AST::MemberAccessExpr* node);
    void checkInterpolation(AST::StringLiteral* node);
    bool isFormattable(Types::TypeRef t);

    Types::TypeRef resolveTypeSpelling(const std::string& spelling, const AST::ExprAST* at);
    bool isAssignable(Types::TypeRef target, Types::TypeRef value, bool valueIsLiteral);
    Types::TypeRef enumUnderlying(Types::TypeRef t) const;
    Types::TypeRef arithResult(Types::TypeRef a, Types::TypeRef b);
    bool isLValue(const AST::NodePtr& node);
    bool blockReturns(const AST::NodeList& body);

    void pushScope();
    void popScope();
    bool declareLocal(const std::string& name, Types::TypeRef type, const AST::ExprAST* at);
    Types::TypeRef lookupLocal(const std::string& name);

    Types::TypeRef record(const AST::ExprAST* node, Types::TypeRef type);
    bool isIntLiteral(const AST::NodePtr& node) const;

    static ErrorReporting::SourceLocation locOf(const AST::ExprAST* node);
    void emit(const std::string& code, const std::string& message,
              const AST::ExprAST* at, const std::string& hint = "");
    bool alreadyErrored(const AST::ExprAST* node);
    void markErrored(const AST::ExprAST* node);

private:
    Types::TypeContext& types_;
    ErrorReporting::ErrorReporter* reporter_;
    SemaResult& result_;

    std::vector<Scope> scopes_;

    std::multimap<std::string, const FunctionInfo*> functionTable_;
    std::vector<FunctionInfo> importedStore_;

    std::map<std::string, AST::FunctionDeclaration*> genericTemplates_;
    std::map<std::string, AST::ClassDeclaration*> genericClassTemplates_;
    std::map<std::string, bool> instantiatedGenericClasses_;
    std::vector<PendingGenericMethod> pendingGenericMethods_;
    std::map<std::string, Types::TypeRef> currentSubst_;

    std::map<std::string, Types::TypeRef> enumConstants_;

    struct PendingMethod {
        std::string className;
        Types::TypeRef classPtr = nullptr;
        AST::Method* method = nullptr;
    };
    std::vector<PendingMethod> pendingMethods_;
    std::map<std::string, std::vector<std::pair<std::string, Types::TypeRef>>> classFields_;
    Types::TypeRef currentThis_ = nullptr;
    std::string currentClass_;

    const FunctionInfo* currentFn_ = nullptr;
    Types::TypeRef currentReturn_ = nullptr;
    bool inUnsafe_ = false;

    std::unordered_map<const AST::ExprAST*, bool> errored_;
};

}
