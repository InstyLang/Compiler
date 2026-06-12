#include <sema/checker.hpp>

#include <cctype>

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

    Types::TypeRef t = types_.fromString(spelling);
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
    AST::FunctionDeclaration* decl = inst.templateDecl;

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

    for (auto& m : tmpl->methods) {
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
        fi.returnType = m.isConstructor
                            ? types_.voidType()
                            : resolveTypeSpelling(substituteAll(m.returnType), at);
        fi.isExternal = false;
        fi.decl = nullptr;
        result_.functions.push_back(fi);

        if (m.isConstructor) {
            cinfo.constructorMangled = memberMangled;
            cinfo.constructorParams.assign(paramTypes.begin() + 1, paramTypes.end());
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
    currentReturn_ = method.isConstructor
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
    Types::TypeRef declared = nullptr;
    if (!node->typeHint.empty()) {
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

    Types::TypeRef finalType = declared;
    if (!finalType) {
        finalType = initType ? initType : types_.errorType();
    }

    if (declared && initType && !declared->isError() && !initType->isError()) {
        if (!isAssignable(declared, initType, initLiteral)) {
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
        if (!isAssignable(targetType, valueType, valLiteral)) {
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

void Checker::checkWhen(AST::WhenStatement* node) {
    if (!node) return;
    if (node->condition) checkExpr(node->condition);
    pushScope();
    checkBlock(node->consequent);
    popScope();
}

void Checker::checkSwitch(AST::SwitchStatement* node) {
    if (!node) return;
    Types::TypeRef subjT = node->subject ? checkExpr(node->subject) : nullptr;
    for (auto& arm : node->arms) {
        for (auto& pat : arm.patterns) {
            Types::TypeRef pt = checkExpr(pat);
            if (subjT && pt && !subjT->isError() && !pt->isError() &&
                !isAssignable(subjT, pt, isIntLiteral(pat)) &&
                !isAssignable(pt, subjT, false)) {
                emit("E2013", "switch arm pattern type " + types_.toString(pt) +
                                  " is not comparable to subject type " +
                                  types_.toString(subjT),
                     pat.get(), "patterns are matched against the subject with '=='");
            }
        }
        pushScope();
        checkBlock(arm.body);
        popScope();
    }
}

void Checker::checkReturn(AST::ReturnStatement* node) {
    if (!node) return;
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
        !isAssignable(ret, vt, lit)) {
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
        case AST::NodeType::IntegerLiteral:
            return record(raw, types_.intType(32, true));
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
        case AST::NodeType::AddressOfExpr:
            return checkAddressOf(static_cast<AST::AddressOfExpr*>(raw));
        case AST::NodeType::DereferenceExpr:
            return checkDeref(static_cast<AST::DereferenceExpr*>(raw));
        case AST::NodeType::MemberAccess: {
            auto* m = static_cast<AST::MemberAccessExpr*>(raw);
            return m->computed ? checkIndex(m) : checkMember(m);
        }
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
            if (ne->arraySize) checkExpr(ne->arraySize);
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
                for (const auto& fn : result_.functions) {
                    if (fn.mangledName == it->second) {
                        return record(node, fn.returnType ? fn.returnType
                                                          : types_.voidType());
                    }
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
                        !isAssignable(pt, at, lit)) {
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
                            !isAssignable(pt, at, lit)) {
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
                            for (const auto& fn : result_.functions) {
                                if (fn.mangledName == it->second) {
                                    return record(node, fn.returnType ? fn.returnType
                                                                       : types_.voidType());
                                }
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

    auto range = functionTable_.equal_range(name);
    if (range.first == range.second) {
        if (!alreadyErrored(node)) {
            emit("E2002", "call to unknown function '" + name + "'", node,
                 "the function is not declared or imported");
            markErrored(node);
        }
        return record(node, types_.errorType());
    }

    const FunctionInfo* fn = range.first->second;

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
                !isAssignable(pt, at, lit)) {
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
    if (name == "sizeof") {
        if (node->genericArgs.empty()) {
            emit("E2015", "sizeof requires a type argument: sizeof<T>()", node,
                 "write the measured type as a generic argument, e.g. sizeof<u64>()");
        } else {
            Types::TypeRef measured = resolveTypeSpelling(node->genericArgs.front(), node);
            if (!measured || measured->isError()) {
                emit("E2015", "sizeof type argument is not a known type", node, "");
            }
        }
        out = types_.intType(64, false);
        return true;
    }
    return false;
}

Types::TypeRef Checker::checkBuiltin(AST::BuiltinCallExpr* node) {
    if (!node) return types_.errorType();
    for (const auto& arg : node->arguments) checkExpr(arg);

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
        case Builtins::Builtin::Strlen:
        case Builtins::Builtin::Sizeof:
        case Builtins::Builtin::Alignof:
        case Builtins::Builtin::PtrToInt:
            return record(node, types_.intType(64, false));
        case Builtins::Builtin::Malloc:
        case Builtins::Builtin::Realloc:
        case Builtins::Builtin::IntToPtr:
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
                     t->kind == Types::Kind::Slice);
    };
    bool reinterpreting = isPtrish(from) || isPtrish(to);
    if (reinterpreting && !inUnsafe_) {
        emit("E2013", "cast<" + node->targetType + "> requires an unsafe context", node,
             "pointer casts must be inside `unsafe { ... }` or an unsafe(on) function");
    }
    return record(node, to);
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
        if (t->kind == Types::Kind::Pointer || t->kind == Types::Kind::Text ||
            t->kind == Types::Kind::Slice) {
            Types::TypeRef elem = t->element ? t->element : types_.intType(8, false);
            return record(node, elem);
        }
        emit("E2007", "cannot dereference non-pointer type " + types_.toString(t), node, "");
    }
    return record(node, types_.errorType());
}

Types::TypeRef Checker::checkMember(AST::MemberAccessExpr* node) {
    if (!node) return types_.errorType();
    Types::TypeRef objType = node->object ? checkExpr(node->object) : types_.errorType();

    std::string member;
    if (node->property && node->property->nodeType() == AST::NodeType::IdentifierExpr) {
        member = static_cast<AST::IdentifierExpr*>(node->property.get())->name;
    }
    if (objType && objType->kind == Types::Kind::Pointer && objType->element) {
        objType = objType->element;
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
        if (base->element) {
            return record(node, base->element);
        }
    }
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
