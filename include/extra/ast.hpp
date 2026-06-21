#pragma once


#include <memory>
#include <string>
#include <vector>

namespace AST {

enum class NodeType {
    ProgramRoot,
    ModuleDeclaration,
    ImportStatement,
    FunctionDeclaration,
    VariableDeclaration,
    AssignmentExpr,
    IfStatement,
    WhileLoop,
    InfiniteLoop,
    WhenStatement,
    SwitchStatement,
    ReturnStatement,
    BreakStatement,
    SkipStatement,
    UnsafeBlock,
    ExpressionStatement,

    IntegerLiteral,
    FloatLiteral,
    BoolLiteral,
    StringLiteral,
    IdentifierExpr,
    UnaryExpr,
    BinaryOperation,
    EqualityCheck,
    LogicalOperation,
    ShiftOperation,
    FunctionCall,
    BuiltinCall,
    CastExpr,
    AddressOfExpr,
    DereferenceExpr,
    MemberAccess,
    IndexExpr,
    ArrayLiteral,
    ObjectLiteral,
    ObjectProperty,
    StructInstantiation,
    NewExpression,
    DeleteExpression,
    InlineAsmExpr,

    TypeReference,
    StructDeclaration,
    EnumDeclaration,
    ClassDeclaration,
    ImplBlock,
    CompileTimeIf,

