#pragma once


#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <extra/ast.hpp>
#include <extra/type_system.hpp>
#include <utilities/errors.hpp>

namespace Sema {

struct FunctionInfo {
    std::string name;
    std::string mangledName;
    std::vector<Types::TypeRef> paramTypes;
    std::vector<std::string> paramNames;
    Types::TypeRef returnType = nullptr;
    bool isExternal = false;
    bool isUnsafe = false;
    AST::FunctionDeclaration* decl = nullptr;
};

struct StructInfo {
    std::string name;
    std::vector<std::pair<std::string, Types::TypeRef>> fields;
    bool packed = false;
    int align = 0;
};

struct ClassInfo {
    std::string name;
    std::vector<std::pair<std::string, Types::TypeRef>> fields;
    std::vector<std::string> methodNames;
    std::string constructorMangled;
    std::vector<Types::TypeRef> constructorParams;
    std::map<std::string, std::string> methodMangled;
    std::map<std::string, std::string> operatorMangled;
};

struct EnumInfo {
    std::string name;
    Types::TypeRef underlying = nullptr;
    std::map<std::string, long long> variants;
};

struct GlobalInfo {
    std::string name;
    Types::TypeRef type = nullptr;
    bool isConst = false;
};

struct GenericInstantiation {
    std::string templateName;
    AST::FunctionDeclaration* templateDecl = nullptr;
    std::vector<std::string> typeArgs;
    std::string mangledName;
    std::vector<Types::TypeRef> paramTypes;
    std::vector<std::string> paramNames;
    Types::TypeRef returnType = nullptr;
};

struct GenericClassInstantiation {
    std::string mangledName;
    AST::ClassDeclaration* templateDecl = nullptr;
    std::vector<std::string> typeArgs;
};

struct SemaResult {
    std::string moduleName;
    std::vector<FunctionInfo> functions;
    std::vector<StructInfo> structs;
    std::vector<ClassInfo> classes;
    std::vector<EnumInfo> enums;
    std::vector<GlobalInfo> globals;
    std::unordered_map<const AST::ExprAST*, Types::TypeRef> exprTypes;
    std::vector<GenericInstantiation> genericInstantiations;
    std::vector<GenericClassInstantiation> genericClassInstantiations;
    std::unordered_map<const AST::ExprAST*, std::string> callTargets;
    bool ok = false;

    Types::TypeRef typeOf(const AST::ExprAST* node) const {
        auto it = exprTypes.find(node);
        return it == exprTypes.end() ? nullptr : it->second;
    }
};

class Analyzer {
public:
    Analyzer(Types::TypeContext& types, ErrorReporting::ErrorReporter* reporter);

    SemaResult analyze(const std::shared_ptr<AST::ProgramRoot>& program,
                       const std::vector<FunctionInfo>& importedFunctions = {},
                       const std::vector<StructInfo>& importedStructs = {});

private:
    class Impl;
    Types::TypeContext& types_;
    ErrorReporting::ErrorReporter* reporter_;
};

std::string mangleFunction(const std::string& moduleName, const std::string& functionName);
std::string mangleMethod(const std::string& typeName, const std::string& methodName,
                         const std::vector<std::string>& paramTypes);
std::string operatorMangleName(const std::string& symbol);
std::string mangleClassMember(const std::string& className, const std::string& memberName,
                              bool isConstructor, bool isOperator,
                              const std::string& operatorSymbol,
                              const std::vector<std::string>& paramTypes);

std::string mangleGenericInstance(const std::string& templateName,
                                  const std::vector<std::string>& typeArgs);

}
