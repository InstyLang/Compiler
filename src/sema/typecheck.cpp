#include <sema/checker.hpp>

#include <cctype>
#include <memory>
#include <set>

#include <extra/ast_clone.hpp>
#include <extra/builtins.hpp>


namespace Sema {

static bool isScopePath(const AST::ExprAST* node) {
    if (!node) return false;
    if (node->nodeType() == AST::NodeType::IdentifierExpr) return true;
    if (node->nodeType() == AST::NodeType::MemberAccess) {
        auto* m = static_cast<const AST::MemberAccessExpr*>(node);
        return m->isScope && !m->computed && m->object &&
               isScopePath(m->object.get());
    }
    return false;
}


void Checker::pushScope() { scopes_.emplace_back(); }

void Checker::popScope() {
    if (!scopes_.empty()) scopes_.pop_back();
}

bool Checker::declareLocal(const std::string& name, Types::TypeRef type,
                           const AST::ExprAST* at) {
    if (scopes_.empty()) pushScope();
    auto& cur = scopes_.back().vars;
    if (cur.find(name) != cur.end()) {
        emit("E2003", "duplicate declaration of '" + name + "'", at,
             "a binding with this name already exists in this scope");
        return false;
    }
    cur[name] = type ? type : types_.errorType();
    return true;
}

Types::TypeRef Checker::lookupLocal(const std::string& name) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto found = it->vars.find(name);
        if (found != it->vars.end()) return found->second;
    }
    return nullptr;
}


Types::TypeRef Checker::resolveTypeSpelling(const std::string& spelling,
                                            const AST::ExprAST* at) {
    if (spelling.empty()) {
        return types_.voidType();
    }

    {
        size_t base = 0;
        while (base < spelling.size() &&
               (std::isalnum(static_cast<unsigned char>(spelling[base])) ||
                spelling[base] == '_' || spelling[base] == '.')) {
            ++base;
        }
        std::string head = spelling.substr(0, base);
        size_t dot = head.rfind('.');
        if (dot != std::string::npos) {
            std::string stripped = head.substr(dot + 1) + spelling.substr(base);
            return resolveTypeSpelling(stripped, at);
        }
    }

    {
        size_t angle = spelling.find('<');
        if (angle != std::string::npos && !spelling.empty() &&
            spelling.back() == '>') {
            std::string base = spelling.substr(0, angle);
            while (!base.empty() && base.back() == ' ') base.pop_back();
            while (!base.empty() && base.front() == ' ') base.erase(base.begin());
            if (genericClassTemplates_.find(base) != genericClassTemplates_.end()) {
                std::string inner =
                    spelling.substr(angle + 1, spelling.size() - angle - 2);
                std::vector<std::string> args;
                int depth = 0;
                std::string cur;
                for (char ch : inner) {
                    if (ch == '<') { ++depth; cur.push_back(ch); }
                    else if (ch == '>') { --depth; cur.push_back(ch); }
                    else if (ch == ',' && depth == 0) {
                        args.push_back(cur);
                        cur.clear();
                    } else {
                        cur.push_back(ch);
                    }
                }
                if (!cur.empty()) args.push_back(cur);
                for (auto& a : args) {
                    while (!a.empty() && a.front() == ' ') a.erase(a.begin());
                    while (!a.empty() && a.back() == ' ') a.pop_back();
                    for (const auto& [g, _] : currentSubst_) {
                        Types::TypeRef gt = currentSubst_[g];
                        if (gt) a = substituteSpelling(a, g, types_.toString(gt));
                    }
                }
                std::string mangled = instantiateGenericClass(base, args, at);
                if (!mangled.empty()) {
                    return types_.namedType(Types::Kind::Class, mangled);
                }
                return types_.errorType();
            }
        }
    }

    // Substitute active generic parameters that appear as the leading token of a
    // compound spelling (e.g. `T[]`, `T*`, `T[][]`) before interning, so a generic
    // body's local/temporary types monomorphize like its params and fields do. A
    // bare `T` is also covered here (and again below as a safety net).
    std::string resolved = spelling;
    if (!currentSubst_.empty()) {
        for (const auto& entry : currentSubst_) {
            if (entry.second) {
                resolved = substituteSpelling(resolved, entry.first,
                                              types_.toString(entry.second));
            }
        }
    }

    Types::TypeRef t = types_.fromString(resolved);
    if (t && t->kind == Types::Kind::Generic && !currentSubst_.empty()) {
        auto it = currentSubst_.find(t->name);
        if (it != currentSubst_.end() && it->second) {
            return it->second;
        }
    }
    if (t && t->isError()) {
        emit("E2001", "unknown type '" + spelling + "'", at,
             "did you forget to declare or import this type?");
        return types_.errorType();
    }
    return t ? t : types_.errorType();
}