    Unknown
};

struct SourceRange {
    int startOffset = -1;
    int endOffset = -1;
    int startLine = 0;
    int startColumn = 0;
    int endLine = 0;
    int endColumn = 0;
};

struct Attribute {
    std::string name;
    std::string value;
};

struct ExprAST {
    SourceRange range;
    virtual ~ExprAST() = default;
    virtual NodeType nodeType() const = 0;
};

using NodePtr = std::shared_ptr<ExprAST>;
using NodeList = std::vector<std::shared_ptr<ExprAST>>;


struct ProgramRoot : ExprAST {
    std::string moduleName;
    std::vector<std::string> imports;
    NodeList body;
    NodeType nodeType() const override { return NodeType::ProgramRoot; }
};

struct ImportStatement : ExprAST {
    std::string moduleName;
    std::string alias;
    std::vector<std::string> importedSymbols;
    bool isWildcard = false;
    NodeType nodeType() const override { return NodeType::ImportStatement; }
};

struct Parameter {
    std::string name;
    std::string type;
    bool isVolatile = false;
};

struct FunctionDeclaration : ExprAST {
    std::string name;
    std::vector<Parameter> parameters;
    std::string returnType;
    std::vector<std::string> genericParams;
    std::vector<Attribute> attributes;
    NodeList body;
    bool hasBody = true;
    bool isExtern = false;   // declared with the `extern` keyword (external C symbol)
    NodeType nodeType() const override { return NodeType::FunctionDeclaration; }
};


struct VariableDeclarationExpr : ExprAST {
    std::string identifier;
    std::string typeHint;
    NodePtr initialValue;
    NodeList constructorArgs;
    bool isConst = false;
    bool isArray = false;
    int arraySize = 0;
    NodeType nodeType() const override { return NodeType::VariableDeclaration; }
};

struct AssignmentExpr : ExprAST {
    NodePtr target;
    NodePtr value;
    NodeType nodeType() const override { return NodeType::AssignmentExpr; }
};

struct IfStatement : ExprAST {
    NodePtr condition;
    NodeList consequent;
    NodeList alternate;
    NodeType nodeType() const override { return NodeType::IfStatement; }
};

struct WhileLoop : ExprAST {
    NodePtr condition;
    NodeList body;
    NodeType nodeType() const override { return NodeType::WhileLoop; }
};

struct InfiniteLoop : ExprAST {
    NodeList body;
    NodeType nodeType() const override { return NodeType::InfiniteLoop; }
};

struct WhenStatement : ExprAST {
    NodePtr condition;
    NodeList consequent;
    NodeType nodeType() const override { return NodeType::WhenStatement; }
};

struct SwitchArm {
    NodeList patterns;
    NodeList body;
    bool isDefault = false;
};

struct SwitchStatement : ExprAST {
    NodePtr subject;
    std::vector<SwitchArm> arms;
    NodeType nodeType() const override { return NodeType::SwitchStatement; }
};

struct ReturnStatement : ExprAST {
    NodePtr returnValue;
    NodeType nodeType() const override { return NodeType::ReturnStatement; }
};

struct BreakStatement : ExprAST {
    NodeType nodeType() const override { return NodeType::BreakStatement; }
};

struct SkipStatement : ExprAST {
    NodeType nodeType() const override { return NodeType::SkipStatement; }
};

struct UnsafeBlock : ExprAST {
    NodeList body;
    NodeType nodeType() const override { return NodeType::UnsafeBlock; }
};


struct IntegerLiteral : ExprAST {
    long long value = 0;
    std::string raw;
    NodeType nodeType() const override { return NodeType::IntegerLiteral; }
};

struct FloatLiteral : ExprAST {
    double value = 0.0;
    std::string raw;
    NodeType nodeType() const override { return NodeType::FloatLiteral; }
};

struct BoolLiteral : ExprAST {
    bool value = false;
    NodeType nodeType() const override { return NodeType::BoolLiteral; }
};

struct StringLiteral : ExprAST {
    std::string value;
    bool hasInterpolation = false;
    std::vector<std::string> literalParts;
    NodeList exprParts;
    NodeType nodeType() const override { return NodeType::StringLiteral; }
};

struct IdentifierExpr : ExprAST {
    std::string name;
    NodeType nodeType() const override { return NodeType::IdentifierExpr; }
};

struct UnaryExpr : ExprAST {
    std::string op;
    NodePtr operand;
    NodeType nodeType() const override { return NodeType::UnaryExpr; }
};

struct BinaryOperationExpr : ExprAST {
    std::string op;
    NodePtr lhs;
    NodePtr rhs;
    NodeType nodeType() const override { return NodeType::BinaryOperation; }
};

struct EqualityCheckExpr : ExprAST {
    std::string op;
    NodePtr left;
    NodePtr right;
    NodeType nodeType() const override { return NodeType::EqualityCheck; }
};

struct LogicalOperationExpr : ExprAST {
    std::string op;
    NodePtr left;
    NodePtr right;
    NodeType nodeType() const override { return NodeType::LogicalOperation; }
};

struct ShiftOperationExpr : ExprAST {
    std::string op;
    NodePtr lhs;
    NodePtr rhs;
    NodeType nodeType() const override { return NodeType::ShiftOperation; }
};

struct FunctionCallExpr : ExprAST {
    NodePtr callee;
    NodeList arguments;
    std::vector<std::string> genericArgs;
    NodeType nodeType() const override { return NodeType::FunctionCall; }
};

struct BuiltinCallExpr : ExprAST {
    std::string name;
    NodeList arguments;
    std::vector<std::string> genericArgs;
    NodeType nodeType() const override { return NodeType::BuiltinCall; }
};

struct CastExpr : ExprAST {
    std::string targetType;
    NodePtr expression;
    NodeType nodeType() const override { return NodeType::CastExpr; }
};

struct AddressOfExpr : ExprAST {
    NodePtr operand;
    NodeType nodeType() const override { return NodeType::AddressOfExpr; }
};

struct DereferenceExpr : ExprAST {
    NodePtr operand;
    NodeType nodeType() const override { return NodeType::DereferenceExpr; }
};

struct MemberAccessExpr : ExprAST {
    NodePtr object;
    NodePtr property;
    bool computed = false;
    bool isScope = false;
    NodeType nodeType() const override { return NodeType::MemberAccess; }
};

struct ArrayLiteral : ExprAST {
    NodeList elements;
    NodeType nodeType() const override { return NodeType::ArrayLiteral; }
};

struct ObjectProperty : ExprAST {
    std::string key;
    NodePtr value;
    NodeType nodeType() const override { return NodeType::ObjectProperty; }
};

struct ObjectLiteral : ExprAST {
    NodeList properties;
    NodeType nodeType() const override { return NodeType::ObjectLiteral; }
};

struct FieldValue {
    std::string name;
    NodePtr value;
};

struct StructInstantiation : ExprAST {
    std::string typeName;
    std::vector<FieldValue> fieldValues;
    NodeType nodeType() const override { return NodeType::StructInstantiation; }
};

struct NewExpression : ExprAST {
    std::string typeName;
    NodePtr initializer;
    NodePtr arraySize;
    NodeList arguments;
    NodeType nodeType() const override { return NodeType::NewExpression; }
};

struct DeleteExpression : ExprAST {
    NodePtr operand;
    NodeType nodeType() const override { return NodeType::DeleteExpression; }
};

struct InlineAsmExpr : ExprAST {
    std::string templateString;
    std::string constraints;
    std::string returnType;
    NodeList inputs;
    NodeType nodeType() const override { return NodeType::InlineAsmExpr; }
};


struct StructField {
    std::string name;
    std::string type;
};

struct StructDeclaration : ExprAST {
    std::string name;
    std::vector<std::string> genericParams;
    std::vector<Attribute> attributes;
    std::vector<StructField> fields;
    NodeType nodeType() const override { return NodeType::StructDeclaration; }
};

struct EnumVariant {
    std::string name;
    long long value = 0;
    bool hasExplicitValue = false;
};

struct EnumDeclaration : ExprAST {
    std::string name;
    std::string underlyingType;
    std::vector<EnumVariant> variants;
    NodeType nodeType() const override { return NodeType::EnumDeclaration; }
};

struct Method {
    std::string name;
    std::vector<Parameter> parameters;
    std::string returnType;
    std::vector<Attribute> attributes;
    bool isConstructor = false;
    bool isDestructor = false;
    bool isOperator = false;
    std::string operatorSymbol;
    NodeList body;
};

struct ClassDeclaration : ExprAST {
    std::string name;
    std::vector<std::string> genericParams;
    std::vector<Attribute> attributes;
    std::vector<StructField> fields;
    std::vector<Method> methods;
    NodeType nodeType() const override { return NodeType::ClassDeclaration; }
};

struct ImplBlock : ExprAST {
    std::string typeName;
    std::vector<Method> methods;
    NodeType nodeType() const override { return NodeType::ImplBlock; }
};

struct CompileTimeBranch {
    NodePtr condition;
    NodeList body;
};

struct CompileTimeIfExpr : ExprAST {
    std::vector<CompileTimeBranch> branches;
    NodeType nodeType() const override { return NodeType::CompileTimeIf; }
};


template <typename T>
NodeType nodeTypeOf();

#define INSTY_NODE_TYPE_OF(TYPE, ENUM) \
    template <> inline NodeType nodeTypeOf<TYPE>() { return NodeType::ENUM; }

INSTY_NODE_TYPE_OF(ProgramRoot, ProgramRoot)
INSTY_NODE_TYPE_OF(ImportStatement, ImportStatement)
INSTY_NODE_TYPE_OF(FunctionDeclaration, FunctionDeclaration)
INSTY_NODE_TYPE_OF(VariableDeclarationExpr, VariableDeclaration)
INSTY_NODE_TYPE_OF(AssignmentExpr, AssignmentExpr)
INSTY_NODE_TYPE_OF(IfStatement, IfStatement)
INSTY_NODE_TYPE_OF(WhileLoop, WhileLoop)
INSTY_NODE_TYPE_OF(InfiniteLoop, InfiniteLoop)
INSTY_NODE_TYPE_OF(WhenStatement, WhenStatement)
INSTY_NODE_TYPE_OF(SwitchStatement, SwitchStatement)
INSTY_NODE_TYPE_OF(ReturnStatement, ReturnStatement)
INSTY_NODE_TYPE_OF(BreakStatement, BreakStatement)
INSTY_NODE_TYPE_OF(SkipStatement, SkipStatement)
INSTY_NODE_TYPE_OF(UnsafeBlock, UnsafeBlock)
INSTY_NODE_TYPE_OF(IntegerLiteral, IntegerLiteral)
INSTY_NODE_TYPE_OF(FloatLiteral, FloatLiteral)
INSTY_NODE_TYPE_OF(BoolLiteral, BoolLiteral)
INSTY_NODE_TYPE_OF(StringLiteral, StringLiteral)
INSTY_NODE_TYPE_OF(IdentifierExpr, IdentifierExpr)
INSTY_NODE_TYPE_OF(UnaryExpr, UnaryExpr)
INSTY_NODE_TYPE_OF(BinaryOperationExpr, BinaryOperation)
INSTY_NODE_TYPE_OF(EqualityCheckExpr, EqualityCheck)
INSTY_NODE_TYPE_OF(LogicalOperationExpr, LogicalOperation)
INSTY_NODE_TYPE_OF(ShiftOperationExpr, ShiftOperation)
INSTY_NODE_TYPE_OF(FunctionCallExpr, FunctionCall)
INSTY_NODE_TYPE_OF(BuiltinCallExpr, BuiltinCall)
INSTY_NODE_TYPE_OF(CastExpr, CastExpr)
INSTY_NODE_TYPE_OF(AddressOfExpr, AddressOfExpr)
INSTY_NODE_TYPE_OF(DereferenceExpr, DereferenceExpr)
INSTY_NODE_TYPE_OF(MemberAccessExpr, MemberAccess)
INSTY_NODE_TYPE_OF(ArrayLiteral, ArrayLiteral)
INSTY_NODE_TYPE_OF(ObjectProperty, ObjectProperty)
INSTY_NODE_TYPE_OF(ObjectLiteral, ObjectLiteral)
INSTY_NODE_TYPE_OF(StructInstantiation, StructInstantiation)
INSTY_NODE_TYPE_OF(NewExpression, NewExpression)
INSTY_NODE_TYPE_OF(DeleteExpression, DeleteExpression)
INSTY_NODE_TYPE_OF(InlineAsmExpr, InlineAsmExpr)
INSTY_NODE_TYPE_OF(StructDeclaration, StructDeclaration)
INSTY_NODE_TYPE_OF(EnumDeclaration, EnumDeclaration)
INSTY_NODE_TYPE_OF(ClassDeclaration, ClassDeclaration)
INSTY_NODE_TYPE_OF(ImplBlock, ImplBlock)
INSTY_NODE_TYPE_OF(CompileTimeIfExpr, CompileTimeIf)

#undef INSTY_NODE_TYPE_OF

template <typename T>
std::shared_ptr<T> ast_cast(const std::shared_ptr<ExprAST>& node) {
    if (node && node->nodeType() == nodeTypeOf<T>()) {
        return std::static_pointer_cast<T>(node);
    }
    return nullptr;
}

}
