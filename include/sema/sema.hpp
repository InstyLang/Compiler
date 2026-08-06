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
    bool isExported = false;
    // A monomorphized generic instantiation (function or class method). Emitted
    // with weak linkage so multiple modules that instantiate the same generic can
    // be linked together (the linker folds the duplicate definitions).
    bool isGenericInstance = false;
    AST::FunctionDeclaration* decl = nullptr;
};

struct StructInfo {
    std::string name;
    std::vector<std::pair<std::string, Types::TypeRef>> fields;
    bool packed = false;
    int align = 0;
    bool isExported = false;
};

struct ClassInfo {
    std::string name;
    std::vector<std::pair<std::string, Types::TypeRef>> fields;
    std::vector<std::string> methodNames;
    std::string constructorMangled;
    std::vector<Types::TypeRef> constructorParams;
    std::string destructorMangled;
    std::map<std::string, std::string> methodMangled;
    std::map<std::string, std::string> operatorMangled;
    bool isExported = false;
};

struct EnumInfo {
    std::string name;
    Types::TypeRef underlying = nullptr;
    std::map<std::string, long long> variants;
    bool isExported = false;
};

struct GlobalInfo {
    std::string name;
    Types::TypeRef type = nullptr;
    bool isConst = false;
    bool isExported = false;
};

// One variant of a tagged-union (sum-type) enum: its name, discriminant tag, and
// payload field types (empty for a unit variant).
struct SumVariant {
    std::string name;
    long long tag = 0;
    std::vector<Types::TypeRef> payload;
};

// A tagged-union enum. Its runtime representation is an aggregate `{ i64 tag;
// <payload storage> }`, so the type itself is registered as a Struct; this record
// carries the variant metadata used to type-check construction / `switch`.
struct SumTypeInfo {
    std::string name;
    std::vector<SumVariant> variants;
    bool isExported = false;
};

struct GenericInstantiation {
    std::string templateName;
    AST::FunctionDeclaration* templateDecl = nullptr;
    std::vector<std::string> typeArgs;
    std::string mangledName;
    std::vector<Types::TypeRef> paramTypes;
    std::vector<std::string> paramNames;
    Types::TypeRef returnType = nullptr;
    // This instantiation's own deep copy of the template, body included. Sema
    // checks and the backend emits this copy rather than `templateDecl`, so each
    // instantiation records and reads its own per-node types (see
    // GenericClassInstantiation::methods for the full reasoning).
    std::shared_ptr<AST::FunctionDeclaration> decl;
};

struct GenericClassInstantiation {
    std::string mangledName;
    AST::ClassDeclaration* templateDecl = nullptr;
    std::vector<std::string> typeArgs;
    // Deep copies of the template's methods, one set per instantiation, in the
    // template's declaration order. Sema checks these rather than the template's
    // own bodies so that each instantiation records its own per-node types
    // (SemaResult::exprTypes is keyed by AST::ExprAST*, so a shared body would
    // let the last instantiation overwrite the others'). The backend emits from
    // them for the same reason. Held by shared_ptr so the Method objects keep a
    // stable address as this vector grows.
    std::vector<std::shared_ptr<AST::Method>> methods;
};

struct SemaResult {
    std::string moduleName;
    std::vector<FunctionInfo> functions;
    std::vector<StructInfo> structs;
    std::vector<ClassInfo> classes;
    std::vector<EnumInfo> enums;
    std::vector<SumTypeInfo> sumTypes;
    std::vector<GlobalInfo> globals;
    std::unordered_map<const AST::ExprAST*, Types::TypeRef> exprTypes;
    std::vector<GenericInstantiation> genericInstantiations;
    std::vector<GenericClassInstantiation> genericClassInstantiations;
    std::unordered_map<const AST::ExprAST*, std::string> callTargets;
    // `X.insize` nodes and the type each one measures. Sema deliberately does not
    // compute the size: only the selector knows the final layout (field padding,
    // `packed`/`align` directives, sum-type tag + payload), so recording the type
    // and letting the backend size it keeps `.insize` equal to the bytes actually
    // emitted rather than to a second, possibly divergent, opinion.
    std::unordered_map<const AST::ExprAST*, Types::TypeRef> insizeTypes;
    // X.inalign nodes and the type each measures. Same arrangement as insizeTypes:
    // the alignment comes from the backend, which is the only place that knows it.
    std::unordered_map<const AST::ExprAST*, Types::TypeRef> inalignTypes;
    // Generic templates declared by this module (raw AST pointers, owned by the
    // module's ProgramRoot). Exported so importing modules can instantiate them
    // (cross-module generics). Only `export`ed templates should be propagated.
    std::vector<AST::ClassDeclaration*> genericClassTemplates;
    std::vector<AST::FunctionDeclaration*> genericFunctionTemplates;
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
                       const std::vector<StructInfo>& importedStructs = {},
                       const std::vector<ClassInfo>& importedClasses = {},
                       const std::vector<EnumInfo>& importedEnums = {},
                       const std::vector<AST::ClassDeclaration*>& importedClassTemplates = {},
                       const std::vector<AST::FunctionDeclaration*>& importedFunctionTemplates = {},
                       const std::vector<SumTypeInfo>& importedSumTypes = {});

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