std::string Checker::substituteSpelling(const std::string& spelling,
                                        const std::string& paramName,
                                        const std::string& concreteSpelling) {
    std::string prefix;
    std::string s = spelling;
    const std::string vol = "volatile ";
    if (s.compare(0, vol.size(), vol) == 0) {
        prefix = vol;
        s = s.substr(vol.size());
    }
    while (!s.empty() && s.front() == ' ') { prefix += ' '; s.erase(s.begin()); }

    size_t i = 0;
    while (i < s.size() &&
           (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_')) {
        ++i;
    }
    std::string token = s.substr(0, i);
    if (token != paramName) {
        return spelling;
    }
    std::string suffix = s.substr(i);
    return prefix + concreteSpelling + suffix;
}


Types::TypeRef Checker::enumUnderlying(Types::TypeRef t) const {
    if (!t || t->kind != Types::Kind::Enum) return t;
    for (const auto& e : result_.enums) {
        if (e.name == t->name && e.underlying) return e.underlying;
    }
    return t;
}

const FunctionInfo* Checker::findFunctionByMangled(const std::string& mangled) const {
    for (const auto& fn : result_.functions) {
        const std::string& symbol = fn.mangledName.empty() ? fn.name : fn.mangledName;
        if (symbol == mangled) {
            return &fn;
        }
    }
    for (const auto& fn : importedStore_) {
        const std::string& symbol = fn.mangledName.empty() ? fn.name : fn.mangledName;
        if (symbol == mangled) {
            return &fn;
        }
    }
    return nullptr;
}

bool Checker::isAssignable(Types::TypeRef target, Types::TypeRef value,
                           bool valueIsLiteral) {
    if (!target || !value) return true;
    if (target->isError() || value->isError()) return true;
    if (Types::TypeContext::equals(target, value)) return true;

    Types::TypeRef t = enumUnderlying(target);
    Types::TypeRef v = enumUnderlying(value);
    if (t != target || v != value) {
        if (Types::TypeContext::equals(t, v)) return true;
        target = t;
        value = v;
    }

    if (target->isInteger() && value->isInteger()) {
        if (valueIsLiteral) return true;
        if (target->isSigned == value->isSigned &&
            target->bitWidth >= value->bitWidth) {
            return true;
        }
        return false;
    }
    if (target->isFloat() && value->isInteger() && valueIsLiteral) return true;
    if (target->isFloat() && value->isFloat()) {
        return target->bitWidth >= value->bitWidth;
    }
    if (target->isPointerLike() && value->isInteger() && valueIsLiteral) return true;
    return false;
}

bool Checker::isSliceInitializer(Types::TypeRef target, Types::TypeRef value,
                                 const AST::NodePtr& valueNode) {
    if (!target || !value || target->kind != Types::Kind::Slice) return false;
    if (target->isError() || value->isError()) return true;
    if (value->kind == Types::Kind::Slice) {
        return Types::TypeContext::equals(target->element, value->element);
    }
    if (value->kind == Types::Kind::Array) {
        return Types::TypeContext::equals(target->element, value->element);
    }
    // Raw pointers do not carry a length. The only pointer expression accepted as
    // slice sugar is `new T[n]`, where codegen can reuse the allocation count for
    // the slice header. Other pointers must be exposed explicitly as `.ptr` later.
    if (value->kind == Types::Kind::Pointer && valueNode &&
        valueNode->nodeType() == AST::NodeType::NewExpression) {
        return Types::TypeContext::equals(target->element, value->element);
    }
    return false;
}

Types::TypeRef Checker::arithResult(Types::TypeRef a, Types::TypeRef b) {
    if (!a || !b) return types_.errorType();
    if (a->isError() || b->isError()) return types_.errorType();
    if (a->isFloat() || b->isFloat()) {
        int w = 64;
        if (a->isFloat() && b->isFloat()) w = a->bitWidth > b->bitWidth ? a->bitWidth : b->bitWidth;
        else if (a->isFloat()) w = a->bitWidth;
        else w = b->bitWidth;
        return types_.floatType(w);
    }
    if (a->isInteger() && b->isInteger()) {
        int w = a->bitWidth > b->bitWidth ? a->bitWidth : b->bitWidth;
        return types_.intType(w, a->isSigned);
    }
    return types_.errorType();
}


bool Checker::isLValue(const AST::NodePtr& node) {
    if (!node) return false;
    switch (node->nodeType()) {
        case AST::NodeType::IdentifierExpr:
        case AST::NodeType::DereferenceExpr:
        case AST::NodeType::MemberAccess:
            return true;
        default:
            return false;
    }
}


void Checker::checkFunction(const FunctionInfo& info) {
    currentFn_ = &info;
    currentReturn_ = info.returnType ? info.returnType : types_.voidType();
    inUnsafe_ = info.isUnsafe;

    pushScope();
    for (size_t i = 0; i < info.paramNames.size(); ++i) {
        Types::TypeRef pt = i < info.paramTypes.size() ? info.paramTypes[i]
                                                       : types_.errorType();
        declareLocal(info.paramNames[i], pt, info.decl);
    }

    AST::FunctionDeclaration* decl = info.decl;
    if (decl) {
        checkBlock(decl->body);

        bool naked = false;
        for (const auto& a : decl->attributes) {
            if (a.name == "naked" && (a.value == "on" || a.value.empty())) naked = true;
        }
        if (decl->hasBody && !naked && currentReturn_ && !currentReturn_->isVoid() &&
            !currentReturn_->isError()) {
            if (!blockReturns(decl->body)) {
                emit("E2011", "missing return in non-void function '" + info.name + "'",
                     decl, "every path must return a value of type " +
                               types_.toString(currentReturn_));
            }
        }
    }

    popScope();
    inUnsafe_ = false;
    currentFn_ = nullptr;
    currentReturn_ = nullptr;
}

void Checker::checkInstantiationBody(const GenericInstantiation& inst) {
    if (!inst.templateDecl) return;
    // Check this instantiation's own copy of the body, so the types recorded per
    // node describe this instantiation rather than whichever one happened to be
    // checked last. Falls back to the template only if no copy was made.
    AST::FunctionDeclaration* decl =
        inst.decl ? inst.decl.get() : inst.templateDecl;

    std::map<std::string, Types::TypeRef> savedSubst = currentSubst_;
    const FunctionInfo* savedFn = currentFn_;
    Types::TypeRef savedReturn = currentReturn_;
    bool savedUnsafe = inUnsafe_;

    currentSubst_.clear();
    for (size_t i = 0; i < decl->genericParams.size() && i < inst.typeArgs.size();
         ++i) {
        currentSubst_[decl->genericParams[i]] =
            resolveTypeSpelling(inst.typeArgs[i], decl);
    }

    currentFn_ = nullptr;
    currentReturn_ = inst.returnType ? inst.returnType : types_.voidType();
    inUnsafe_ = false;
    for (const auto& a : decl->attributes) {
        if (a.name == "unsafe" && (a.value == "on" || a.value.empty())) {
            inUnsafe_ = true;
        }
    }

    pushScope();
    for (size_t i = 0; i < inst.paramNames.size(); ++i) {
        Types::TypeRef pt = i < inst.paramTypes.size() ? inst.paramTypes[i]
                                                       : types_.errorType();
        declareLocal(inst.paramNames[i], pt, decl);
    }
    checkBlock(decl->body);
    popScope();

    currentSubst_ = savedSubst;
    currentFn_ = savedFn;
    currentReturn_ = savedReturn;
    inUnsafe_ = savedUnsafe;
}

std::string Checker::instantiateGenericClass(const std::string& templateName,
                                             const std::vector<std::string>& typeArgs,
                                             const AST::ExprAST* at) {
    auto tIt = genericClassTemplates_.find(templateName);
    if (tIt == genericClassTemplates_.end()) {
        return "";
    }
    AST::ClassDeclaration* tmpl = tIt->second;

    if (typeArgs.size() != tmpl->genericParams.size()) {
        emit("E2008",
             "wrong number of type arguments to generic class '" + templateName +
                 "': expected " + std::to_string(tmpl->genericParams.size()) +
                 ", got " + std::to_string(typeArgs.size()),
             at, "");
        return "";
    }

    std::string mangled = Sema::mangleGenericInstance(templateName, typeArgs);

    if (instantiatedGenericClasses_.find(mangled) !=
        instantiatedGenericClasses_.end()) {
        return mangled;
    }
    instantiatedGenericClasses_[mangled] = true;

    std::map<std::string, Types::TypeRef> subst;
    std::map<std::string, std::string> substSpelling;
    for (size_t i = 0; i < tmpl->genericParams.size(); ++i) {
        subst[tmpl->genericParams[i]] = resolveTypeSpelling(typeArgs[i], at);
        substSpelling[tmpl->genericParams[i]] = typeArgs[i];
    }

    auto substituteAll = [&](const std::string& spelling) -> std::string {
        std::string out = spelling;
        for (const auto& g : tmpl->genericParams) {
            out = substituteSpelling(out, g, substSpelling[g]);
        }
        return out;
    };

    types_.registerNamed(mangled, Types::Kind::Class);

    std::map<std::string, Types::TypeRef> savedSubst = currentSubst_;
    currentSubst_ = subst;

    StructInfo sinfo;
    sinfo.name = mangled;
    std::vector<std::pair<std::string, Types::TypeRef>> fields;
    for (const auto& f : tmpl->fields) {
        Types::TypeRef ft = resolveTypeSpelling(substituteAll(f.type), at);
        sinfo.fields.emplace_back(f.name, ft);
        fields.emplace_back(f.name, ft);
    }
    result_.structs.push_back(sinfo);
    classFields_[mangled] = fields;

    ClassInfo cinfo;
    cinfo.name = mangled;
    cinfo.fields = fields;

    Types::TypeRef classType = types_.namedType(Types::Kind::Class, mangled);
    Types::TypeRef classPtr = types_.pointerType(classType);

    // One deep copy of every method per instantiation. The copies are spelled
    // exactly like the template (still in terms of `T`); only their node
    // identity differs. That is what keeps each instantiation's recorded types
    // separate -- see AST::cloneNode and GenericClassInstantiation::methods.
    std::vector<std::shared_ptr<AST::Method>> instanceMethods;
    instanceMethods.reserve(tmpl->methods.size());
    for (const auto& templateMethod : tmpl->methods) {
        instanceMethods.push_back(
            std::make_shared<AST::Method>(AST::cloneMethod(templateMethod)));
    }

    for (const auto& methodOwner : instanceMethods) {
        AST::Method& m = *methodOwner;
        if (m.isDestructor) {
            if (!m.parameters.empty()) {
                emit("E1501", "destructor for class '" + mangled + "' cannot take parameters",
                     at, "declare it exactly as `destructor() -> void`");
            }
            if (!m.hasExplicitReturnType) {
                emit("E1501", "destructor for class '" + mangled +
                               "' must explicitly return void",
                     at, "declare it exactly as `destructor() -> void`");
            } else {
                Types::TypeRef dtorRet = resolveTypeSpelling(substituteAll(m.returnType), at);
                if (!dtorRet || !dtorRet->isVoid()) {
                    emit("E1501", "destructor for class '" + mangled +
                                   "' must return void",
                         at, "declare it exactly as `destructor() -> void`");
                }
            }
        }

        std::vector<std::string> paramTypeSpellings;
        std::vector<Types::TypeRef> paramTypes;
        std::vector<std::string> paramNames;
        paramTypes.push_back(classPtr);
        paramNames.push_back("this");
        for (const auto& p : m.parameters) {
            std::string subSpelling = substituteAll(p.type);
            paramTypeSpellings.push_back(subSpelling);
            paramTypes.push_back(resolveTypeSpelling(subSpelling, at));
            paramNames.push_back(p.name);
        }

        std::string memberMangled = Sema::mangleClassMember(
            mangled, m.name, m.isConstructor, m.isOperator, m.operatorSymbol,
            paramTypeSpellings);

        FunctionInfo fi;
        fi.name = memberMangled;
        fi.mangledName = memberMangled;
        fi.paramTypes = paramTypes;
        fi.paramNames = paramNames;
        fi.returnType = (m.isConstructor || m.isDestructor)
                            ? types_.voidType()
                            : resolveTypeSpelling(substituteAll(m.returnType), at);
        fi.isExternal = false;
        fi.isGenericInstance = true;
        fi.decl = nullptr;
        result_.functions.push_back(fi);

        if (m.isConstructor) {
            cinfo.constructorMangled = memberMangled;
            cinfo.constructorParams.assign(paramTypes.begin() + 1, paramTypes.end());
        } else if (m.isDestructor) {
            cinfo.destructorMangled = memberMangled;
        } else if (m.isOperator) {
            cinfo.operatorMangled[m.operatorSymbol] = memberMangled;
        } else {
            cinfo.methodNames.push_back(m.name);
            cinfo.methodMangled[m.name] = memberMangled;
        }

        pendingGenericMethods_.push_back({mangled, classPtr, &m, subst});
    }

    currentSubst_ = savedSubst;

    result_.classes.push_back(std::move(cinfo));

    GenericClassInstantiation gci;
    gci.mangledName = mangled;
    gci.templateDecl = tmpl;
    gci.typeArgs = typeArgs;
    gci.methods = std::move(instanceMethods);
    result_.genericClassInstantiations.push_back(std::move(gci));

    return mangled;
}

void Checker::checkGenericClassMethod(const PendingGenericMethod& pm) {
    if (!pm.method) return;
    AST::Method& method = *pm.method;

    std::map<std::string, Types::TypeRef> savedSubst = currentSubst_;
    Types::TypeRef savedThis = currentThis_;
    std::string savedClass = currentClass_;
    const FunctionInfo* savedFn = currentFn_;
    Types::TypeRef savedReturn = currentReturn_;
    bool savedUnsafe = inUnsafe_;

    currentSubst_ = pm.subst;
    currentThis_ = pm.classPtr;
    currentClass_ = pm.className;
    currentReturn_ = (method.isConstructor || method.isDestructor)
                         ? types_.voidType()
                         : resolveTypeSpelling(method.returnType, nullptr);
    currentFn_ = nullptr;
    inUnsafe_ = false;

    pushScope();
    declareLocal("this", pm.classPtr, nullptr);
    for (const auto& p : method.parameters) {
        declareLocal(p.name, resolveTypeSpelling(p.type, nullptr), nullptr);
    }
    checkBlock(method.body);
    popScope();

    currentSubst_ = savedSubst;
    currentThis_ = savedThis;
    currentClass_ = savedClass;
    currentFn_ = savedFn;
    currentReturn_ = savedReturn;
    inUnsafe_ = savedUnsafe;
}

void Checker::checkBlock(const AST::NodeList& body) {
    for (const auto& stmt : body) {
        checkStatement(stmt);
    }
}

void Checker::checkStatement(const AST::NodePtr& node) {
    if (!node) return;
    switch (node->nodeType()) {
        case AST::NodeType::VariableDeclaration:
            checkVarDecl(static_cast<AST::VariableDeclarationExpr*>(node.get()));
            break;
        case AST::NodeType::AssignmentExpr:
            checkAssignment(static_cast<AST::AssignmentExpr*>(node.get()));
            break;
        case AST::NodeType::IfStatement:
            checkIf(static_cast<AST::IfStatement*>(node.get()));
            break;
        case AST::NodeType::WhileLoop:
            checkWhile(static_cast<AST::WhileLoop*>(node.get()));
            break;
        case AST::NodeType::InfiniteLoop:
            checkLoop(static_cast<AST::InfiniteLoop*>(node.get()));
            break;
        case AST::NodeType::ForLoop:
            checkFor(static_cast<AST::ForLoop*>(node.get()));
            break;
        case AST::NodeType::WhenStatement:
            checkWhen(static_cast<AST::WhenStatement*>(node.get()));
            break;
        case AST::NodeType::SwitchStatement:
            checkSwitch(static_cast<AST::SwitchStatement*>(node.get()));
            break;
        case AST::NodeType::ReturnStatement:
            checkReturn(static_cast<AST::ReturnStatement*>(node.get()));
            break;
        case AST::NodeType::UnsafeBlock:
            checkUnsafe(static_cast<AST::UnsafeBlock*>(node.get()));
            break;
        case AST::NodeType::BreakStatement:
        case AST::NodeType::SkipStatement:
            break;
        default:
            checkExpr(node);
            break;
    }
}


void Checker::checkVarDecl(AST::VariableDeclarationExpr* node) {
    if (!node) return;
    // `auto` requests type inference from the initializer (like C++). Leave
    // `declared` null so the initializer's type becomes the variable's type.
    const bool isAuto = node->typeHint == "auto";
    Types::TypeRef declared = nullptr;
    if (!node->typeHint.empty() && !isAuto) {
        declared = resolveTypeSpelling(node->typeHint, node);
        if (node->isArray && declared && !declared->isError() &&
            declared->kind != Types::Kind::Array &&
            declared->kind != Types::Kind::Slice) {
            declared = types_.arrayType(declared, node->arraySize);
        }
    }

    Types::TypeRef initType = nullptr;
    bool initLiteral = false;
    if (node->initialValue) {
        initType = checkExpr(node->initialValue);
        initLiteral = isIntLiteral(node->initialValue);
    }
    for (const auto& arg : node->constructorArgs) {
        checkExpr(arg);
    }

    if (isAuto && !node->initialValue) {
        emit("E2006", "'auto' variable '" + node->identifier +
                          "' requires an initializer to infer its type",
             node, "write `auto name = <value>`");
    }

    Types::TypeRef finalType = declared;
    if (!finalType) {
        finalType = initType ? initType : types_.errorType();
    }

    if (declared && initType && !declared->isError() && !initType->isError()) {
        if (!isAssignable(declared, initType, initLiteral) &&
            !isSliceInitializer(declared, initType, node->initialValue)) {
            emit("E2005", "cannot initialize '" + node->identifier + "' of type " +
                              types_.toString(declared) + " with value of type " +
                              types_.toString(initType),
                 node, "types must match (only implicit numeric widening is allowed)");
        }
    }

    declareLocal(node->identifier, finalType, node);
    record(node, finalType);
}

void Checker::checkAssignment(AST::AssignmentExpr* node) {
    if (!node) return;
    Types::TypeRef targetType = node->target ? checkExpr(node->target) : nullptr;
    Types::TypeRef valueType = node->value ? checkExpr(node->value) : nullptr;

    if (!isLValue(node->target)) {
        emit("E2004", "assignment target is not assignable", node,
             "the left-hand side must be a variable, dereference, index, or member");
        return;
    }
    bool valLiteral = isIntLiteral(node->value);
    if (targetType && valueType && !targetType->isError() && !valueType->isError()) {
        if (!isAssignable(targetType, valueType, valLiteral) &&
            !isSliceInitializer(targetType, valueType, node->value)) {
            emit("E2004", "cannot assign value of type " + types_.toString(valueType) +
                              " to target of type " + types_.toString(targetType),
                 node, "types must match (only implicit numeric widening is allowed)");
        }
    }
}

void Checker::checkIf(AST::IfStatement* node) {
    if (!node) return;
    if (node->condition) {
        Types::TypeRef c = checkExpr(node->condition);
        if (c && !c->isError() && !(c->isInteger() || c->kind == Types::Kind::Bool)) {
            emit("E2012", "condition must be bool or integer", node->condition.get(),
                 "got " + types_.toString(c));
        }
    }
    pushScope();
    checkBlock(node->consequent);
    popScope();
    pushScope();
    checkBlock(node->alternate);
    popScope();
}

void Checker::checkWhile(AST::WhileLoop* node) {
    if (!node) return;
    if (node->condition) {
        Types::TypeRef c = checkExpr(node->condition);
        if (c && !c->isError() && !(c->isInteger() || c->kind == Types::Kind::Bool)) {
            emit("E2012", "condition must be bool or integer", node->condition.get(),
                 "got " + types_.toString(c));
        }
    }
    pushScope();
    checkBlock(node->body);
    popScope();
}

void Checker::checkLoop(AST::InfiniteLoop* node) {
    if (!node) return;
    pushScope();
    checkBlock(node->body);
    popScope();
}

void Checker::checkFor(AST::ForLoop* node) {
    if (!node) return;
    pushScope();

    Types::TypeRef varType = types_.errorType();
    if (node->isRange) {
        Types::TypeRef ts = node->rangeStart ? checkExpr(node->rangeStart)
                                             : types_.errorType();
        Types::TypeRef te = node->rangeEnd ? checkExpr(node->rangeEnd)
                                           : types_.errorType();
        auto requireInt = [&](Types::TypeRef t, const AST::NodePtr& at) {
            if (t && !t->isError() && !t->isInteger()) {
                emit("E2016", "for-range bound must be an integer", at.get(),
                     "got " + types_.toString(t));
            }
        };
        requireInt(ts, node->rangeStart);
        requireInt(te, node->rangeEnd);
        // The loop variable spans both bounds: use their unified integer type
        // (falling back to i64) so `for i in 0..xs.len` gives `i` an i64.
        if (ts && te && ts->isInteger() && te->isInteger()) {
            varType = arithResult(ts, te);
        } else {
            varType = types_.intType(64, true);
        }
    } else {
        Types::TypeRef it = node->iterable ? checkExpr(node->iterable)
                                           : types_.errorType();
        if (it && !it->isError()) {
            if (it->kind == Types::Kind::Slice || it->kind == Types::Kind::Array) {
                varType = it->element ? it->element : types_.errorType();
            } else if (it->kind == Types::Kind::Text) {
                varType = types_.intType(8, false);  // bytes
            } else {
                emit("E2016", "cannot iterate a value of type " + types_.toString(it),
                     node->iterable.get(),
                     "for-in iterates a slice, fixed array, or text");
            }
        }
    }

    // Record the loop-variable type on the node so the backend can reproduce it
    // exactly (width/signedness for the range counter, element type otherwise).
    record(node, varType);
    declareLocal(node->varName, varType, node);
    checkBlock(node->body);
    popScope();
}

void Checker::checkWhen(AST::WhenStatement* node) {
    if (!node) return;
    if (node->condition) checkExpr(node->condition);
    pushScope();
    checkBlock(node->consequent);
    popScope();
}

const SumTypeInfo* Checker::sumTypeByName(const std::string& name) const {
    for (const auto& st : result_.sumTypes) {
        if (st.name == name) return &st;
    }
    return nullptr;
}

const SumTypeInfo* Checker::asSumVariantAccess(const AST::MemberAccessExpr* m,
                                               const SumVariant** outVariant) const {
    if (outVariant) *outVariant = nullptr;
    if (!m || m->computed) return nullptr;
    // Resolve a possibly-scoped type name (e.g. `module::Type` or `Type`).
    std::string typeName;
    {
        const AST::ExprAST* obj = m->object.get();
        if (!obj) return nullptr;
        if (obj->nodeType() == AST::NodeType::IdentifierExpr) {
            typeName = static_cast<const AST::IdentifierExpr*>(obj)->name;
        } else if (obj->nodeType() == AST::NodeType::MemberAccess) {
            auto* inner = static_cast<const AST::MemberAccessExpr*>(obj);
            if (inner->isScope && inner->property &&
                inner->property->nodeType() == AST::NodeType::IdentifierExpr) {
                typeName = static_cast<const AST::IdentifierExpr*>(inner->property.get())->name;
            }
        }
        if (typeName.empty()) return nullptr;
    }
    const SumTypeInfo* st = sumTypeByName(typeName);
    if (!st) return nullptr;
    if (!m->property || m->property->nodeType() != AST::NodeType::IdentifierExpr)
        return st;
    const std::string& variantName =
        static_cast<const AST::IdentifierExpr&>(*m->property).name;
    if (outVariant) {
        for (const auto& v : st->variants) {
            if (v.name == variantName) { *outVariant = &v; break; }
        }
    }
    return st;
}

void Checker::checkSwitch(AST::SwitchStatement* node) {
    if (!node) return;
    Types::TypeRef subjT = node->subject ? checkExpr(node->subject) : nullptr;
    const SumTypeInfo* st =
        (subjT && !subjT->isError()) ? sumTypeByName(subjT->name) : nullptr;
    if (subjT && !subjT->isError() && !st) {
        emit("E2018", "switch requires a tagged-union value, got " +
                          types_.toString(subjT),
             node->subject.get(), "switch works on sum-type enums");
    }

    bool hasDefault = false;
    std::set<std::string> covered;
    for (auto& arm : node->arms) {
        pushScope();
        if (arm.isDefault) {
            hasDefault = true;
        } else if (st) {
            const SumVariant* v = nullptr;
            for (const auto& cand : st->variants) {
                if (cand.name == arm.variant) { v = &cand; break; }
            }
            if (!v) {
                emit("E2018", "'" + arm.variant + "' is not a variant of '" +
                                  st->name + "'",
                     node, "");
            } else {
                covered.insert(arm.variant);
                if (arm.bindings.size() != v->payload.size()) {
                    emit("E2018", "variant '" + arm.variant + "' binds " +
                                      std::to_string(v->payload.size()) +
                                      " field(s), got " +
                                      std::to_string(arm.bindings.size()),
                         node, "switch `Variant(a, b)` must bind each payload field");
                } else {
                    for (size_t i = 0; i < arm.bindings.size(); ++i) {
                        declareLocal(arm.bindings[i], v->payload[i], node);
                    }
                }
            }
        }
        checkBlock(arm.body);
        popScope();
    }

    if (!hasDefault && st) {
        std::string missing;
        for (const auto& v : st->variants) {
            if (!covered.count(v.name)) {
                if (!missing.empty()) missing += ", ";
                missing += v.name;
            }
        }
        if (!missing.empty()) {
            emit("E2014", "non-exhaustive match on '" + st->name +
                              "': missing " + missing,
                 node, "cover every variant, or add a `_ => ...` default arm");
        }
    }
}

void Checker::checkReturn(AST::ReturnStatement* node) {
    if (!node) return;
    // Auto-return inference (lambda bodies): unify the types of all returned
    // values. A concrete type wins over one derived from an integer literal, and
    // two integers unify to the wider width, so e.g. `return 0` followed by
    // `return some_i64_call()` infers i64.
    if (inferReturn_) {
        Types::TypeRef vt = node->returnValue ? checkExpr(node->returnValue)
                                              : types_.voidType();
        const bool lit = node->returnValue && isIntLiteral(node->returnValue);
        if (!inferredReturn_) {
            inferredReturn_ = vt;
            inferredFromLiteral_ = lit;
        } else if (!Types::TypeContext::equals(inferredReturn_, vt) && vt &&
                   !vt->isError() && !inferredReturn_->isError()) {
            const bool bothInt = inferredReturn_->kind == Types::Kind::Int &&
                                 vt->kind == Types::Kind::Int;
            if (inferredFromLiteral_ && !lit) {
                inferredReturn_ = vt;        // concrete type supersedes a literal
                inferredFromLiteral_ = false;
            } else if (bothInt && (!lit || !inferredFromLiteral_)) {
                if (vt->bitWidth > inferredReturn_->bitWidth) inferredReturn_ = vt;
            }
        }
        return;
    }
    Types::TypeRef ret = currentReturn_ ? currentReturn_ : types_.voidType();
    if (!node->returnValue) {
        if (!ret->isVoid() && !ret->isError()) {
            emit("E2010", "return without a value in non-void function", node,
                 "expected a value of type " + types_.toString(ret));
        }
        return;
    }
    Types::TypeRef vt = checkExpr(node->returnValue);
    bool lit = isIntLiteral(node->returnValue);
    if (ret->isVoid()) {
        emit("E2010", "returning a value from a void function", node,
             "remove the return value or change the return type");
        return;
    }
    if (vt && ret && !vt->isError() && !ret->isError() &&
        !isAssignable(ret, vt, lit) &&
        !isSliceInitializer(ret, vt, node->returnValue)) {
        emit("E2010", "return type mismatch: expected " + types_.toString(ret) +
                          ", got " + types_.toString(vt),
             node, "the value must be assignable to the declared return type");
    }
}

void Checker::checkUnsafe(AST::UnsafeBlock* node) {
    if (!node) return;
    bool prev = inUnsafe_;
    inUnsafe_ = true;
    pushScope();
    checkBlock(node->body);
    popScope();
    inUnsafe_ = prev;
}


bool Checker::blockReturns(const AST::NodeList& body) {
    if (body.empty()) return false;
    const AST::NodePtr& last = body.back();
    if (!last) return false;
    switch (last->nodeType()) {
        case AST::NodeType::ReturnStatement:
            return true;
        case AST::NodeType::IfStatement: {
            auto* iff = static_cast<AST::IfStatement*>(last.get());
            if (iff->alternate.empty()) return false;
            return blockReturns(iff->consequent) && blockReturns(iff->alternate);
        }
        case AST::NodeType::InfiniteLoop: {
            auto* lp = static_cast<AST::InfiniteLoop*>(last.get());
            for (const auto& s : lp->body) {
                if (s && s->nodeType() == AST::NodeType::BreakStatement) return false;
            }
            return true;
        }
        case AST::NodeType::UnsafeBlock: {
            auto* ub = static_cast<AST::UnsafeBlock*>(last.get());
            return blockReturns(ub->body);
        }
        default:
            return false;
    }
}


Types::TypeRef Checker::checkExpr(const AST::NodePtr& node) {
    if (!node) return types_.errorType();
    AST::ExprAST* raw = node.get();
    switch (node->nodeType()) {
        case AST::NodeType::IntegerLiteral: {
            // Type the literal by its magnitude so it isn't silently narrowed:
            // i32 by default (preserving the common case), widening to i64 / u64
            // / i128 / u128 only when the value does not fit a narrower type.
            const auto* il = static_cast<const AST::IntegerLiteral*>(raw);
            const __int128 v = il->value;
            Types::TypeRef litTy;
            const __int128 i64Min = -(((__int128)1) << 63);
            const __int128 i64Max = (((__int128)1) << 63) - 1;
            const __int128 u64Max = (((__int128)1) << 64) - 1;
            if (v >= -2147483648 && v <= 2147483647) {
                litTy = types_.intType(32, true);    // fits in i32 (common case)
            } else if (v >= i64Min && v <= i64Max) {
                litTy = types_.intType(64, true);     // fits in signed i64
            } else if (v >= 0 && v <= u64Max) {
                litTy = types_.intType(64, false);    // fits in u64
            } else if (v >= 0) {
                litTy = types_.intType(128, false);   // needs u128
            } else {
                litTy = types_.intType(128, true);    // needs i128
            }
            return record(raw, litTy);
        }
        case AST::NodeType::FloatLiteral:
            return record(raw, types_.floatType(64));
        case AST::NodeType::BoolLiteral:
            return record(raw, types_.boolType());
        case AST::NodeType::StringLiteral: {
            auto* sl = static_cast<AST::StringLiteral*>(raw);
            if (sl->hasInterpolation) {
                checkInterpolation(sl);
            }
            return record(raw, types_.textType());
        }
        case AST::NodeType::IdentifierExpr:
            return checkIdentifier(static_cast<AST::IdentifierExpr*>(raw));
        case AST::NodeType::UnaryExpr:
            return checkUnary(static_cast<AST::UnaryExpr*>(raw));
        case AST::NodeType::BinaryOperation:
            return checkBinary(static_cast<AST::BinaryOperationExpr*>(raw));
        case AST::NodeType::EqualityCheck:
            return checkEquality(static_cast<AST::EqualityCheckExpr*>(raw));
        case AST::NodeType::LogicalOperation:
            return checkLogical(static_cast<AST::LogicalOperationExpr*>(raw));
        case AST::NodeType::ShiftOperation:
            return checkShift(static_cast<AST::ShiftOperationExpr*>(raw));
        case AST::NodeType::FunctionCall:
            return checkCall(static_cast<AST::FunctionCallExpr*>(raw));
        case AST::NodeType::BuiltinCall:
            return checkBuiltin(static_cast<AST::BuiltinCallExpr*>(raw));
        case AST::NodeType::CastExpr:
            return checkCast(static_cast<AST::CastExpr*>(raw));
        case AST::NodeType::Lambda:
            return checkLambda(static_cast<AST::LambdaExpr*>(raw));
        case AST::NodeType::AddressOfExpr:
            return checkAddressOf(static_cast<AST::AddressOfExpr*>(raw));
        case AST::NodeType::DereferenceExpr:
            return checkDeref(static_cast<AST::DereferenceExpr*>(raw));
        case AST::NodeType::MemberAccess: {
            auto* m = static_cast<AST::MemberAccessExpr*>(raw);
            return m->computed ? checkIndex(m) : checkMember(m);
        }
        case AST::NodeType::SliceExpr:
            return checkSlice(static_cast<AST::SliceExpr*>(raw));
        case AST::NodeType::AssignmentExpr:
            checkAssignment(static_cast<AST::AssignmentExpr*>(raw));
            return record(raw, types_.voidType());
        case AST::NodeType::InlineAsmExpr: {
            if (!inUnsafe_) {
                emit("E2013", "inline asm requires an unsafe context", raw,
                     "wrap this in `unsafe { ... }` or mark the function unsafe(on)");
            }
            auto* a = static_cast<AST::InlineAsmExpr*>(raw);
            for (const auto& in : a->inputs) checkExpr(in);
            Types::TypeRef rt = a->returnType.empty() ? types_.voidType()
                                                      : resolveTypeSpelling(a->returnType, raw);
            return record(raw, rt);
        }
        case AST::NodeType::StructInstantiation: {
            auto* si = static_cast<AST::StructInstantiation*>(raw);
            for (const auto& fv : si->fieldValues) checkExpr(fv.value);
            return record(raw, resolveTypeSpelling(si->typeName, raw));
        }
        case AST::NodeType::NewExpression: {
            auto* ne = static_cast<AST::NewExpression*>(raw);
            if (ne->initializer) checkExpr(ne->initializer);
            if (ne->arraySize) {
                Types::TypeRef countTy = checkExpr(ne->arraySize);
                if (countTy && !countTy->isError() &&
                    !(countTy->isInteger() || countTy->kind == Types::Kind::Bool)) {
                    emit("E2005", "new[] size must be an integer", ne->arraySize.get(),
                         "use an integer expression for the element count");
                }
            }
            for (const auto& a : ne->arguments) checkExpr(a);
            Types::TypeRef inner = resolveTypeSpelling(ne->typeName, raw);
            return record(raw, types_.pointerType(inner));
        }
        case AST::NodeType::DeleteExpression: {
            auto* de = static_cast<AST::DeleteExpression*>(raw);
            if (de->operand) checkExpr(de->operand);
            return record(raw, types_.voidType());
        }
        case AST::NodeType::ArrayLiteral: {
            auto* al = static_cast<AST::ArrayLiteral*>(raw);
            Types::TypeRef elem = nullptr;
            for (const auto& e : al->elements) {
                Types::TypeRef et = checkExpr(e);
                if (!elem) elem = et;
            }
            if (!elem) elem = types_.errorType();
            return record(raw, types_.arrayType(elem, (int64_t)al->elements.size()));
        }
        default:
            return record(raw, types_.errorType());
    }
}


Types::TypeRef Checker::checkIdentifier(AST::IdentifierExpr* node) {
    if (!node) return types_.errorType();
    if (node->name == "this" && currentThis_) {
        return record(node, currentThis_);
    }
    Types::TypeRef t = lookupLocal(node->name);
    if (t) return record(node, t);
    if (auto ev = enumConstants_.find(node->name); ev != enumConstants_.end()) {
        return record(node, ev->second);
    }
    if (functionTable_.find(node->name) != functionTable_.end()) {
        return record(node, types_.errorType());
    }
    if (!alreadyErrored(node)) {
        emit("E2002", "unresolved name '" + node->name + "'", node,
             "this identifier is not declared in any enclosing scope");
        markErrored(node);
    }
    return record(node, types_.errorType());
}

Types::TypeRef Checker::checkUnary(AST::UnaryExpr* node) {
    if (!node) return types_.errorType();
    Types::TypeRef t = node->operand ? checkExpr(node->operand) : types_.errorType();
    if (node->op == "!") {
        if (t && !t->isError() && !(t->isInteger() || t->kind == Types::Kind::Bool)) {
            emit("E2007", "operator '!' requires an integer or bool operand", node,
                 "got " + types_.toString(t));
        }
        return record(node, types_.boolType());
    }
    if (node->op == "-") {
        if (t && !t->isError() && !t->isNumeric()) {
            emit("E2007", "unary '-' requires a numeric operand", node,
                 "got " + types_.toString(t));
            return record(node, types_.errorType());
        }
        return record(node, t);
    }
    return record(node, t);
}

Types::TypeRef Checker::checkBinary(AST::BinaryOperationExpr* node) {
    if (!node) return types_.errorType();
    Types::TypeRef a = node->lhs ? checkExpr(node->lhs) : types_.errorType();
    Types::TypeRef b = node->rhs ? checkExpr(node->rhs) : types_.errorType();
    const std::string& op = node->op;

    if (a && a->kind == Types::Kind::Class) {
        for (const auto& ci : result_.classes) {
            if (ci.name != a->name) continue;
            auto it = ci.operatorMangled.find(op);
            if (it != ci.operatorMangled.end()) {
                if (const FunctionInfo* fn = findFunctionByMangled(it->second)) {
                    return record(node, fn->returnType ? fn->returnType
                                                       : types_.voidType());
                }
            }
        }
    }

    bool comparison = (op == "<" || op == ">" || op == "<=" || op == ">=");
    if (a->isError() || b->isError()) {
        return record(node, comparison ? types_.boolType() : types_.errorType());
    }

    Types::TypeRef ad = enumUnderlying(a);
    Types::TypeRef bd = enumUnderlying(b);

    if ((op == "+" || op == "-") && a->isPointerLike() && b->isInteger()) {
        return record(node, a);
    }
    if (comparison) {
        bool ok = (ad->isNumeric() && bd->isNumeric()) ||
                  (a->isPointerLike() && b->isPointerLike());
        if (!ok) {
            emit("E2006", "comparison operator '" + op + "' requires numeric or pointer operands",
                 node, types_.toString(a) + " " + op + " " + types_.toString(b));
        }
        return record(node, types_.boolType());
    }
    if (ad->isNumeric() && bd->isNumeric()) {
        return record(node, arithResult(ad, bd));
    }
    emit("E2006", "operator '" + op + "' requires numeric operands", node,
         types_.toString(a) + " " + op + " " + types_.toString(b));
    return record(node, types_.errorType());
}

Types::TypeRef Checker::checkEquality(AST::EqualityCheckExpr* node) {
    if (!node) return types_.errorType();
    Types::TypeRef a = node->left ? checkExpr(node->left) : types_.errorType();
    Types::TypeRef b = node->right ? checkExpr(node->right) : types_.errorType();
    if (!a->isError() && !b->isError()) {
        Types::TypeRef ad = enumUnderlying(a);
        Types::TypeRef bd = enumUnderlying(b);
        bool comparable = Types::TypeContext::equals(a, b) ||
                          (ad->isNumeric() && bd->isNumeric()) ||
                          (a->isPointerLike() && b->isPointerLike());
        if (!comparable) {
            emit("E2006", "operator '" + node->op + "' compares incompatible types",
                 node, types_.toString(a) + " vs " + types_.toString(b));
        }
    }
    return record(node, types_.boolType());
}

Types::TypeRef Checker::checkLogical(AST::LogicalOperationExpr* node) {
    if (!node) return types_.errorType();
    Types::TypeRef a = node->left ? checkExpr(node->left) : types_.errorType();
    Types::TypeRef b = node->right ? checkExpr(node->right) : types_.errorType();
    auto okOperand = [](Types::TypeRef t) {
        return t->isError() || t->isInteger() || t->kind == Types::Kind::Bool;
    };
    if (!okOperand(a) || !okOperand(b)) {
        emit("E2006", "logical operator '" + node->op + "' requires bool or integer operands",
             node, types_.toString(a) + " " + node->op + " " + types_.toString(b));
    }
    return record(node, types_.boolType());
}

Types::TypeRef Checker::checkShift(AST::ShiftOperationExpr* node) {
    if (!node) return types_.errorType();
    Types::TypeRef a = node->lhs ? checkExpr(node->lhs) : types_.errorType();
    Types::TypeRef b = node->rhs ? checkExpr(node->rhs) : types_.errorType();
    if (!a->isError() && !a->isInteger()) {
        emit("E2006", "shift operator '" + node->op + "' requires an integer left operand",
             node, "got " + types_.toString(a));
        return record(node, types_.errorType());
    }
    if (!b->isError() && !b->isInteger()) {
        emit("E2006", "shift operator '" + node->op + "' requires an integer right operand",
             node, "got " + types_.toString(b));
    }
    return record(node, a->isError() ? types_.errorType() : a);
}


Types::TypeRef Checker::checkCall(AST::FunctionCallExpr* node) {
    if (!node) return types_.errorType();
    for (const auto& arg : node->arguments) checkExpr(arg);

    std::string name;
    AST::ExprAST* calleeNode = node->callee ? node->callee.get() : nullptr;
    if (node->callee) {
        if (node->callee->nodeType() == AST::NodeType::IdentifierExpr) {
            name = static_cast<AST::IdentifierExpr*>(calleeNode)->name;
        } else if (node->callee->nodeType() == AST::NodeType::MemberAccess) {
            auto* m = static_cast<AST::MemberAccessExpr*>(calleeNode);
            if (m->property && m->property->nodeType() == AST::NodeType::IdentifierExpr) {
                name = static_cast<AST::IdentifierExpr*>(m->property.get())->name;
            }
        }
    }

    // Sum-type variant construction: `E.Variant(args)`. The callee is a member
    // access whose object names a sum type; the result is a value of that type.
    if (calleeNode && calleeNode->nodeType() == AST::NodeType::MemberAccess) {
        const SumVariant* variant = nullptr;
        const SumTypeInfo* st = asSumVariantAccess(
            static_cast<AST::MemberAccessExpr*>(calleeNode), &variant);
        if (st) {
            if (!variant) {
                emit("E2018", "'" + name + "' is not a variant of '" + st->name + "'",
                     node, "");
                return record(node, types_.errorType());
            }
            if (node->arguments.size() != variant->payload.size()) {
                emit("E2008", "variant '" + st->name + "." + variant->name +
                                  "' expects " + std::to_string(variant->payload.size()) +
                                  " field(s), got " +
                                  std::to_string(node->arguments.size()),
                     node, "");
            } else {
                for (size_t i = 0; i < node->arguments.size(); ++i) {
                    Types::TypeRef at = checkExpr(node->arguments[i]);
                    Types::TypeRef pt = variant->payload[i];
                    bool lit = isIntLiteral(node->arguments[i]);
                    if (at && pt && !at->isError() && !pt->isError() &&
                        !isAssignable(pt, at, lit)) {
                        emit("E2009", "payload " + std::to_string(i + 1) + " of '" +
                                          st->name + "." + variant->name +
                                          "' has type " + types_.toString(at) +
                                          ", expected " + types_.toString(pt),
                             node->arguments[i].get(), "");
                    }
                }
            }
            return record(node, types_.namedType(Types::Kind::Struct, st->name));
        }
    }

    if (name.empty()) {
        if (node->callee) checkExpr(node->callee);
        return record(node, types_.errorType());
    }

    {
        Types::TypeRef intrinsic = nullptr;
        if (checkIntrinsicCall(node, name, intrinsic)) {
            return record(node, intrinsic ? intrinsic : types_.voidType());
        }
    }

    if (calleeNode && calleeNode->nodeType() == AST::NodeType::IdentifierExpr &&
        !node->genericArgs.empty()) {
        auto tIt = genericTemplates_.find(name);
        if (tIt != genericTemplates_.end()) {
            AST::FunctionDeclaration* tmpl = tIt->second;

            if (node->genericArgs.size() != tmpl->genericParams.size()) {
                emit("E2008",
                     "wrong number of type arguments to generic function '" + name +
                         "': expected " + std::to_string(tmpl->genericParams.size()) +
                         ", got " + std::to_string(node->genericArgs.size()),
                     node, "");
                return record(node, types_.errorType());
            }

            std::vector<std::string> concreteSpellings(node->genericArgs);
            std::map<std::string, std::string> subSpelling;
            for (size_t i = 0; i < tmpl->genericParams.size(); ++i) {
                subSpelling[tmpl->genericParams[i]] = node->genericArgs[i];
            }

            auto substituteAll = [&](const std::string& spelling) -> std::string {
                std::string out = spelling;
                for (const auto& g : tmpl->genericParams) {
                    out = substituteSpelling(out, g, subSpelling[g]);
                }
                return out;
            };

            std::vector<Types::TypeRef> paramTypes;
            std::vector<std::string> paramNames;
            for (const auto& p : tmpl->parameters) {
                paramNames.push_back(p.name);
                paramTypes.push_back(
                    resolveTypeSpelling(substituteAll(p.type), node));
            }
            Types::TypeRef returnType =
                tmpl->returnType.empty()
                    ? types_.voidType()
                    : resolveTypeSpelling(substituteAll(tmpl->returnType), node);

            if (node->arguments.size() != paramTypes.size()) {
                emit("E2008", "wrong number of arguments to '" + name +
                                  "': expected " + std::to_string(paramTypes.size()) +
                                  ", got " + std::to_string(node->arguments.size()),
                     node, "");
            } else {
                for (size_t i = 0; i < node->arguments.size(); ++i) {
                    Types::TypeRef at = node->arguments[i]
                                            ? checkExpr(node->arguments[i])
                                            : types_.errorType();
                    Types::TypeRef pt = paramTypes[i];
                    bool lit = isIntLiteral(node->arguments[i]);
                    if (at && pt && !at->isError() && !pt->isError() &&
                        !isAssignable(pt, at, lit) &&
                        !isSliceInitializer(pt, at, node->arguments[i])) {
                        emit("E2009", "argument " + std::to_string(i + 1) + " to '" +
                                          name + "' has type " + types_.toString(at) +
                                          ", expected " + types_.toString(pt),
                             node->arguments[i].get(), "");
                    }
                }
            }

            std::string mangled =
                Sema::mangleGenericInstance(name, node->genericArgs);

            bool exists = false;
            for (const auto& gi : result_.genericInstantiations) {
                if (gi.mangledName == mangled) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                GenericInstantiation inst;
                inst.templateName = name;
                inst.templateDecl = tmpl;
                inst.typeArgs = node->genericArgs;
                inst.mangledName = mangled;
                inst.paramTypes = paramTypes;
                inst.paramNames = paramNames;
                inst.returnType = returnType;
                inst.decl = AST::cloneFunctionDeclaration(*tmpl);
                result_.genericInstantiations.push_back(std::move(inst));
            }

            result_.callTargets[node] = mangled;
            return record(node, returnType);
        }
    }

    if (calleeNode && calleeNode->nodeType() == AST::NodeType::IdentifierExpr &&
        !node->genericArgs.empty() &&
        genericClassTemplates_.find(name) != genericClassTemplates_.end()) {
        std::string mangled = instantiateGenericClass(name, node->genericArgs, node);
        if (mangled.empty()) {
            return record(node, types_.errorType());
        }
        for (const auto& ci : result_.classes) {
            if (ci.name != mangled) continue;
            if (!ci.constructorMangled.empty()) {
                if (node->arguments.size() != ci.constructorParams.size()) {
                    emit("E2008", "wrong number of arguments to constructor '" +
                                      name + "': expected " +
                                      std::to_string(ci.constructorParams.size()) +
                                      ", got " +
                                      std::to_string(node->arguments.size()),
                         node, "");
                } else {
                    for (size_t i = 0; i < node->arguments.size(); ++i) {
                        Types::TypeRef at = node->arguments[i]
                                                ? checkExpr(node->arguments[i])
                                                : types_.errorType();
                        Types::TypeRef pt = ci.constructorParams[i];
                        bool lit = isIntLiteral(node->arguments[i]);
                        if (at && pt && !at->isError() && !pt->isError() &&
                            !isAssignable(pt, at, lit) &&
                            !isSliceInitializer(pt, at, node->arguments[i])) {
                            emit("E2009", "argument " + std::to_string(i + 1) +
                                              " to constructor '" + name +
                                              "' has type " + types_.toString(at) +
                                              ", expected " + types_.toString(pt),
                                 node->arguments[i].get(), "");
                        }
                    }
                }
                result_.callTargets[node] = ci.constructorMangled;
            }
            return record(node, types_.namedType(Types::Kind::Class, mangled));
        }
        return record(node, types_.namedType(Types::Kind::Class, mangled));
    }

    if (calleeNode && calleeNode->nodeType() == AST::NodeType::IdentifierExpr) {
        for (const auto& ci : result_.classes) {
            if (ci.name == name) {
                if (!ci.constructorMangled.empty()) {
                    if (node->arguments.size() != ci.constructorParams.size()) {
                        emit("E2008", "wrong number of arguments to constructor '" +
                                          name + "': expected " +
                                          std::to_string(ci.constructorParams.size()) +
                                          ", got " + std::to_string(node->arguments.size()),
                             node, "");
                    }
                }
                return record(node, types_.namedType(Types::Kind::Class, name));
            }
        }
    }

    if (calleeNode && calleeNode->nodeType() == AST::NodeType::MemberAccess) {
        auto* m = static_cast<AST::MemberAccessExpr*>(calleeNode);
        if (!m->computed && m->object) {
            Types::TypeRef objType = nullptr;
            if (m->object->nodeType() == AST::NodeType::IdentifierExpr) {
                auto* idn = static_cast<AST::IdentifierExpr*>(m->object.get());
                if (idn->name == "this" && currentThis_) {
                    objType = currentThis_;
                } else {
                    objType = lookupLocal(idn->name);
                }
            } else if (isScopePath(m->object.get())) {
                objType = nullptr;
            } else {
                objType = checkExpr(m->object);
            }
            Types::TypeRef classType = objType;
            if (classType && classType->kind == Types::Kind::Pointer && classType->element) {
                classType = classType->element;
            }
            if (classType && classType->kind == Types::Kind::Class) {
                checkExpr(m->object);
                for (const auto& ci : result_.classes) {
                    if (ci.name == classType->name) {
                        auto it = ci.methodMangled.find(name);
                        if (it != ci.methodMangled.end()) {
                            if (const FunctionInfo* fn = findFunctionByMangled(it->second)) {
                                return record(node, fn->returnType ? fn->returnType
                                                                   : types_.voidType());
                            }
                            return record(node, types_.voidType());
                        }
                        if (!alreadyErrored(node)) {
                            emit("E2002", "class '" + ci.name +
                                              "' has no method '" + name + "'",
                                 node, "");
                            markErrored(node);
                        }
                        return record(node, types_.errorType());
                    }
                }
            }
        }
    }

    // Calling a function-typed local variable (e.g. a lambda stored in `auto f`):
    // `f(args)` is an indirect call whose result type is the function's return
    // type. Arguments were already checked at the top of checkCall.
    if (calleeNode && calleeNode->nodeType() == AST::NodeType::IdentifierExpr) {
        Types::TypeRef vt = lookupLocal(name);
        if (vt && vt->kind == Types::Kind::Closure) vt = vt->element;
        if (vt && vt->kind == Types::Kind::Function) {
            if (node->arguments.size() != vt->params.size()) {
                emit("E2008", "wrong number of arguments to '" + name +
                                  "': expected " + std::to_string(vt->params.size()) +
                                  ", got " + std::to_string(node->arguments.size()),
                     node, "");
            }
            return record(node, vt->returnType ? vt->returnType : types_.voidType());
        }
    }

    auto range = functionTable_.equal_range(name);
    if (range.first == range.second) {
        if (!alreadyErrored(node)) {
            emit("E2002", "call to unknown function '" + name + "'", node,
                 "the function is not declared or imported");
            markErrored(node);
        }
        return record(node, types_.errorType());
    }

    const FunctionInfo* fn = &range.first->second;

    // Disambiguate overloaded names by owning module. Free functions mangle as
    // `<module>_<func>`, so we can prefer the candidate that belongs to the most
    // specific module:
    //   - For `qualifier.func(...)` (member-access callee, e.g. windows.print),
    //     prefer the function from module `qualifier`.
    //   - For a bare `func(...)` call, prefer the function from the module being
    //     analyzed (so a module's internal calls bind to its own functions even
    //     when an imported module exports the same name, e.g. std::io.print vs
    //     windows::io.print).
    {
        std::string preferModule;
        if (calleeNode && calleeNode->nodeType() == AST::NodeType::MemberAccess) {
            auto* m = static_cast<AST::MemberAccessExpr*>(calleeNode);
            if (m->object && m->object->nodeType() == AST::NodeType::IdentifierExpr) {
                preferModule = static_cast<AST::IdentifierExpr*>(m->object.get())->name;
            }
        } else if (calleeNode &&
                   calleeNode->nodeType() == AST::NodeType::IdentifierExpr) {
            preferModule = result_.moduleName;
        }
        if (!preferModule.empty()) {
            const std::string wanted = preferModule + "_" + name;
            for (auto cand = range.first; cand != range.second; ++cand) {
                if (cand->second.mangledName == wanted) {
                    fn = &cand->second;
                    break;
                }
            }
        }
    }

    if (node->arguments.size() != fn->paramTypes.size()) {
        emit("E2008", "wrong number of arguments to '" + name + "': expected " +
                          std::to_string(fn->paramTypes.size()) + ", got " +
                          std::to_string(node->arguments.size()),
             node, "");
    } else {
        for (size_t i = 0; i < node->arguments.size(); ++i) {
            Types::TypeRef at = node->arguments[i] ? checkExpr(node->arguments[i])
                                                   : types_.errorType();
            Types::TypeRef pt = fn->paramTypes[i];
            bool lit = isIntLiteral(node->arguments[i]);
            if (at && pt && !at->isError() && !pt->isError() &&
                !isAssignable(pt, at, lit) &&
                !isSliceInitializer(pt, at, node->arguments[i])) {
                emit("E2009", "argument " + std::to_string(i + 1) + " to '" + name +
                                  "' has type " + types_.toString(at) + ", expected " +
                                  types_.toString(pt),
                     node->arguments[i].get(), "");
            }
        }
    }

    if (fn->isUnsafe && !inUnsafe_) {
        emit("E2013", "calling unsafe function '" + name + "' outside an unsafe context",
             node, "wrap the call in `unsafe { ... }` or mark the caller unsafe(on)");
    }

    // Record the resolved symbol so the backend can lower the call to the exact
    // function sema picked. This is essential for module-qualified calls (e.g.
    // `io.print(...)`) whose MemberAccess callee the selector cannot resolve on
    // its own, and for disambiguating same-named functions from different
    // modules (the selector's fallback would otherwise pick the first match by
    // source name).
    result_.callTargets[node] =
        fn->mangledName.empty() ? fn->name : fn->mangledName;

    return record(node, fn->returnType ? fn->returnType : types_.voidType());
}

bool Checker::checkIntrinsicCall(AST::FunctionCallExpr* node, const std::string& name,
                                 Types::TypeRef& out) {
    auto requireUnsafe = [&](const char* what) {
        if (!inUnsafe_) {
            emit("E2013",
                 std::string(what) + " requires an unsafe context", node,
                 "wrap the call in `unsafe { ... }` or mark the function unsafe(on)");
        }
    };
    auto genericType = [&]() -> Types::TypeRef {
        if (!node->genericArgs.empty()) {
            return resolveTypeSpelling(node->genericArgs.front(), node);
        }
        return types_.voidType();
    };

    if (name == "asm") {
        requireUnsafe("inline assembly");
        out = node->genericArgs.empty() ? types_.voidType() : genericType();
        return true;
    }
    if (name == "volatileLoad") {
        requireUnsafe("volatile load");
        out = genericType();
        return true;
    }
    if (name == "volatileStore") {
        requireUnsafe("volatile store");
        out = types_.voidType();
        return true;
    }
    if (name == "atomicLoad" || name == "atomicFetchAdd") {
        requireUnsafe("atomic operation");
        out = genericType();
        return true;
    }
    if (name == "atomicStore" || name == "atomicFence") {
        requireUnsafe("atomic operation");
        out = types_.voidType();
        return true;
    }
    if (name == "atomicCompareExchange") {
        requireUnsafe("atomic operation");
        out = types_.boolType();
        return true;
    }
    if (name == "fnCall") {
        requireUnsafe("indirect call");
        out = node->genericArgs.empty() ? types_.voidType() : genericType();
        return true;
    }
    return false;
}

Types::TypeRef Checker::checkBuiltin(AST::BuiltinCallExpr* node) {
    if (!node) return types_.errorType();
    std::vector<Types::TypeRef> argTypes;
    argTypes.reserve(node->arguments.size());
    for (const auto& arg : node->arguments) argTypes.push_back(checkExpr(arg));

    Builtins::Builtin id = Builtins::lookup(node->name);
    if (id == Builtins::Builtin::Unknown) {
        emit("E2002", "unknown builtin '@" + node->name + "'", node, "");
        return record(node, types_.errorType());
    }
    const Builtins::BuiltinSpec& spec = Builtins::spec(id);
    int argc = static_cast<int>(node->arguments.size());
    if (argc < spec.minArgs || (spec.maxArgs >= 0 && argc > spec.maxArgs)) {
        emit("E2008", "@" + node->name + " expects " + std::to_string(spec.minArgs) +
                          (spec.maxArgs < 0 ? "+" : ".." + std::to_string(spec.maxArgs)) +
                          " arguments, got " + std::to_string(argc),
             node, "");
    }
    if (spec.requiresUnsafe && !inUnsafe_) {
        emit("E2013", "@" + node->name + " requires an unsafe context", node,
             "wrap this in `unsafe { ... }` or mark the function unsafe(on)");
    }
    switch (id) {
        case Builtins::Builtin::Hash: {
            // @hash(s): 64-bit hash of a NUL-terminated `text` (or byte pointer).
            // A string-literal argument is folded at compile time in the backend.
            if (!argTypes.empty()) {
                Types::TypeRef at = argTypes.front();
                if (at && !at->isError() && !at->isPointerLike()) {
                    emit("E2016", "@hash requires a text or pointer argument", node,
                         "pass a `text` value or a byte pointer");
                }
            }
            return record(node, types_.intType(64, false));
        }
        case Builtins::Builtin::Malloc:
        case Builtins::Builtin::Realloc:
            return record(node, types_.pointerType(types_.intType(8, false)));
        case Builtins::Builtin::Utf16:
            if (node->arguments.empty() ||
                node->arguments.front()->nodeType() != AST::NodeType::StringLiteral) {
                emit("E2016", "@utf16 requires a string literal argument", node,
                     "e.g. @utf16(\"EFI\\\\BOOT\\\\KERNEL.EFI\")");
            } else {
                auto* lit = static_cast<AST::StringLiteral*>(node->arguments.front().get());
                if (lit->hasInterpolation) {
                    emit("E2016", "@utf16 does not accept an interpolated string", node,
                         "the wide string must be a compile-time constant");
                }
            }
            return record(node, types_.pointerType(types_.intType(16, false)));
        default:
            return record(node, types_.voidType());
    }
}

Types::TypeRef Checker::checkCast(AST::CastExpr* node) {
    if (!node) return types_.errorType();
    Types::TypeRef from = node->expression ? checkExpr(node->expression)
                                           : types_.errorType();
    Types::TypeRef to = resolveTypeSpelling(node->targetType, node);

    auto isPtrish = [](Types::TypeRef t) {
        return t && (t->kind == Types::Kind::Pointer || t->kind == Types::Kind::Text ||
                     t->kind == Types::Kind::Function);
    };
    bool reinterpreting = isPtrish(from) || isPtrish(to);
    if (reinterpreting && !inUnsafe_) {
        emit("E2013", "cast<" + node->targetType + "> requires an unsafe context", node,
             "pointer casts must be inside `unsafe { ... }` or an unsafe(on) function");
    }
    return record(node, to);
}

namespace {

void collectCapturesInNode(AST::NodePtr& node,
                            const std::set<std::string>& paramNames,
                            std::set<std::string>& captures);

void collectCapturesInList(AST::NodeList& body,
                            const std::set<std::string>& paramNames,
                            std::set<std::string>& captures) {
    for (auto& stmt : body) collectCapturesInNode(stmt, paramNames, captures);
}

void collectCapturesInNode(AST::NodePtr& node,
                            const std::set<std::string>& paramNames,
                            std::set<std::string>& captures) {
    if (!node) return;
    switch (node->nodeType()) {
        case AST::NodeType::IdentifierExpr: {
            auto& id = static_cast<AST::IdentifierExpr&>(*node);
            if (paramNames.count(id.name)) return;
            captures.insert(id.name);
            return;
        }
        case AST::NodeType::FunctionCall: {
            auto& c = static_cast<AST::FunctionCallExpr&>(*node);
            collectCapturesInNode(c.callee, paramNames, captures);
            collectCapturesInList(c.arguments, paramNames, captures);
            return;
        }
        case AST::NodeType::BuiltinCall: {
            auto& b = static_cast<AST::BuiltinCallExpr&>(*node);
            collectCapturesInList(b.arguments, paramNames, captures);
            return;
        }
        case AST::NodeType::BinaryOperation: {
            auto& b = static_cast<AST::BinaryOperationExpr&>(*node);
            collectCapturesInNode(b.lhs, paramNames, captures);
            collectCapturesInNode(b.rhs, paramNames, captures);
            return;
        }
        case AST::NodeType::EqualityCheck: {
            auto& e = static_cast<AST::EqualityCheckExpr&>(*node);
            collectCapturesInNode(e.left, paramNames, captures);
            collectCapturesInNode(e.right, paramNames, captures);
            return;
        }
        case AST::NodeType::LogicalOperation: {
            auto& l = static_cast<AST::LogicalOperationExpr&>(*node);
            collectCapturesInNode(l.left, paramNames, captures);
            collectCapturesInNode(l.right, paramNames, captures);
            return;
        }
        case AST::NodeType::UnaryExpr: {
            auto& u = static_cast<AST::UnaryExpr&>(*node);
            collectCapturesInNode(u.operand, paramNames, captures);
            return;
        }
        case AST::NodeType::CastExpr: {
            auto& c = static_cast<AST::CastExpr&>(*node);
            collectCapturesInNode(c.expression, paramNames, captures);
            return;
        }
        case AST::NodeType::MemberAccess: {
            auto& m = static_cast<AST::MemberAccessExpr&>(*node);
            collectCapturesInNode(m.object, paramNames, captures);
            if (!m.computed) collectCapturesInNode(m.property, paramNames, captures);
            return;
        }
        case AST::NodeType::DereferenceExpr: {
            auto& d = static_cast<AST::DereferenceExpr&>(*node);
            collectCapturesInNode(d.operand, paramNames, captures);
            return;
        }
        case AST::NodeType::AddressOfExpr: {
            auto& a = static_cast<AST::AddressOfExpr&>(*node);
            collectCapturesInNode(a.operand, paramNames, captures);
            return;
        }
        case AST::NodeType::ReturnStatement: {
            auto& r = static_cast<AST::ReturnStatement&>(*node);
            if (r.returnValue) collectCapturesInNode(r.returnValue, paramNames, captures);
            return;
        }
        case AST::NodeType::AssignmentExpr: {
            auto& a = static_cast<AST::AssignmentExpr&>(*node);
            collectCapturesInNode(a.target, paramNames, captures);
            collectCapturesInNode(a.value, paramNames, captures);
            return;
        }
        case AST::NodeType::IfStatement: {
            auto& i = static_cast<AST::IfStatement&>(*node);
            collectCapturesInNode(i.condition, paramNames, captures);
            collectCapturesInList(i.consequent, paramNames, captures);
            if (!i.alternate.empty())
                collectCapturesInList(i.alternate, paramNames, captures);
            return;
        }
        case AST::NodeType::WhileLoop: {
            auto& w = static_cast<AST::WhileLoop&>(*node);
            collectCapturesInNode(w.condition, paramNames, captures);
            collectCapturesInList(w.body, paramNames, captures);
            return;
        }
        case AST::NodeType::ForLoop: {
            auto& f = static_cast<AST::ForLoop&>(*node);
            if (f.isRange) {
                collectCapturesInNode(f.rangeStart, paramNames, captures);
                collectCapturesInNode(f.rangeEnd, paramNames, captures);
            } else {
                collectCapturesInNode(f.iterable, paramNames, captures);
            }
            collectCapturesInList(f.body, paramNames, captures);
            return;
        }
        case AST::NodeType::SwitchStatement: {
            auto& s = static_cast<AST::SwitchStatement&>(*node);
            collectCapturesInNode(s.subject, paramNames, captures);
            for (auto& arm : s.arms)
                collectCapturesInList(arm.body, paramNames, captures);
            return;
        }
        case AST::NodeType::VariableDeclaration: {
            auto& d = static_cast<AST::VariableDeclarationExpr&>(*node);
            if (d.initialValue)
                collectCapturesInNode(d.initialValue, paramNames, captures);
            return;
        }
        case AST::NodeType::UnsafeBlock: {
            auto& u = static_cast<AST::UnsafeBlock&>(*node);
            collectCapturesInList(u.body, paramNames, captures);
            return;
        }
        default:
            return;
    }
}

void transformCaptureList(AST::NodeList& body,
                           const std::set<std::string>& captureSet,
                           const std::string& envName);

AST::NodePtr transformCaptureRef(AST::NodePtr node,
                                  const std::set<std::string>& captureSet,
                                  const std::string& envName) {
    if (!node) return nullptr;
    switch (node->nodeType()) {
        case AST::NodeType::IdentifierExpr: {
            auto& id = static_cast<AST::IdentifierExpr&>(*node);
            if (captureSet.count(id.name)) {
                auto obj = std::make_shared<AST::IdentifierExpr>();
                obj->name = envName;
                auto prop = std::make_shared<AST::IdentifierExpr>();
                prop->name = id.name;
                auto acc = std::make_shared<AST::MemberAccessExpr>();
                acc->object = obj;
                acc->property = prop;
                acc->computed = false;
                return acc;
            }
            return node;
        }
        case AST::NodeType::FunctionCall: {
            auto& c = static_cast<AST::FunctionCallExpr&>(*node);
            c.callee = transformCaptureRef(c.callee, captureSet, envName);
            for (auto& a : c.arguments) a = transformCaptureRef(a, captureSet, envName);
            return node;
        }
        case AST::NodeType::BuiltinCall: {
            auto& b = static_cast<AST::BuiltinCallExpr&>(*node);
            for (auto& a : b.arguments) a = transformCaptureRef(a, captureSet, envName);
            return node;
        }
        case AST::NodeType::BinaryOperation: {
            auto& b = static_cast<AST::BinaryOperationExpr&>(*node);
            b.lhs = transformCaptureRef(b.lhs, captureSet, envName);
            b.rhs = transformCaptureRef(b.rhs, captureSet, envName);
            return node;
        }
        case AST::NodeType::EqualityCheck: {
            auto& e = static_cast<AST::EqualityCheckExpr&>(*node);
            e.left = transformCaptureRef(e.left, captureSet, envName);
            e.right = transformCaptureRef(e.right, captureSet, envName);
            return node;
        }
        case AST::NodeType::LogicalOperation: {
            auto& l = static_cast<AST::LogicalOperationExpr&>(*node);
            l.left = transformCaptureRef(l.left, captureSet, envName);
            l.right = transformCaptureRef(l.right, captureSet, envName);
            return node;
        }
        case AST::NodeType::UnaryExpr: {
            auto& u = static_cast<AST::UnaryExpr&>(*node);
            u.operand = transformCaptureRef(u.operand, captureSet, envName);
            return node;
        }
        case AST::NodeType::CastExpr: {
            auto& c = static_cast<AST::CastExpr&>(*node);
            c.expression = transformCaptureRef(c.expression, captureSet, envName);
            return node;
        }
        case AST::NodeType::MemberAccess: {
            auto& m = static_cast<AST::MemberAccessExpr&>(*node);
            m.object = transformCaptureRef(m.object, captureSet, envName);
            if (!m.computed) m.property = transformCaptureRef(m.property, captureSet, envName);
            return node;
        }
        case AST::NodeType::DereferenceExpr: {
            auto& d = static_cast<AST::DereferenceExpr&>(*node);
            d.operand = transformCaptureRef(d.operand, captureSet, envName);
            return node;
        }
        case AST::NodeType::AddressOfExpr: {
            auto& a = static_cast<AST::AddressOfExpr&>(*node);
            a.operand = transformCaptureRef(a.operand, captureSet, envName);
            return node;
        }
        case AST::NodeType::ReturnStatement: {
            auto& r = static_cast<AST::ReturnStatement&>(*node);
            if (r.returnValue) r.returnValue = transformCaptureRef(r.returnValue, captureSet, envName);
            return node;
        }
        case AST::NodeType::AssignmentExpr: {
            auto& a = static_cast<AST::AssignmentExpr&>(*node);
            a.target = transformCaptureRef(a.target, captureSet, envName);
            a.value = transformCaptureRef(a.value, captureSet, envName);
            return node;
        }
        case AST::NodeType::IfStatement: {
            auto& i = static_cast<AST::IfStatement&>(*node);
            i.condition = transformCaptureRef(i.condition, captureSet, envName);
            transformCaptureList(i.consequent, captureSet, envName);
            if (!i.alternate.empty())
                transformCaptureList(i.alternate, captureSet, envName);
            return node;
        }
        case AST::NodeType::WhileLoop: {
            auto& w = static_cast<AST::WhileLoop&>(*node);
            w.condition = transformCaptureRef(w.condition, captureSet, envName);
            transformCaptureList(w.body, captureSet, envName);
            return node;
        }
        case AST::NodeType::ForLoop: {
            auto& f = static_cast<AST::ForLoop&>(*node);
            if (f.isRange) {
                f.rangeStart = transformCaptureRef(f.rangeStart, captureSet, envName);
                f.rangeEnd = transformCaptureRef(f.rangeEnd, captureSet, envName);
            } else {
                f.iterable = transformCaptureRef(f.iterable, captureSet, envName);
            }
            transformCaptureList(f.body, captureSet, envName);
            return node;
        }
        case AST::NodeType::SwitchStatement: {
            auto& s = static_cast<AST::SwitchStatement&>(*node);
            s.subject = transformCaptureRef(s.subject, captureSet, envName);
            for (auto& arm : s.arms)
                transformCaptureList(arm.body, captureSet, envName);
            return node;
        }
        case AST::NodeType::VariableDeclaration: {
            auto& d = static_cast<AST::VariableDeclarationExpr&>(*node);
            if (d.initialValue)
                d.initialValue = transformCaptureRef(d.initialValue, captureSet, envName);
            return node;
        }
        case AST::NodeType::UnsafeBlock: {
            auto& u = static_cast<AST::UnsafeBlock&>(*node);
            transformCaptureList(u.body, captureSet, envName);
            return node;
        }
        default:
            return node;
    }
}

void transformCaptureList(AST::NodeList& body,
                           const std::set<std::string>& captureSet,
                           const std::string& envName) {
    for (auto& stmt : body) stmt = transformCaptureRef(stmt, captureSet, envName);
}

} // anonymous namespace

Types::TypeRef Checker::checkLambda(AST::LambdaExpr* node) {
    if (!node || !node->function) return record(node, types_.errorType());
    AST::FunctionDeclaration* fn = node->function.get();

    std::vector<Types::TypeRef> paramTypes;
    paramTypes.reserve(fn->parameters.size());
    for (const auto& p : fn->parameters) {
        paramTypes.push_back(resolveTypeSpelling(p.type, node));
    }

    // --- Capture detection ---
    // Walk the body BEFORE isolating scopes; any identifier that exists in
    // enclosing scopes (but is not a lambda parameter) is a capture.
    std::set<std::string> paramNames;
    for (const auto& p : fn->parameters) paramNames.insert(p.name);
    std::set<std::string> captureSet;
    collectCapturesInList(fn->body, paramNames, captureSet);

    // Determine capture types from the enclosing scope
    std::map<std::string, Types::TypeRef> captureTypes;
    for (const auto& cap : captureSet) {
        Types::TypeRef ct = lookupLocal(cap);
        if (ct) captureTypes[cap] = ct;
    }

    // Build the ordered capture list
    node->captures.clear();
    for (const auto& cap : captureSet) node->captures.push_back(cap);

    Types::TypeRef envStructType = nullptr;
    Types::TypeRef envPtrType = nullptr;

    if (!node->captures.empty()) {
        // Create an environment struct for the captured variables
        static int envCounter = 0;
        std::string envName = "__closure_env_" + std::to_string(envCounter++);
        node->envStructName = envName;

        StructInfo sinfo;
        sinfo.name = envName;
        sinfo.fields.emplace_back("__fn", types_.pointerType(types_.voidType()));
        for (const auto& cap : node->captures) {
            auto it = captureTypes.find(cap);
            sinfo.fields.emplace_back(cap,
                it != captureTypes.end() ? it->second : types_.voidType());
        }
        result_.structs.push_back(sinfo);
        types_.registerNamed(envName, Types::Kind::Struct);

        envStructType = types_.namedType(Types::Kind::Struct, envName);
        envPtrType = types_.pointerType(envStructType);

        // Prepend __env parameter to the lifted function
        AST::Parameter envParam;
        envParam.name = "__env";
        envParam.type = envName + "*";
        fn->parameters.insert(fn->parameters.begin(), std::move(envParam));

        // Resolve __env's type for the paramTypes list
        paramTypes.insert(paramTypes.begin(), envPtrType);

        // Transform the body: replace captured-var references with __env.field
        transformCaptureList(fn->body, captureSet, "__env");
    }

    // Isolate the scope and infer the return type (original logic, unchanged)
    std::vector<Scope> savedScopes = std::move(scopes_);
    scopes_.clear();
    const FunctionInfo* savedFn = currentFn_;
    Types::TypeRef savedReturn = currentReturn_;
    bool savedInfer = inferReturn_;
    Types::TypeRef savedInferred = inferredReturn_;
    bool savedInferredLit = inferredFromLiteral_;
    bool savedUnsafe = inUnsafe_;

    currentFn_ = nullptr;
    currentReturn_ = nullptr;
    inferReturn_ = true;
    inferredReturn_ = nullptr;
    inferredFromLiteral_ = false;
    inUnsafe_ = false;

    pushScope();
    for (size_t i = 0; i < fn->parameters.size(); ++i) {
        declareLocal(fn->parameters[i].name, paramTypes[i], node);
    }
    checkBlock(fn->body);
    popScope();

    Types::TypeRef ret = inferredReturn_ ? inferredReturn_ : types_.voidType();

    scopes_ = std::move(savedScopes);
    currentFn_ = savedFn;
    currentReturn_ = savedReturn;
    inferReturn_ = savedInfer;
    inferredReturn_ = savedInferred;
    inferredFromLiteral_ = savedInferredLit;
    inUnsafe_ = savedUnsafe;

    // Concrete return spelling + register the lifted function so it is emitted
    // (exactly once) alongside the module's own functions.
    fn->returnType = types_.toString(ret);
    bool isExtern = false;
    std::string mangled = computeMangledName(fn, isExtern);
    bool present = false;
    for (const auto& f : result_.functions) {
        if (f.mangledName == mangled) { present = true; break; }
    }
    if (!present) {
        FunctionInfo info;
        info.name = fn->name;
        info.mangledName = mangled;
        info.decl = fn;
        info.returnType = ret;
        for (size_t i = 0; i < fn->parameters.size(); ++i) {
            info.paramNames.push_back(fn->parameters[i].name);
            info.paramTypes.push_back(paramTypes[i]);
        }
        result_.functions.push_back(std::move(info));
    }

    return record(node, node->captures.empty()
                             ? types_.functionType(paramTypes, ret)
                             : types_.closureType(types_.functionType(
                                   std::vector<Types::TypeRef>(paramTypes.begin() + 1, paramTypes.end()),
                                   ret)));
}

Types::TypeRef Checker::checkAddressOf(AST::AddressOfExpr* node) {
    if (!node) return types_.errorType();
    Types::TypeRef t = node->operand ? checkExpr(node->operand) : types_.errorType();
    if (!isLValue(node->operand)) {
        emit("E2007", "operator '&' requires an addressable operand (lvalue)", node, "");
        return record(node, types_.errorType());
    }
    return record(node, types_.pointerType(t));
}

Types::TypeRef Checker::checkDeref(AST::DereferenceExpr* node) {
    if (!node) return types_.errorType();
    Types::TypeRef t = node->operand ? checkExpr(node->operand) : types_.errorType();
    if (!inUnsafe_) {
        emit("E2013", "pointer dereference requires an unsafe context", node,
             "wrap this in `unsafe { ... }` or mark the function unsafe(on)");
    }
    if (t && !t->isError()) {
        if (t->kind == Types::Kind::Pointer || t->kind == Types::Kind::Text) {
            Types::TypeRef elem = t->element ? t->element : types_.intType(8, false);
            return record(node, elem);
        }
        emit("E2007", "cannot dereference non-pointer type " + types_.toString(t), node, "");
    }
    return record(node, types_.errorType());
}

Types::TypeRef Checker::checkMember(AST::MemberAccessExpr* node) {
    if (!node) return types_.errorType();

    std::string member;
    if (node->property && node->property->nodeType() == AST::NodeType::IdentifierExpr) {
        member = static_cast<AST::IdentifierExpr*>(node->property.get())->name;
    }

    // `SomeType.insize` / `SomeType.inalign`: the byte size or the alignment of a
    // type named directly, e.g. `i64.insize`, `Point.inalign`, or `T.insize` inside
    // a generic. Answered first, before the object is treated as a value or as a
    // sum-type name: a type name is not a value and would fail name resolution, and
    // for a sum type `Sum.insize` would otherwise be read as a variant that does
    // not exist.
    //
    // A variable of the same name takes precedence, so the value form below still
    // applies and no existing identifier changes meaning.
    const bool wantSize = (member == "insize");
    const bool wantAlign = (member == "inalign");
    if ((wantSize || wantAlign) && node->object && !node->computed &&
        node->object->nodeType() == AST::NodeType::IdentifierExpr) {
        const std::string& spelling =
            static_cast<AST::IdentifierExpr&>(*node->object).name;
        if (!lookupLocal(spelling)) {
            // Resolve quietly: an unknown name here is not necessarily an error,
            // it may be the value form handled further down.
            Types::TypeRef measured;
            auto subst = currentSubst_.find(spelling);
            if (subst != currentSubst_.end()) {
                measured = subst->second;  // generic parameter, monomorphized
            } else {
                measured = types_.fromString(spelling);
            }
            if (measured && !measured->isError()) {
                if (measured->kind == Types::Kind::Generic) {
                    // An unsubstituted type parameter has no size yet. Only
                    // reachable outside any instantiation, so say so plainly
                    // rather than reporting a bogus number.
                    emit("E2015",
                         "'" + spelling +
                             "' is an unsubstituted generic parameter; its size is "
                             "only known once instantiated",
                         node, "");
                    return record(node, types_.errorType());
                }
                if (wantSize) {
                    result_.insizeTypes[node] = measured;
                } else {
                    result_.inalignTypes[node] = measured;
                }
                return record(node, types_.intType(64, false));
            }
        }
    }

    // `E.Variant` where E is a sum type and the variant carries no payload: a
    // nullary variant value. (Payload variants are built via `E.Variant(...)`,
    // handled in checkCall.)
    {
        const SumVariant* variant = nullptr;
        const SumTypeInfo* st = asSumVariantAccess(node, &variant);
        if (st) {
            if (variant && variant->payload.empty()) {
                return record(node, types_.namedType(Types::Kind::Struct, st->name));
            }
            if (variant) {
                emit("E2018", "variant '" + st->name + "." + variant->name +
                                  "' carries a payload; construct it with arguments",
                     node, "e.g. " + st->name + "." + variant->name + "(...)");
                return record(node, types_.namedType(Types::Kind::Struct, st->name));
            }
            emit("E2018", "'" + st->name + "' has no such variant", node, "");
            return record(node, types_.errorType());
        }
    }

    Types::TypeRef objType = node->object ? checkExpr(node->object) : types_.errorType();

    // `value.insize`: the size of the value's own type. Deliberately answered
    // before the pointer auto-deref below, so a pointer reports the size of the
    // pointer (8) rather than of what it points at -- auto-deref exists so field
    // access can reach through a pointer, and these are not fields. A struct that
    // really declares such a field keeps it.
    if ((wantSize || wantAlign) && objType && !objType->isError() &&
        !node->computed) {
        bool shadowedByField = false;
        if (objType->kind == Types::Kind::Struct ||
            objType->kind == Types::Kind::Class) {
            for (const auto& s : result_.structs) {
                if (s.name != objType->name) continue;
                for (const auto& f : s.fields) {
                    if (f.first == member) shadowedByField = true;
                }
            }
        }
        if (!shadowedByField) {
            if (wantSize) {
                result_.insizeTypes[node] = objType;
            } else {
                result_.inalignTypes[node] = objType;
            }
            return record(node, types_.intType(64, false));
        }
    }

    if (objType && objType->kind == Types::Kind::Pointer && objType->element) {
        objType = objType->element;
    }
    if (objType && objType->kind == Types::Kind::Slice) {
        if (member == "ptr") {
            return record(node, types_.pointerType(objType->element));
        }
        if (member == "len") {
            return record(node, types_.intType(64, true));
        }
        emit("E2007", "slice has no member '" + member + "'", node,
             "available members are `.ptr` and `.len`");
        return record(node, types_.errorType());
    }
    if (objType && (objType->kind == Types::Kind::Struct ||
                    objType->kind == Types::Kind::Class)) {
        for (const auto& s : result_.structs) {
            if (s.name == objType->name) {
                for (const auto& f : s.fields) {
                    if (f.first == member) {
                        return record(node, f.second ? f.second : types_.errorType());
                    }
                }
            }
        }
    }
    return record(node, types_.errorType());
}

Types::TypeRef Checker::checkIndex(AST::MemberAccessExpr* node) {
    if (!node) return types_.errorType();
    Types::TypeRef base = node->object ? checkExpr(node->object) : types_.errorType();
    if (node->property) checkExpr(node->property);

    if (base && !base->isError()) {
        if ((base->kind == Types::Kind::Pointer ||
             base->kind == Types::Kind::Text) &&
            !inUnsafe_) {
            emit("E2013", "indexing through a pointer requires an unsafe context", node,
                 "wrap this in `unsafe { ... }` or mark the function unsafe(on)");
        }
        if (base->kind == Types::Kind::Text) {
            return record(node, types_.intType(8, false));
        }
        if (base->kind == Types::Kind::Slice && base->element) {
            return record(node, base->element);
        }
        if (base->element) {
            return record(node, base->element);
        }
    }
    return record(node, types_.errorType());
}

Types::TypeRef Checker::checkSlice(AST::SliceExpr* node) {
    if (!node) return types_.errorType();
    Types::TypeRef base = node->object ? checkExpr(node->object) : types_.errorType();

    auto checkBound = [&](const AST::NodePtr& b) {
        if (!b) return;
        Types::TypeRef t = checkExpr(b);
        if (t && !t->isError() && !t->isInteger()) {
            emit("E2015", "slice bound must be an integer", b.get(),
                 "got " + types_.toString(t));
        }
    };
    checkBound(node->start);
    checkBound(node->end);

    if (!base || base->isError()) return record(node, types_.errorType());

    // The result is a slice over the source's element type. A slice or fixed
    // array slices to a slice of the same element; `text` slices to bytes (u8).
    if (base->kind == Types::Kind::Slice || base->kind == Types::Kind::Array) {
        return record(node, types_.sliceType(base->element));
    }
    if (base->kind == Types::Kind::Text) {
        return record(node, types_.sliceType(types_.intType(8, false)));
    }
    // A raw pointer has no length, so an open upper bound is unknowable; require
    // an explicit end and an unsafe context (the caller vouches for the range).
    if (base->kind == Types::Kind::Pointer) {
        if (!node->end) {
            emit("E2015", "cannot slice a raw pointer without an explicit end bound",
                 node, "write ptr[start..end]; a pointer carries no length");
        }
        if (!inUnsafe_) {
            emit("E2013", "slicing a raw pointer requires an unsafe context", node,
                 "wrap this in `unsafe { ... }` (a pointer range is unchecked)");
        }
        return record(node, types_.sliceType(base->element));
    }

    emit("E2015", "cannot slice a value of type " + types_.toString(base), node,
         "only slices, arrays, text, and (unsafe) pointers can be sliced");
    return record(node, types_.errorType());
}

bool Checker::isFormattable(Types::TypeRef t) {
    if (!t || t->isError()) {
        return false;
    }
    switch (t->kind) {
        case Types::Kind::Int:
        case Types::Kind::Float:
        case Types::Kind::Bool:
        case Types::Kind::Text:
            return true;
        case Types::Kind::Struct:
        case Types::Kind::Class:
            return true;
        default:
            return false;
    }
}

void Checker::checkInterpolation(AST::StringLiteral* node) {
    if (!node) return;
    for (const auto& part : node->exprParts) {
        if (!part) continue;
        Types::TypeRef t = checkExpr(part);
        if (!t || t->isError()) {
            continue;
        }
        if (!isFormattable(t)) {
            if (!alreadyErrored(part.get())) {
                emit("E2014",
                     "cannot interpolate a value of type " + types_.toString(t) +
                         " into a string",
                     part.get(),
                     "interpolation supports numbers, bool, char/u8, text, and "
                     "structs/classes (with a toString method or formattable "
                     "fields)");
                markErrored(part.get());
            }
        }
    }
}

}




