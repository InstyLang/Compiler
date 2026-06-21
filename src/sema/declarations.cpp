#include <sema/checker.hpp>


namespace Sema {

namespace {

const AST::Attribute* findAttr(const std::vector<AST::Attribute>& attrs,
                               const std::string& name) {
    for (const auto& a : attrs) {
        if (a.name == name) {
            return &a;
        }
    }
    return nullptr;
}

}

void Checker::declarePrepass(const std::shared_ptr<AST::ProgramRoot>& program) {
    for (const auto& node : program->body) {
        if (!node) continue;
        switch (node->nodeType()) {
            case AST::NodeType::StructDeclaration:
                if (auto* s = static_cast<AST::StructDeclaration*>(node.get()))
                    types_.registerNamed(s->name, Types::Kind::Struct);
                break;
            case AST::NodeType::EnumDeclaration:
                if (auto* e = static_cast<AST::EnumDeclaration*>(node.get()))
                    types_.registerNamed(e->name, Types::Kind::Enum);
                break;
            case AST::NodeType::ClassDeclaration:
                if (auto* c = static_cast<AST::ClassDeclaration*>(node.get()))
                    types_.registerNamed(c->name, Types::Kind::Class);
                break;
            default:
                break;
        }
    }

    for (const auto& node : program->body) {
        if (!node) continue;
        switch (node->nodeType()) {
            case AST::NodeType::StructDeclaration:
                declareStruct(static_cast<AST::StructDeclaration*>(node.get()));
                break;
            case AST::NodeType::EnumDeclaration:
                declareEnum(static_cast<AST::EnumDeclaration*>(node.get()));
                break;
            case AST::NodeType::ClassDeclaration:
                declareClass(static_cast<AST::ClassDeclaration*>(node.get()));
                break;
            case AST::NodeType::VariableDeclaration:
                declareGlobal(static_cast<AST::VariableDeclarationExpr*>(node.get()));
                break;
            case AST::NodeType::FunctionDeclaration:
                declareFunction(static_cast<AST::FunctionDeclaration*>(node.get()));
                break;
            default:
                break;
        }
    }
}

void Checker::declareStruct(AST::StructDeclaration* node) {
    if (!node) return;
    StructInfo info;
    info.name = node->name;
    if (const auto* a = findAttr(node->attributes, "packed")) {
        info.packed = true;
        (void)a;
    }
    if (const auto* a = findAttr(node->attributes, "align")) {
        try { info.align = std::stoi(a->value); } catch (...) { info.align = 0; }
    }
    for (const auto& f : node->fields) {
        Types::TypeRef ft = resolveTypeSpelling(f.type, node);
        info.fields.emplace_back(f.name, ft);
    }
    result_.structs.push_back(std::move(info));
}

void Checker::declareEnum(AST::EnumDeclaration* node) {
    if (!node) return;
    EnumInfo info;
    info.name = node->name;
    std::string underlying = node->underlyingType.empty() ? "i32" : node->underlyingType;
    info.underlying = resolveTypeSpelling(underlying, node);
    Types::TypeRef enumType = types_.namedType(Types::Kind::Enum, node->name);
    long long next = 0;
    for (const auto& v : node->variants) {
        long long value = v.hasExplicitValue ? v.value : next;
        info.variants[v.name] = value;
        next = value + 1;
        enumConstants_[v.name] = enumType;
    }
    result_.enums.push_back(std::move(info));
}

void Checker::declareClass(AST::ClassDeclaration* node) {
    if (!node) return;

    if (!node->genericParams.empty()) {
        genericClassTemplates_[node->name] = node;
        return;
    }

    StructInfo sinfo;
    sinfo.name = node->name;
    std::vector<std::pair<std::string, Types::TypeRef>> fields;
    for (const auto& f : node->fields) {
        Types::TypeRef ft = resolveTypeSpelling(f.type, node);
        sinfo.fields.emplace_back(f.name, ft);
        fields.emplace_back(f.name, ft);
    }
    result_.structs.push_back(sinfo);
    classFields_[node->name] = fields;

    ClassInfo cinfo;
    cinfo.name = node->name;
    cinfo.fields = fields;

    Types::TypeRef classType = types_.namedType(Types::Kind::Class, node->name);
    Types::TypeRef classPtr = types_.pointerType(classType);

    for (auto& m : node->methods) {
        std::vector<std::string> paramTypeSpellings;
        std::vector<Types::TypeRef> paramTypes;
        std::vector<std::string> paramNames;
        paramTypes.push_back(classPtr);
        paramNames.push_back("this");
        for (const auto& p : m.parameters) {
            paramTypeSpellings.push_back(p.type);
            paramTypes.push_back(resolveTypeSpelling(p.type, node));
            paramNames.push_back(p.name);
        }

        std::string mangled = Sema::mangleClassMember(
            node->name, m.name, m.isConstructor, m.isOperator, m.operatorSymbol,
            paramTypeSpellings);

        FunctionInfo fi;
        fi.name = mangled;
        fi.mangledName = mangled;
        fi.paramTypes = paramTypes;
        fi.paramNames = paramNames;
        fi.returnType = m.isConstructor ? types_.voidType()
                                        : resolveTypeSpelling(m.returnType, node);
        fi.isExternal = false;
        fi.decl = nullptr;
        result_.functions.push_back(fi);

        if (m.isConstructor) {
            cinfo.constructorMangled = mangled;
            cinfo.constructorParams.assign(paramTypes.begin() + 1, paramTypes.end());
        } else if (m.isOperator) {
            cinfo.operatorMangled[m.operatorSymbol] = mangled;
        } else {
            cinfo.methodNames.push_back(m.name);
            cinfo.methodMangled[m.name] = mangled;
        }

        pendingMethods_.push_back({node->name, classPtr, &m});
    }

    result_.classes.push_back(std::move(cinfo));
}

void Checker::checkClassMethod(const std::string& className, Types::TypeRef classPtr,
                               AST::Method& method) {
    Types::TypeRef savedThis = currentThis_;
    std::string savedClass = currentClass_;
    const FunctionInfo* savedFn = currentFn_;
    Types::TypeRef savedReturn = currentReturn_;
    bool savedUnsafe = inUnsafe_;

    currentThis_ = classPtr;
    currentClass_ = className;
    currentReturn_ = method.isConstructor
                         ? types_.voidType()
                         : resolveTypeSpelling(method.returnType, nullptr);
    inUnsafe_ = false;

    pushScope();
    declareLocal("this", classPtr, nullptr);
    for (const auto& p : method.parameters) {
        declareLocal(p.name, resolveTypeSpelling(p.type, nullptr), nullptr);
    }
    checkBlock(method.body);
    popScope();

    currentThis_ = savedThis;
    currentClass_ = savedClass;
    currentFn_ = savedFn;
    currentReturn_ = savedReturn;
    inUnsafe_ = savedUnsafe;
}

void Checker::declareGlobal(AST::VariableDeclarationExpr* node) {
    if (!node) return;
    GlobalInfo info;
    info.name = node->identifier;
    info.isConst = node->isConst;
    if (!node->typeHint.empty()) {
        info.type = resolveTypeSpelling(node->typeHint, node);
        if (node->isArray && info.type && !info.type->isError() &&
            info.type->kind != Types::Kind::Array &&
            info.type->kind != Types::Kind::Slice) {
            info.type = types_.arrayType(info.type, node->arraySize);
        }
    } else {
        info.type = types_.errorType();
    }
    result_.globals.push_back(std::move(info));
}

std::string Checker::computeMangledName(AST::FunctionDeclaration* node, bool& outIsExtern) {
    outIsExtern = false;
    const std::string& fnName = node->name;

    if (fnName == "main") {
        return "main";
    }

    if (const auto* a = findAttr(node->attributes, "name")) {
        if (!a->value.empty()) {
            return a->value;
        }
    }

    // The `extern` keyword declares an external C/ABI symbol: never mangle it,
    // so calls resolve to the raw symbol (e.g. libc `malloc`) at link time.
    if (node->isExtern) {
        outIsExtern = true;
        return fnName;
    }

    if (const auto* a = findAttr(node->attributes, "mangle")) {
        if (a->value == "off") {
            return fnName;
        }
    }

    if (const auto* a = findAttr(node->attributes, "extern")) {
        outIsExtern = true;
        if (a->value == "C" || a->value.empty()) {
            return fnName;
        }
        return fnName;
    }

    return Sema::mangleFunction(result_.moduleName, fnName);
}

void Checker::declareFunction(AST::FunctionDeclaration* node) {
    if (!node) return;

    if (!node->genericParams.empty()) {
        genericTemplates_[node->name] = node;
        return;
    }

    FunctionInfo info;
    info.name = node->name;
    info.decl = node;

    bool isExtern = false;
    info.mangledName = computeMangledName(node, isExtern);
    info.isExternal = isExtern || !node->hasBody;

    if (const auto* a = findAttr(node->attributes, "unsafe")) {
        info.isUnsafe = (a->value == "on" || a->value.empty());
    }

    for (const auto& p : node->parameters) {
        info.paramNames.push_back(p.name);
        Types::TypeRef pt = resolveTypeSpelling(p.type, node);
        if (p.isVolatile && pt && pt->kind == Types::Kind::Pointer) {
            pt = types_.pointerType(pt->element, true);
        }
        info.paramTypes.push_back(pt);
    }

    if (node->returnType.empty()) {
        info.returnType = types_.voidType();
    } else {
        info.returnType = resolveTypeSpelling(node->returnType, node);
    }

    result_.functions.push_back(std::move(info));
}

}
