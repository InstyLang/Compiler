#include "extra/ast_clone.hpp"

#include <memory>

namespace AST {

namespace {

// Copies the fields every node carries. Called by each concrete case so the
// clone keeps its source location (diagnostics point at the right place even
// when the node reached the checker through a generic instantiation).
template <typename T>
std::shared_ptr<T> make(const ExprAST& from) {
    auto node = std::make_shared<T>();
    node->range = from.range;
    return node;
}

std::vector<SwitchArm> cloneSwitchArms(const std::vector<SwitchArm>& arms) {
    std::vector<SwitchArm> out;
    out.reserve(arms.size());
    for (const auto& arm : arms) {
        SwitchArm copy;
        copy.patterns = cloneNodeList(arm.patterns);
        copy.body = cloneNodeList(arm.body);
        copy.isDefault = arm.isDefault;
        out.push_back(std::move(copy));
    }
    return out;
}

std::vector<MatchArm> cloneMatchArms(const std::vector<MatchArm>& arms) {
    std::vector<MatchArm> out;
    out.reserve(arms.size());
    for (const auto& arm : arms) {
        MatchArm copy;
        copy.variant = arm.variant;
        copy.bindings = arm.bindings;
        copy.body = cloneNodeList(arm.body);
        copy.isDefault = arm.isDefault;
        out.push_back(std::move(copy));
    }
    return out;
}

std::vector<FieldValue> cloneFieldValues(const std::vector<FieldValue>& values) {
    std::vector<FieldValue> out;
    out.reserve(values.size());
    for (const auto& fv : values) {
        FieldValue copy;
        copy.name = fv.name;
        copy.value = cloneNode(fv.value);
        out.push_back(std::move(copy));
    }
    return out;
}

std::vector<CompileTimeBranch> cloneBranches(
    const std::vector<CompileTimeBranch>& branches) {
    std::vector<CompileTimeBranch> out;
    out.reserve(branches.size());
    for (const auto& b : branches) {
        CompileTimeBranch copy;
        copy.condition = cloneNode(b.condition);
        copy.body = cloneNodeList(b.body);
        out.push_back(std::move(copy));
    }
    return out;
}

}  // namespace

std::shared_ptr<FunctionDeclaration> cloneFunctionDeclaration(
    const FunctionDeclaration& fn) {
    auto out = make<FunctionDeclaration>(fn);
    out->name = fn.name;
    out->parameters = fn.parameters;
    out->returnType = fn.returnType;
    out->genericParams = fn.genericParams;
    out->attributes = fn.attributes;
    out->body = cloneNodeList(fn.body);
    out->hasBody = fn.hasBody;
    out->isExtern = fn.isExtern;
    out->isExported = fn.isExported;
    return out;
}

NodeList cloneNodeList(const NodeList& list) {
    NodeList out;
    out.reserve(list.size());
    for (const auto& node : list) {
        out.push_back(cloneNode(node));
    }
    return out;
}

NodePtr cloneNode(const NodePtr& node) {
    if (!node) {
        return nullptr;
    }

    switch (node->nodeType()) {
        case NodeType::ProgramRoot: {
            const auto& n = static_cast<const ProgramRoot&>(*node);
            auto out = make<ProgramRoot>(n);
            out->moduleName = n.moduleName;
            out->imports = n.imports;
            out->body = cloneNodeList(n.body);
            return out;
        }
        case NodeType::ImportStatement: {
            const auto& n = static_cast<const ImportStatement&>(*node);
            auto out = make<ImportStatement>(n);
            out->moduleName = n.moduleName;
            out->alias = n.alias;
            out->importedSymbols = n.importedSymbols;
            out->isWildcard = n.isWildcard;
            return out;
        }
        case NodeType::FunctionDeclaration: {
            const auto& n = static_cast<const FunctionDeclaration&>(*node);
            return cloneFunctionDeclaration(n);
        }
        case NodeType::VariableDeclaration: {
            const auto& n = static_cast<const VariableDeclarationExpr&>(*node);
            auto out = make<VariableDeclarationExpr>(n);
            out->identifier = n.identifier;
            out->typeHint = n.typeHint;
            out->initialValue = cloneNode(n.initialValue);
            out->constructorArgs = cloneNodeList(n.constructorArgs);
            out->isConst = n.isConst;
            out->isArray = n.isArray;
            out->arraySize = n.arraySize;
            out->isExported = n.isExported;
            return out;
        }
        case NodeType::AssignmentExpr: {
            const auto& n = static_cast<const AssignmentExpr&>(*node);
            auto out = make<AssignmentExpr>(n);
            out->target = cloneNode(n.target);
            out->value = cloneNode(n.value);
            return out;
        }
        case NodeType::IfStatement: {
            const auto& n = static_cast<const IfStatement&>(*node);
            auto out = make<IfStatement>(n);
            out->condition = cloneNode(n.condition);
            out->consequent = cloneNodeList(n.consequent);
            out->alternate = cloneNodeList(n.alternate);
            return out;
        }
        case NodeType::WhileLoop: {
            const auto& n = static_cast<const WhileLoop&>(*node);
            auto out = make<WhileLoop>(n);
            out->condition = cloneNode(n.condition);
            out->body = cloneNodeList(n.body);
            return out;
        }
        case NodeType::InfiniteLoop: {
            const auto& n = static_cast<const InfiniteLoop&>(*node);
            auto out = make<InfiniteLoop>(n);
            out->body = cloneNodeList(n.body);
            return out;
        }
        case NodeType::ForLoop: {
            const auto& n = static_cast<const ForLoop&>(*node);
            auto out = make<ForLoop>(n);
            out->varName = n.varName;
            out->isRange = n.isRange;
            out->iterable = cloneNode(n.iterable);
            out->rangeStart = cloneNode(n.rangeStart);
            out->rangeEnd = cloneNode(n.rangeEnd);
            out->body = cloneNodeList(n.body);
            return out;
        }
        case NodeType::WhenStatement: {
            const auto& n = static_cast<const WhenStatement&>(*node);
            auto out = make<WhenStatement>(n);
            out->condition = cloneNode(n.condition);
            out->consequent = cloneNodeList(n.consequent);
            return out;
        }
        case NodeType::SwitchStatement: {
            const auto& n = static_cast<const SwitchStatement&>(*node);
            auto out = make<SwitchStatement>(n);
            out->subject = cloneNode(n.subject);
            out->arms = cloneSwitchArms(n.arms);
            return out;
        }
        case NodeType::MatchStatement: {
            const auto& n = static_cast<const MatchStatement&>(*node);
            auto out = make<MatchStatement>(n);
            out->subject = cloneNode(n.subject);
            out->arms = cloneMatchArms(n.arms);
            return out;
        }
        case NodeType::ReturnStatement: {
            const auto& n = static_cast<const ReturnStatement&>(*node);
            auto out = make<ReturnStatement>(n);
            out->returnValue = cloneNode(n.returnValue);
            return out;
        }
        case NodeType::BreakStatement:
            return make<BreakStatement>(*node);
        case NodeType::SkipStatement:
            return make<SkipStatement>(*node);
        case NodeType::UnsafeBlock: {
            const auto& n = static_cast<const UnsafeBlock&>(*node);
            auto out = make<UnsafeBlock>(n);
            out->body = cloneNodeList(n.body);
            return out;
        }
        case NodeType::IntegerLiteral: {
            const auto& n = static_cast<const IntegerLiteral&>(*node);
            auto out = make<IntegerLiteral>(n);
            out->value = n.value;
            out->raw = n.raw;
            return out;
        }
        case NodeType::FloatLiteral: {
            const auto& n = static_cast<const FloatLiteral&>(*node);
            auto out = make<FloatLiteral>(n);
            out->value = n.value;
            out->raw = n.raw;
            return out;
        }
        case NodeType::BoolLiteral: {
            const auto& n = static_cast<const BoolLiteral&>(*node);
            auto out = make<BoolLiteral>(n);
            out->value = n.value;
            return out;
        }
        case NodeType::StringLiteral: {
            const auto& n = static_cast<const StringLiteral&>(*node);
            auto out = make<StringLiteral>(n);
            out->value = n.value;
            out->hasInterpolation = n.hasInterpolation;
            out->literalParts = n.literalParts;
            out->exprParts = cloneNodeList(n.exprParts);
            return out;
        }
        case NodeType::IdentifierExpr: {
            const auto& n = static_cast<const IdentifierExpr&>(*node);
            auto out = make<IdentifierExpr>(n);
            out->name = n.name;
            return out;
        }
        case NodeType::UnaryExpr: {
            const auto& n = static_cast<const UnaryExpr&>(*node);
            auto out = make<UnaryExpr>(n);
            out->op = n.op;
            out->operand = cloneNode(n.operand);
            return out;
        }
        case NodeType::BinaryOperation: {
            const auto& n = static_cast<const BinaryOperationExpr&>(*node);
            auto out = make<BinaryOperationExpr>(n);
            out->op = n.op;
            out->lhs = cloneNode(n.lhs);
            out->rhs = cloneNode(n.rhs);
            return out;
        }
        case NodeType::EqualityCheck: {
            const auto& n = static_cast<const EqualityCheckExpr&>(*node);
            auto out = make<EqualityCheckExpr>(n);
            out->op = n.op;
            out->left = cloneNode(n.left);
            out->right = cloneNode(n.right);
            return out;
        }
        case NodeType::LogicalOperation: {
            const auto& n = static_cast<const LogicalOperationExpr&>(*node);
            auto out = make<LogicalOperationExpr>(n);
            out->op = n.op;
            out->left = cloneNode(n.left);
            out->right = cloneNode(n.right);
            return out;
        }
        case NodeType::ShiftOperation: {
            const auto& n = static_cast<const ShiftOperationExpr&>(*node);
            auto out = make<ShiftOperationExpr>(n);
            out->op = n.op;
            out->lhs = cloneNode(n.lhs);
            out->rhs = cloneNode(n.rhs);
            return out;
        }
        case NodeType::FunctionCall: {
            const auto& n = static_cast<const FunctionCallExpr&>(*node);
            auto out = make<FunctionCallExpr>(n);
            out->callee = cloneNode(n.callee);
            out->arguments = cloneNodeList(n.arguments);
            out->genericArgs = n.genericArgs;
            return out;
        }
        case NodeType::BuiltinCall: {
            const auto& n = static_cast<const BuiltinCallExpr&>(*node);
            auto out = make<BuiltinCallExpr>(n);
            out->name = n.name;
            out->arguments = cloneNodeList(n.arguments);
            out->genericArgs = n.genericArgs;
            return out;
        }
        case NodeType::CastExpr: {
            const auto& n = static_cast<const CastExpr&>(*node);
            auto out = make<CastExpr>(n);
            out->targetType = n.targetType;
            out->expression = cloneNode(n.expression);
            return out;
        }
        case NodeType::AddressOfExpr: {
            const auto& n = static_cast<const AddressOfExpr&>(*node);
            auto out = make<AddressOfExpr>(n);
            out->operand = cloneNode(n.operand);
            return out;
        }
        case NodeType::DereferenceExpr: {
            const auto& n = static_cast<const DereferenceExpr&>(*node);
            auto out = make<DereferenceExpr>(n);
            out->operand = cloneNode(n.operand);
            return out;
        }
        case NodeType::MemberAccess: {
            const auto& n = static_cast<const MemberAccessExpr&>(*node);
            auto out = make<MemberAccessExpr>(n);
            out->object = cloneNode(n.object);
            out->property = cloneNode(n.property);
            out->computed = n.computed;
            out->isScope = n.isScope;
            return out;
        }
        case NodeType::ArrayLiteral: {
            const auto& n = static_cast<const ArrayLiteral&>(*node);
            auto out = make<ArrayLiteral>(n);
            out->elements = cloneNodeList(n.elements);
            return out;
        }
        case NodeType::SliceExpr: {
            const auto& n = static_cast<const SliceExpr&>(*node);
            auto out = make<SliceExpr>(n);
            out->object = cloneNode(n.object);
            out->start = cloneNode(n.start);
            out->end = cloneNode(n.end);
            return out;
        }
        case NodeType::ObjectProperty: {
            const auto& n = static_cast<const ObjectProperty&>(*node);
            auto out = make<ObjectProperty>(n);
            out->key = n.key;
            out->value = cloneNode(n.value);
            return out;
        }
        case NodeType::ObjectLiteral: {
            const auto& n = static_cast<const ObjectLiteral&>(*node);
            auto out = make<ObjectLiteral>(n);
            out->properties = cloneNodeList(n.properties);
            return out;
        }
        case NodeType::StructInstantiation: {
            const auto& n = static_cast<const StructInstantiation&>(*node);
            auto out = make<StructInstantiation>(n);
            out->typeName = n.typeName;
            out->fieldValues = cloneFieldValues(n.fieldValues);
            return out;
        }
        case NodeType::NewExpression: {
            const auto& n = static_cast<const NewExpression&>(*node);
            auto out = make<NewExpression>(n);
            out->typeName = n.typeName;
            out->initializer = cloneNode(n.initializer);
            out->arraySize = cloneNode(n.arraySize);
            out->arguments = cloneNodeList(n.arguments);
            return out;
        }
        case NodeType::DeleteExpression: {
            const auto& n = static_cast<const DeleteExpression&>(*node);
            auto out = make<DeleteExpression>(n);
            out->operand = cloneNode(n.operand);
            return out;
        }
        case NodeType::InlineAsmExpr: {
            const auto& n = static_cast<const InlineAsmExpr&>(*node);
            auto out = make<InlineAsmExpr>(n);
            out->templateString = n.templateString;
            out->constraints = n.constraints;
            out->returnType = n.returnType;
            out->inputs = cloneNodeList(n.inputs);
            out->isBlock = n.isBlock;
            out->rawBody = n.rawBody;
            out->attributes = n.attributes;
            return out;
        }
        case NodeType::Lambda: {
            const auto& n = static_cast<const LambdaExpr&>(*node);
            auto out = make<LambdaExpr>(n);
            out->name = n.name;
            // The lifted function is a top-level declaration owned by the
            // program body and emitted from there exactly once; the lambda only
            // names it. Sharing the pointer keeps that single definition (a
            // clone would be an unregistered duplicate).
            out->function = n.function;
            return out;
        }
        case NodeType::StructDeclaration: {
            const auto& n = static_cast<const StructDeclaration&>(*node);
            auto out = make<StructDeclaration>(n);
            out->name = n.name;
            out->genericParams = n.genericParams;
            out->attributes = n.attributes;
            out->fields = n.fields;
            out->isExported = n.isExported;
            return out;
        }
        case NodeType::EnumDeclaration: {
            const auto& n = static_cast<const EnumDeclaration&>(*node);
            auto out = make<EnumDeclaration>(n);
            out->name = n.name;
            out->underlyingType = n.underlyingType;
            out->variants = n.variants;
            out->isExported = n.isExported;
            return out;
        }
        case NodeType::ClassDeclaration: {
            const auto& n = static_cast<const ClassDeclaration&>(*node);
            auto out = make<ClassDeclaration>(n);
            out->name = n.name;
            out->genericParams = n.genericParams;
            out->attributes = n.attributes;
            out->fields = n.fields;
            out->methods.reserve(n.methods.size());
            for (const auto& m : n.methods) {
                out->methods.push_back(cloneMethod(m));
            }
            out->isExported = n.isExported;
            return out;
        }
        case NodeType::ImplBlock: {
            const auto& n = static_cast<const ImplBlock&>(*node);
            auto out = make<ImplBlock>(n);
            out->typeName = n.typeName;
            out->methods.reserve(n.methods.size());
            for (const auto& m : n.methods) {
                out->methods.push_back(cloneMethod(m));
            }
            return out;
        }
        case NodeType::CompileTimeIf: {
            const auto& n = static_cast<const CompileTimeIfExpr&>(*node);
            auto out = make<CompileTimeIfExpr>(n);
            out->branches = cloneBranches(n.branches);
            return out;
        }
        // Vestigial enum values: no node class declares them (they have no
        // INSTY_NODE_TYPE_OF entry), so no node can report them. Indexing is a
        // MemberAccessExpr with `computed` set, handled above.
        case NodeType::ModuleDeclaration:
        case NodeType::ExpressionStatement:
        case NodeType::IndexExpr:
        case NodeType::TypeReference:
        case NodeType::Unknown:
            break;
    }

    // Every node kind above is handled. Returning the original here would
    // silently reintroduce shared identity (the bug this file exists to fix),
    // so refuse instead: a null makes the omission obvious immediately.
    return nullptr;
}

Method cloneMethod(const Method& method) {
    Method out;
    out.name = method.name;
    out.parameters = method.parameters;
    out.returnType = method.returnType;
    out.hasExplicitReturnType = method.hasExplicitReturnType;
    out.attributes = method.attributes;
    out.isConstructor = method.isConstructor;
    out.isDestructor = method.isDestructor;
    out.isOperator = method.isOperator;
    out.operatorSymbol = method.operatorSymbol;
    out.body = cloneNodeList(method.body);
    return out;
}

}  // namespace AST
