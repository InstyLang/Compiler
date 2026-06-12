#include <codegen/codegen.hpp>
#include <codegen/codegen_internal.hpp>

#include <extra/builtins.hpp>

#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/Alignment.h>

namespace {

bool typeIsSigned(Types::TypeRef t) {
    return !t || t->isSigned;
}

bool typeIsFloat(Types::TypeRef t) {
    return t && t->kind == Types::Kind::Float;
}

void balanceIntOperands(llvm::IRBuilder<>& builder, llvm::Value*& lhs,
                        llvm::Value*& rhs, bool signedExtend) {
    llvm::Type* lt = lhs->getType();
    llvm::Type* rt = rhs->getType();
    if (!lt->isIntegerTy() || !rt->isIntegerTy()) {
        return;
    }
    unsigned lw = lt->getIntegerBitWidth();
    unsigned rw = rt->getIntegerBitWidth();
    if (lw == rw) {
        return;
    }
    if (lw < rw) {
        lhs = signedExtend ? builder.CreateSExt(lhs, rt, "sext")
                           : builder.CreateZExt(lhs, rt, "zext");
    } else {
        rhs = signedExtend ? builder.CreateSExt(rhs, lt, "sext")
                           : builder.CreateZExt(rhs, lt, "zext");
    }
}

}

llvm::Value* CodeGenerator::generateExpression(const AST::NodePtr& expr) {
    if (!expr) {
        return nullptr;
    }

    switch (expr->nodeType()) {
        case AST::NodeType::IntegerLiteral: {
            auto lit = std::static_pointer_cast<AST::IntegerLiteral>(expr);
            Types::TypeRef t = sema_ ? sema_->typeOf(lit.get()) : nullptr;
            unsigned width = (t && t->bitWidth) ? static_cast<unsigned>(t->bitWidth) : 32;
            return llvm::ConstantInt::get(typeFactory->createInt(width),
                                          static_cast<uint64_t>(lit->value), true);
        }

        case AST::NodeType::BoolLiteral: {
            auto lit = std::static_pointer_cast<AST::BoolLiteral>(expr);
            return typeFactory->createConstInt(1, lit->value ? 1 : 0);
        }

        case AST::NodeType::FloatLiteral: {
            auto lit = std::static_pointer_cast<AST::FloatLiteral>(expr);
            Types::TypeRef t = sema_ ? sema_->typeOf(lit.get()) : nullptr;
            unsigned width = (t && t->bitWidth) ? static_cast<unsigned>(t->bitWidth) : 64;
            return llvm::ConstantFP::get(typeFactory->createFloat(width), lit->value);
        }

        case AST::NodeType::StringLiteral: {
            auto lit = std::static_pointer_cast<AST::StringLiteral>(expr);
            if (lit->hasInterpolation) {
                return generateInterpolation(*lit);
            }
            return getStringConstant(lit->value);
        }

        case AST::NodeType::IdentifierExpr: {
            auto id = std::static_pointer_cast<AST::IdentifierExpr>(expr);
            if (const ValueSlot* slot = lookupLocal(id->name)) {
                llvm::Type* loadTy = slot->type ? lowerType(slot->type)
                                                : typeFactory->createInt(32);
                return builder->CreateLoad(loadTy, slot->address, id->name);
            }
            {
                auto& state = CodegenInternal::stateFor(this);
                auto git = state.globals.find(id->name);
                if (git != state.globals.end()) {
                    llvm::Type* loadTy = git->second.second
                                             ? lowerType(git->second.second)
                                             : typeFactory->createInt(32);
                    if (loadTy->isArrayTy()) {
                        return git->second.first;
                    }
                    return builder->CreateLoad(loadTy, git->second.first, id->name);
                }
            }
            if (auto it = functions_.find(id->name); it != functions_.end()) {
                return it->second;
            }
            {
                auto& state = CodegenInternal::stateFor(this);
                auto ev = state.enumConstants.find(id->name);
                if (ev != state.enumConstants.end()) {
                    unsigned width = 32;
                    if (ev->second.second && ev->second.second->bitWidth) {
                        width = static_cast<unsigned>(ev->second.second->bitWidth);
                    }
                    return typeFactory->createConstInt(
                        width, static_cast<uint64_t>(ev->second.first));
                }
            }
            reportCodegenError("E40200", "unknown identifier '" + id->name + "'");
            return nullptr;
        }

        case AST::NodeType::UnaryExpr: {
            auto un = std::static_pointer_cast<AST::UnaryExpr>(expr);
            llvm::Value* operand = generateExpression(un->operand);
            if (!operand) {
                return nullptr;
            }
            if (un->op == "!") {
                llvm::Value* cmp = builder->CreateICmpEQ(
                    operand, llvm::ConstantInt::get(operand->getType(), 0), "not");
                return cmp;
            }
            if (un->op == "-") {
                if (operand->getType()->isFloatingPointTy()) {
                    return builder->CreateFNeg(operand, "fneg");
                }
                return builder->CreateNeg(operand, "neg");
            }
            if (un->op == "~") {
                return builder->CreateNot(operand, "bnot");
            }
            return operand;
        }

        case AST::NodeType::BinaryOperation: {
            auto bin = std::static_pointer_cast<AST::BinaryOperationExpr>(expr);
            if (llvm::Value* ov = tryClassOperator(bin->lhs, bin->rhs, bin->op)) {
                return ov;
            }
            llvm::Value* lhs = generateExpression(bin->lhs);
            llvm::Value* rhs = generateExpression(bin->rhs);
            if (!lhs || !rhs) {
                return nullptr;
            }
            Types::TypeRef lt = sema_ ? sema_->typeOf(bin->lhs.get()) : nullptr;
            Types::TypeRef rt = sema_ ? sema_->typeOf(bin->rhs.get()) : nullptr;
            const std::string& op = bin->op;

            bool lhsPtr = lhs->getType()->isPointerTy();
            bool rhsPtr = rhs->getType()->isPointerTy();
            if ((op == "+" || op == "-") && (lhsPtr ^ rhsPtr)) {
                llvm::Value* ptr = lhsPtr ? lhs : rhs;
                llvm::Value* idx = lhsPtr ? rhs : lhs;
                Types::TypeRef ptrType = lhsPtr ? lt : rt;
                llvm::Type* elemTy = typeFactory->createInt(8);
                if (ptrType && ptrType->element) {
                    elemTy = lowerType(ptrType->element);
                } else if (ptrType && ptrType->kind == Types::Kind::Text) {
                    elemTy = typeFactory->createInt(8);
                }
                if (!idx->getType()->isIntegerTy(64)) {
                    idx = idx->getType()->isIntegerTy()
                              ? builder->CreateSExtOrTrunc(idx, typeFactory->createInt(64))
                              : idx;
                }
                if (op == "-") {
                    idx = builder->CreateNeg(idx, "negidx");
                }
                return builder->CreateGEP(elemTy, ptr, idx, "ptr.arith");
            }

            bool isFloat = typeIsFloat(lt) || typeIsFloat(rt) ||
                           lhs->getType()->isFloatingPointTy();
            bool isSigned = typeIsSigned(lt) && typeIsSigned(rt);

            if (!isFloat) {
                balanceIntOperands(*builder, lhs, rhs, isSigned);
            }

            if (op == "+") {
                return isFloat ? builder->CreateFAdd(lhs, rhs, "add")
                               : builder->CreateAdd(lhs, rhs, "add");
            }
            if (op == "-") {
                return isFloat ? builder->CreateFSub(lhs, rhs, "sub")
                               : builder->CreateSub(lhs, rhs, "sub");
            }
            if (op == "*") {
                return isFloat ? builder->CreateFMul(lhs, rhs, "mul")
                               : builder->CreateMul(lhs, rhs, "mul");
            }
            if (op == "/") {
                if (isFloat) return builder->CreateFDiv(lhs, rhs, "div");
                return isSigned ? builder->CreateSDiv(lhs, rhs, "div")
                                : builder->CreateUDiv(lhs, rhs, "div");
            }
            if (op == "%") {
                if (isFloat) return builder->CreateFRem(lhs, rhs, "rem");
                return isSigned ? builder->CreateSRem(lhs, rhs, "rem")
                                : builder->CreateURem(lhs, rhs, "rem");
            }
            if (op == "&") return builder->CreateAnd(lhs, rhs, "and");
            if (op == "|") return builder->CreateOr(lhs, rhs, "or");
            if (op == "^") return builder->CreateXor(lhs, rhs, "xor");

            if (op == "<") {
                if (isFloat) return builder->CreateFCmpOLT(lhs, rhs, "lt");
                return isSigned ? builder->CreateICmpSLT(lhs, rhs, "lt")
                                : builder->CreateICmpULT(lhs, rhs, "lt");
            }
            if (op == ">") {
                if (isFloat) return builder->CreateFCmpOGT(lhs, rhs, "gt");
                return isSigned ? builder->CreateICmpSGT(lhs, rhs, "gt")
                                : builder->CreateICmpUGT(lhs, rhs, "gt");
            }
            if (op == "<=") {
                if (isFloat) return builder->CreateFCmpOLE(lhs, rhs, "le");
                return isSigned ? builder->CreateICmpSLE(lhs, rhs, "le")
                                : builder->CreateICmpULE(lhs, rhs, "le");
            }
            if (op == ">=") {
                if (isFloat) return builder->CreateFCmpOGE(lhs, rhs, "ge");
                return isSigned ? builder->CreateICmpSGE(lhs, rhs, "ge")
                                : builder->CreateICmpUGE(lhs, rhs, "ge");
            }
            reportCodegenError("E40201", "unsupported binary operator '" + op + "'");
            return lhs;
        }

        case AST::NodeType::EqualityCheck: {
            auto eq = std::static_pointer_cast<AST::EqualityCheckExpr>(expr);
            if (llvm::Value* ov = tryClassOperator(eq->left, eq->right, eq->op)) {
                return ov;
            }
            llvm::Value* lhs = generateExpression(eq->left);
            llvm::Value* rhs = generateExpression(eq->right);
            if (!lhs || !rhs) {
                return nullptr;
            }
            bool isFloat = lhs->getType()->isFloatingPointTy();
            if (!isFloat) {
                Types::TypeRef elt = sema_ ? sema_->typeOf(eq->left.get()) : nullptr;
                Types::TypeRef ert = sema_ ? sema_->typeOf(eq->right.get()) : nullptr;
                bool signedExt = typeIsSigned(elt) && typeIsSigned(ert);
                balanceIntOperands(*builder, lhs, rhs, signedExt);
            }
            if (eq->op == "==") {
                return isFloat ? builder->CreateFCmpOEQ(lhs, rhs, "eq")
                               : builder->CreateICmpEQ(lhs, rhs, "eq");
            }
            return isFloat ? builder->CreateFCmpONE(lhs, rhs, "ne")
                           : builder->CreateICmpNE(lhs, rhs, "ne");
        }

        case AST::NodeType::LogicalOperation: {
            auto logic = std::static_pointer_cast<AST::LogicalOperationExpr>(expr);
            llvm::Value* lhs = generateExpression(logic->left);
            if (!lhs) {
                return nullptr;
            }
            if (!lhs->getType()->isIntegerTy(1)) {
                lhs = builder->CreateICmpNE(
                    lhs, llvm::ConstantInt::get(lhs->getType(), 0), "lhsbool");
            }

            llvm::Function* fn = currentFunction_;
            llvm::BasicBlock* startBB = builder->GetInsertBlock();
            llvm::BasicBlock* rhsBB = llvm::BasicBlock::Create(*context, "logic.rhs", fn);
            llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "logic.end", fn);

            bool isAnd = logic->op == "&&";
            if (isAnd) {
                builder->CreateCondBr(lhs, rhsBB, mergeBB);
            } else {
                builder->CreateCondBr(lhs, mergeBB, rhsBB);
            }

            builder->SetInsertPoint(rhsBB);
            llvm::Value* rhs = generateExpression(logic->right);
            if (!rhs) {
                rhs = typeFactory->createConstInt(1, 0);
            }
            if (!rhs->getType()->isIntegerTy(1)) {
                rhs = builder->CreateICmpNE(
                    rhs, llvm::ConstantInt::get(rhs->getType(), 0), "rhsbool");
            }
            llvm::BasicBlock* rhsEndBB = builder->GetInsertBlock();
            builder->CreateBr(mergeBB);

            builder->SetInsertPoint(mergeBB);
            llvm::PHINode* phi = builder->CreatePHI(typeFactory->createInt(1), 2, "logic");
            phi->addIncoming(typeFactory->createConstInt(1, isAnd ? 0 : 1), startBB);
            phi->addIncoming(rhs, rhsEndBB);
            return phi;
        }

        case AST::NodeType::ShiftOperation: {
            auto sh = std::static_pointer_cast<AST::ShiftOperationExpr>(expr);
            llvm::Value* lhs = generateExpression(sh->lhs);
            llvm::Value* rhs = generateExpression(sh->rhs);
            if (!lhs || !rhs) {
                return nullptr;
            }
            Types::TypeRef lt = sema_ ? sema_->typeOf(sh->lhs.get()) : nullptr;
            if (lhs->getType()->isIntegerTy() && rhs->getType()->isIntegerTy() &&
                lhs->getType() != rhs->getType()) {
                rhs = builder->CreateZExtOrTrunc(rhs, lhs->getType(), "shamt");
            }
            if (sh->op == "<<") {
                return builder->CreateShl(lhs, rhs, "shl");
            }
            return typeIsSigned(lt) ? builder->CreateAShr(lhs, rhs, "ashr")
                                    : builder->CreateLShr(lhs, rhs, "lshr");
        }

        case AST::NodeType::FunctionCall: {
            auto call = std::static_pointer_cast<AST::FunctionCallExpr>(expr);
            return generateCall(*call);
        }

        case AST::NodeType::BuiltinCall: {
            auto call = std::static_pointer_cast<AST::BuiltinCallExpr>(expr);
            return generateBuiltinCall(*call);
        }

        case AST::NodeType::CastExpr: {
            auto cast = std::static_pointer_cast<AST::CastExpr>(expr);
            llvm::Value* value = generateExpression(cast->expression);
            if (!value) {
                return nullptr;
            }
            Types::TypeRef from = sema_ ? sema_->typeOf(cast->expression.get()) : nullptr;
            Types::TypeRef to = sema_ ? sema_->typeOf(cast.get()) : nullptr;
            return coerce(value, from, to);
        }

        case AST::NodeType::AddressOfExpr: {
            auto addr = std::static_pointer_cast<AST::AddressOfExpr>(expr);
            Types::TypeRef ty = nullptr;
            return generateLValue(addr->operand, ty);
        }

        case AST::NodeType::DereferenceExpr: {
            auto deref = std::static_pointer_cast<AST::DereferenceExpr>(expr);
            llvm::Value* ptr = generateExpression(deref->operand);
            if (!ptr) {
                return nullptr;
            }
            Types::TypeRef resultType = sema_ ? sema_->typeOf(deref.get()) : nullptr;
            llvm::Type* loadTy = resultType ? lowerType(resultType)
                                            : typeFactory->createInt(8);
            return builder->CreateLoad(loadTy, ptr, "deref");
        }

        case AST::NodeType::MemberAccess: {
            auto member = std::static_pointer_cast<AST::MemberAccessExpr>(expr);
            if (member->computed) {
                llvm::Value* base = generateExpression(member->object);
                llvm::Value* index = generateExpression(member->property);
                if (!base || !index) {
                    return nullptr;
                }
                Types::TypeRef resultType = sema_ ? sema_->typeOf(member.get()) : nullptr;
                llvm::Type* elemTy = resultType ? lowerType(resultType)
                                                : typeFactory->createInt(8);
                if (elemTy->isVoidTy()) {
                    elemTy = typeFactory->createInt(8);
                }
                llvm::Value* ptr = builder->CreateGEP(elemTy, base, index, "idx");
                return builder->CreateLoad(elemTy, ptr, "elem");
            }
            Types::TypeRef fieldType = nullptr;
            llvm::Value* addr = generateLValue(expr, fieldType);
            if (addr) {
                llvm::Type* loadTy = fieldType ? lowerType(fieldType)
                                               : typeFactory->createInt(32);
                return builder->CreateLoad(loadTy, addr, "field");
            }
            reportCodegenError("E40202", "unsupported member access");
            return nullptr;
        }

        case AST::NodeType::NewExpression: {
            if (runtimeOptions_.allocatorMode == Runtime::AllocatorMode::None) {
                reportCodegenError("E40130",
                                   "heap allocation requires an allocator (none selected)");
                return nullptr;
            }
            llvm::Function* alloc = module->getFunction("__ins_alloc");
            if (!alloc) {
                alloc = generateMallocFunction();
            }
            if (!alloc) {
                return nullptr;
            }
            llvm::Value* size = typeFactory->createConstInt(64, 0);
            llvm::Value* align = typeFactory->createConstInt(64, 16);
            return builder->CreateCall(alloc, {size, align}, "new");
        }

        case AST::NodeType::DeleteExpression: {
            auto del = std::static_pointer_cast<AST::DeleteExpression>(expr);
            if (runtimeOptions_.allocatorMode == Runtime::AllocatorMode::None) {
                reportCodegenError("E40131",
                                   "heap free requires an allocator (none selected)");
                return nullptr;
            }
            llvm::Function* freeFn = module->getFunction("__ins_free");
            if (!freeFn) {
                freeFn = generateFreeFunction();
            }
            llvm::Value* ptr = generateExpression(del->operand);
            if (freeFn && ptr) {
                builder->CreateCall(freeFn,
                                    {ptr, typeFactory->createConstInt(64, 0),
                                     typeFactory->createConstInt(64, 16)});
            }
            return nullptr;
        }

        case AST::NodeType::ArrayLiteral:
            reportCodegenError("E40140", "array literals are not yet implemented");
            return nullptr;

        case AST::NodeType::ObjectLiteral:
            reportCodegenError("E40141", "object literals are not yet implemented");
            return nullptr;

        case AST::NodeType::StructInstantiation: {
            auto inst = std::static_pointer_cast<AST::StructInstantiation>(expr);
            auto& state = CodegenInternal::stateFor(this);
            auto it = state.structs.find(inst->typeName);
            if (it == state.structs.end() || !it->second.llvmType) {
                reportCodegenError("E40142",
                                   "unknown struct type '" + inst->typeName + "'");
                return nullptr;
            }
            llvm::StructType* st = it->second.llvmType;
            llvm::Value* tmp = builder->CreateAlloca(st, nullptr, inst->typeName + ".tmp");
            for (const auto& fv : inst->fieldValues) {
                int idx = it->second.indexOf(fv.name);
                if (idx < 0) {
                    reportCodegenError("E40143", "unknown field '" + fv.name +
                                                     "' in struct '" + inst->typeName + "'");
                    continue;
                }
                llvm::Value* val = generateExpression(fv.value);
                if (!val) {
                    continue;
                }
                Types::TypeRef fromT = sema_ ? sema_->typeOf(fv.value.get()) : nullptr;
                Types::TypeRef toT = it->second.fieldType(fv.name);
                if (toT) {
                    val = coerce(val, fromT, toT);
                }
                llvm::Value* fieldPtr = builder->CreateStructGEP(
                    st, tmp, static_cast<unsigned>(idx), fv.name + ".init");
                builder->CreateStore(val, fieldPtr);
            }
            return builder->CreateLoad(st, tmp, inst->typeName + ".val");
        }

        default:
            reportCodegenError("E40203", "unsupported expression in codegen");
            return nullptr;
    }
}

llvm::Value* CodeGenerator::generateLValue(const AST::NodePtr& expr,
                                           Types::TypeRef& outType) {
    if (!expr) {
        return nullptr;
    }

    switch (expr->nodeType()) {
        case AST::NodeType::IdentifierExpr: {
            auto id = std::static_pointer_cast<AST::IdentifierExpr>(expr);
            if (const ValueSlot* slot = lookupLocal(id->name)) {
                outType = slot->type;
                return slot->address;
            }
            {
                auto& state = CodegenInternal::stateFor(this);
                auto git = state.globals.find(id->name);
                if (git != state.globals.end()) {
                    outType = git->second.second;
                    return git->second.first;
                }
            }
            reportCodegenError("E40210", "cannot take address of '" + id->name + "'");
            return nullptr;
        }

        case AST::NodeType::DereferenceExpr: {
            auto deref = std::static_pointer_cast<AST::DereferenceExpr>(expr);
            outType = sema_ ? sema_->typeOf(deref.get()) : nullptr;
            return generateExpression(deref->operand);
        }

        case AST::NodeType::MemberAccess: {
            auto member = std::static_pointer_cast<AST::MemberAccessExpr>(expr);
            if (member->computed) {
                llvm::Value* base = generateExpression(member->object);
                llvm::Value* index = generateExpression(member->property);
                outType = sema_ ? sema_->typeOf(member.get()) : nullptr;
                llvm::Type* elemTy = outType ? lowerType(outType)
                                             : typeFactory->createInt(8);
                if (elemTy->isVoidTy()) {
                    elemTy = typeFactory->createInt(8);
                }
                if (!base || !index) {
                    return nullptr;
                }
                return builder->CreateGEP(elemTy, base, index, "idx.addr");
            }
            Types::TypeRef objType = sema_ ? sema_->typeOf(member->object.get()) : nullptr;
            std::string fieldName;
            if (member->property &&
                member->property->nodeType() == AST::NodeType::IdentifierExpr) {
                fieldName =
                    std::static_pointer_cast<AST::IdentifierExpr>(member->property)->name;
            }

            std::string aggName;
            llvm::Value* basePtr = nullptr;
            if (objType && objType->kind == Types::Kind::Pointer && objType->element &&
                (objType->element->kind == Types::Kind::Struct ||
                 objType->element->kind == Types::Kind::Class)) {
                aggName = objType->element->name;
                basePtr = generateExpression(member->object);
            } else if (objType && (objType->kind == Types::Kind::Struct ||
                                    objType->kind == Types::Kind::Class)) {
                aggName = objType->name;
                Types::TypeRef objLValueType = nullptr;
                basePtr = generateLValue(member->object, objLValueType);
            }

            if (basePtr && !aggName.empty()) {
                auto& state = CodegenInternal::stateFor(this);
                if (!state.activeClassInstance.empty() &&
                    member->object &&
                    member->object->nodeType() == AST::NodeType::IdentifierExpr &&
                    std::static_pointer_cast<AST::IdentifierExpr>(member->object)
                            ->name == "this") {
                    aggName = state.activeClassInstance;
                }
                auto it = state.structs.find(aggName);
                if (it != state.structs.end()) {
                    int idx = it->second.indexOf(fieldName);
                    if (idx >= 0) {
                        outType = it->second.fieldType(fieldName);
                        return builder->CreateStructGEP(it->second.llvmType, basePtr,
                                                        static_cast<unsigned>(idx),
                                                        fieldName + ".addr");
                    }
                }
            }
            reportCodegenError("E40211", "unknown struct field '" + fieldName + "'");
            return nullptr;
        }

        default:
            reportCodegenError("E40212", "expression is not an lvalue");
            return nullptr;
    }
}

llvm::Value* CodeGenerator::coerceIntTo(llvm::Value* value, unsigned bits) {
    if (!value || !value->getType()->isIntegerTy()) {
        return value;
    }
    unsigned src = value->getType()->getIntegerBitWidth();
    llvm::Type* target = typeFactory->createInt(bits);
    if (src == bits) return value;
    if (src < bits) return builder->CreateZExt(value, target, "zext");
    return builder->CreateTrunc(value, target, "trunc");
}

llvm::Value* CodeGenerator::tryClassOperator(const AST::NodePtr& lhs,
                                             const AST::NodePtr& rhs,
                                             const std::string& op) {
    if (!sema_ || !lhs) {
        return nullptr;
    }
    Types::TypeRef lt = sema_->typeOf(lhs.get());
    if (!lt || lt->kind != Types::Kind::Class) {
        return nullptr;
    }
    std::string mangled;
    for (const auto& ci : sema_->classes) {
        if (ci.name != lt->name) continue;
        auto it = ci.operatorMangled.find(op);
        if (it != ci.operatorMangled.end()) {
            mangled = it->second;
        }
        break;
    }
    if (mangled.empty()) {
        return nullptr;
    }
    llvm::Function* fn = module->getFunction(mangled);
    if (!fn) {
        return nullptr;
    }

    Types::TypeRef thisType = nullptr;
    llvm::Value* thisPtr = generateLValue(lhs, thisType);
    if (!thisPtr) {
        return nullptr;
    }

    std::vector<llvm::Value*> args;
    args.push_back(thisPtr);
    if (rhs) {
        llvm::Value* r = generateExpression(rhs);
        if (!r) {
            return nullptr;
        }
        if (fn->getFunctionType()->getNumParams() >= 2) {
            llvm::Type* want = fn->getFunctionType()->getParamType(1);
            if (r->getType() != want && r->getType()->isIntegerTy() &&
                want->isIntegerTy()) {
                r = coerceIntTo(r, want->getIntegerBitWidth());
            }
        }
        args.push_back(r);
    }

    if (fn->getReturnType()->isVoidTy()) {
        builder->CreateCall(fn, args);
        return nullptr;
    }
    return builder->CreateCall(fn, args, "op");
}

llvm::Value* CodeGenerator::coerce(llvm::Value* value, Types::TypeRef from,
                                   Types::TypeRef to) {
    if (!value || !to) {
        return value;
    }
    llvm::Type* targetTy = lowerType(to);
    llvm::Type* srcTy = value->getType();
    if (srcTy == targetTy) {
        return value;
    }

    if (srcTy->isIntegerTy() && targetTy->isIntegerTy()) {
        unsigned srcBits = srcTy->getIntegerBitWidth();
        unsigned dstBits = targetTy->getIntegerBitWidth();
        if (dstBits < srcBits) {
            return builder->CreateTrunc(value, targetTy, "trunc");
        }
        if (dstBits > srcBits) {
            bool signedSrc = from ? from->isSigned : true;
            return signedSrc ? builder->CreateSExt(value, targetTy, "sext")
                             : builder->CreateZExt(value, targetTy, "zext");
        }
        return value;
    }

    if (srcTy->isIntegerTy() && targetTy->isFloatingPointTy()) {
        bool signedSrc = from ? from->isSigned : true;
        return signedSrc ? builder->CreateSIToFP(value, targetTy, "sitofp")
                         : builder->CreateUIToFP(value, targetTy, "uitofp");
    }
    if (srcTy->isFloatingPointTy() && targetTy->isIntegerTy()) {
        bool signedDst = to->isSigned;
        return signedDst ? builder->CreateFPToSI(value, targetTy, "fptosi")
                         : builder->CreateFPToUI(value, targetTy, "fptoui");
    }

    if (srcTy->isFloatingPointTy() && targetTy->isFloatingPointTy()) {
        if (srcTy->getPrimitiveSizeInBits() < targetTy->getPrimitiveSizeInBits()) {
            return builder->CreateFPExt(value, targetTy, "fpext");
        }
        return builder->CreateFPTrunc(value, targetTy, "fptrunc");
    }

    if (srcTy->isPointerTy() && targetTy->isPointerTy()) {
        return value;
    }
    if (srcTy->isIntegerTy() && targetTy->isPointerTy()) {
        return builder->CreateIntToPtr(value, targetTy, "inttoptr");
    }
    if (srcTy->isPointerTy() && targetTy->isIntegerTy()) {
        return builder->CreatePtrToInt(value, targetTy, "ptrtoint");
    }

    return value;
}


namespace {

std::string flattenAccessChain(const AST::NodePtr& node) {
    if (!node) {
        return std::string();
    }
    if (node->nodeType() == AST::NodeType::IdentifierExpr) {
        return std::static_pointer_cast<AST::IdentifierExpr>(node)->name;
    }
    if (node->nodeType() == AST::NodeType::MemberAccess) {
        auto member = std::static_pointer_cast<AST::MemberAccessExpr>(node);
        if (member->computed || !member->object || !member->property ||
            member->property->nodeType() != AST::NodeType::IdentifierExpr) {
            return std::string();
        }
        std::string base = flattenAccessChain(member->object);
        if (base.empty()) {
            return std::string();
        }
        const std::string sep = member->isScope ? "::" : ".";
        return base + sep +
               std::static_pointer_cast<AST::IdentifierExpr>(member->property)->name;
    }
    return std::string();
}

std::string flattenCalleeName(const AST::NodePtr& callee, std::string& moduleName,
                              std::string& leaf) {
    if (!callee) {
        return std::string();
    }
    if (callee->nodeType() == AST::NodeType::IdentifierExpr) {
        leaf = std::static_pointer_cast<AST::IdentifierExpr>(callee)->name;
        return leaf;
    }
    if (callee->nodeType() == AST::NodeType::MemberAccess) {
        auto member = std::static_pointer_cast<AST::MemberAccessExpr>(callee);
        if (!member->computed && member->object && member->property &&
            member->property->nodeType() == AST::NodeType::IdentifierExpr) {
            std::string fullPath = flattenAccessChain(member->object);
            if (fullPath.empty()) {
                return std::string();
            }
            std::string::size_type sepPos = fullPath.find_last_of(":.");
            moduleName = (sepPos == std::string::npos)
                             ? fullPath
                             : fullPath.substr(sepPos + 1);
            leaf = std::static_pointer_cast<AST::IdentifierExpr>(member->property)->name;
            const std::string sep = member->isScope ? "::" : ".";
            return fullPath + sep + leaf;
        }
    }
    return std::string();
}

}

llvm::Value* CodeGenerator::generateCall(const AST::FunctionCallExpr& call) {
    {
        std::string m, l;
        flattenCalleeName(call.callee, m, l);
        if (m.empty() && !l.empty()) {
            bool handled = false;
            llvm::Value* result = generateIntrinsicCall(call, l, handled);
            if (handled) {
                return result;
            }
        }
    }

    if (sema_ && CodegenInternal::callHasGenericArgs(call) && call.callee &&
        call.callee->nodeType() == AST::NodeType::IdentifierExpr) {
        Types::TypeRef callType = sema_->typeOf(&call);
        if (callType && callType->kind == Types::Kind::Class) {
            const Sema::ClassInfo* cinfo = nullptr;
            for (const auto& ci : sema_->classes) {
                if (ci.name == callType->name) {
                    cinfo = &ci;
                    break;
                }
            }
            if (cinfo) {
                auto& state = CodegenInternal::stateFor(this);
                auto sit = state.structs.find(cinfo->name);
                if (sit == state.structs.end() || !sit->second.llvmType) {
                    reportCodegenError("E40230",
                                       "unknown class type '" + cinfo->name + "'");
                    return nullptr;
                }
                llvm::StructType* st = sit->second.llvmType;
                llvm::Value* tmp =
                    builder->CreateAlloca(st, nullptr, cinfo->name + ".tmp");

                std::string ctorSym = cinfo->constructorMangled;
                auto ctIt = sema_->callTargets.find(&call);
                if (ctIt != sema_->callTargets.end() && !ctIt->second.empty()) {
                    ctorSym = ctIt->second;
                }
                if (!ctorSym.empty()) {
                    const Sema::FunctionInfo* ctorInfo = nullptr;
                    for (const auto& fn : sema_->functions) {
                        if (fn.mangledName == ctorSym) {
                            ctorInfo = &fn;
                            break;
                        }
                    }
                    llvm::Function* ctor = module->getFunction(ctorSym);
                    if (!ctor && ctorInfo) {
                        ctor = getOrDeclareFunction(*ctorInfo);
                    }
                    if (!ctor) {
                        reportCodegenError("E40231",
                                           "missing constructor for class '" +
                                               cinfo->name + "'");
                        return nullptr;
                    }
                    std::vector<llvm::Value*> args;
                    args.push_back(tmp);
                    for (std::size_t i = 0; i < call.arguments.size(); ++i) {
                        llvm::Value* a = generateExpression(call.arguments[i]);
                        if (!a) {
                            return nullptr;
                        }
                        Types::TypeRef fromT = sema_->typeOf(call.arguments[i].get());
                        Types::TypeRef toT =
                            (ctorInfo && (i + 1) < ctorInfo->paramTypes.size())
                                ? ctorInfo->paramTypes[i + 1]
                                : nullptr;
                        if (toT) {
                            a = coerce(a, fromT, toT);
                        }
                        args.push_back(a);
                    }
                    builder->CreateCall(ctor, args);
                } else {
                    builder->CreateStore(llvm::Constant::getNullValue(st), tmp);
                }
                return builder->CreateLoad(st, tmp, cinfo->name + ".val");
            }
        }
    }

    if (sema_ && CodegenInternal::callHasGenericArgs(call)) {
        std::string mangled;
        auto ctIt = sema_->callTargets.find(&call);
        if (ctIt != sema_->callTargets.end()) {
            mangled = ctIt->second;
        }
        if (mangled.empty() && call.callee &&
            call.callee->nodeType() == AST::NodeType::IdentifierExpr) {
            std::string fname =
                std::static_pointer_cast<AST::IdentifierExpr>(call.callee)->name;
            mangled = Sema::mangleGenericInstance(fname, call.genericArgs);
        }
        if (!mangled.empty()) {
            const Sema::GenericInstantiation* inst = nullptr;
            for (const auto& gi : sema_->genericInstantiations) {
                if (gi.mangledName == mangled) {
                    inst = &gi;
                    break;
                }
            }
            llvm::Function* gfn = module->getFunction(mangled);
            if (!gfn) {
                if (auto it = functions_.find(mangled); it != functions_.end()) {
                    gfn = it->second;
                }
            }
            if (gfn) {
                llvm::FunctionType* gTy = gfn->getFunctionType();
                std::vector<llvm::Value*> args;
                args.reserve(call.arguments.size());
                for (std::size_t i = 0; i < call.arguments.size(); ++i) {
                    llvm::Value* a = generateExpression(call.arguments[i]);
                    if (!a) {
                        return nullptr;
                    }
                    Types::TypeRef fromT =
                        sema_->typeOf(call.arguments[i].get());
                    Types::TypeRef toT =
                        (inst && i < inst->paramTypes.size()) ? inst->paramTypes[i]
                                                              : nullptr;
                    if (toT) {
                        a = coerce(a, fromT, toT);
                    } else if (i < gTy->getNumParams()) {
                        llvm::Type* want = gTy->getParamType(i);
                        if (a->getType() != want && a->getType()->isIntegerTy() &&
                            want->isIntegerTy()) {
                            a = coerceIntTo(a, want->getIntegerBitWidth());
                        }
                    }
                    args.push_back(a);
                }
                if (gTy->getReturnType()->isVoidTy()) {
                    builder->CreateCall(gfn, args);
                    return nullptr;
                }
                return builder->CreateCall(gfn, args, "call");
            }
        }
        reportCodegenError("E40121",
                           "could not resolve generic call instantiation");
        return nullptr;
    }

    std::string moduleName;
    std::string leaf;
    std::string dotted = flattenCalleeName(call.callee, moduleName, leaf);

    if (sema_ && call.callee) {
        if (moduleName.empty() && !leaf.empty() &&
            call.callee->nodeType() == AST::NodeType::IdentifierExpr) {
            for (const auto& ci : sema_->classes) {
                if (ci.name == leaf) {
                    auto& state = CodegenInternal::stateFor(this);
                    auto sit = state.structs.find(ci.name);
                    if (sit == state.structs.end() || !sit->second.llvmType) {
                        reportCodegenError("E40230",
                                           "unknown class type '" + ci.name + "'");
                        return nullptr;
                    }
                    llvm::StructType* st = sit->second.llvmType;
                    llvm::Value* tmp =
                        builder->CreateAlloca(st, nullptr, ci.name + ".tmp");

                    if (!ci.constructorMangled.empty()) {
                        const Sema::FunctionInfo* ctorInfo = nullptr;
                        for (const auto& fn : sema_->functions) {
                            if (fn.mangledName == ci.constructorMangled) {
                                ctorInfo = &fn;
                                break;
                            }
                        }
                        llvm::Function* ctor =
                            module->getFunction(ci.constructorMangled);
                        if (!ctor && ctorInfo) {
                            ctor = getOrDeclareFunction(*ctorInfo);
                        }
                        if (!ctor) {
                            reportCodegenError("E40231",
                                               "missing constructor for class '" +
                                                   ci.name + "'");
                            return nullptr;
                        }
                        std::vector<llvm::Value*> args;
                        args.push_back(tmp);
                        for (std::size_t i = 0; i < call.arguments.size(); ++i) {
                            llvm::Value* a = generateExpression(call.arguments[i]);
                            if (!a) {
                                return nullptr;
                            }
                            Types::TypeRef fromT =
                                sema_->typeOf(call.arguments[i].get());
                            Types::TypeRef toT =
                                (ctorInfo && (i + 1) < ctorInfo->paramTypes.size())
                                    ? ctorInfo->paramTypes[i + 1]
                                    : nullptr;
                            if (toT) {
                                a = coerce(a, fromT, toT);
                            }
                            args.push_back(a);
                        }
                        builder->CreateCall(ctor, args);
                    } else {
                        builder->CreateStore(llvm::Constant::getNullValue(st), tmp);
                    }
                    return builder->CreateLoad(st, tmp, ci.name + ".val");
                }
            }
        }
        if (call.callee->nodeType() == AST::NodeType::MemberAccess) {
            auto member = std::static_pointer_cast<AST::MemberAccessExpr>(call.callee);
            if (!member->computed && member->object) {
                Types::TypeRef objType = sema_->typeOf(member->object.get());
                bool objIsPtr = objType && objType->kind == Types::Kind::Pointer;
                Types::TypeRef classType = objType;
                if (objIsPtr && classType->element) {
                    classType = classType->element;
                }
                if (classType && classType->kind == Types::Kind::Class) {
                    for (const auto& ci : sema_->classes) {
                        if (ci.name != classType->name) {
                            continue;
                        }
                        auto mit = ci.methodMangled.find(leaf);
                        std::string mangled;
                        if (mit != ci.methodMangled.end()) {
                            mangled = mit->second;
                        }
                        if (mangled.empty()) {
                            reportCodegenError("E40232",
                                               "class '" + ci.name +
                                                   "' has no method '" + leaf + "'");
                            return nullptr;
                        }
                        const Sema::FunctionInfo* mInfo = nullptr;
                        for (const auto& fn : sema_->functions) {
                            if (fn.mangledName == mangled) {
                                mInfo = &fn;
                                break;
                            }
                        }
                        llvm::Function* mfn = module->getFunction(mangled);
                        if (!mfn && mInfo) {
                            mfn = getOrDeclareFunction(*mInfo);
                        }
                        if (!mfn) {
                            reportCodegenError("E40233",
                                               "missing method '" + leaf +
                                                   "' for class '" + ci.name + "'");
                            return nullptr;
                        }
                        llvm::Value* thisPtr = nullptr;
                        if (objIsPtr) {
                            thisPtr = generateExpression(member->object);
                        } else {
                            Types::TypeRef lvT = nullptr;
                            thisPtr = generateLValue(member->object, lvT);
                        }
                        if (!thisPtr) {
                            return nullptr;
                        }
                        std::vector<llvm::Value*> args;
                        args.push_back(thisPtr);
                        for (std::size_t i = 0; i < call.arguments.size(); ++i) {
                            llvm::Value* a = generateExpression(call.arguments[i]);
                            if (!a) {
                                return nullptr;
                            }
                            Types::TypeRef fromT =
                                sema_->typeOf(call.arguments[i].get());
                            Types::TypeRef toT =
                                (mInfo && (i + 1) < mInfo->paramTypes.size())
                                    ? mInfo->paramTypes[i + 1]
                                    : nullptr;
                            if (toT) {
                                a = coerce(a, fromT, toT);
                            }
                            args.push_back(a);
                        }
                        if (mfn->getReturnType()->isVoidTy()) {
                            builder->CreateCall(mfn, args);
                            return nullptr;
                        }
                        return builder->CreateCall(mfn, args, "call");
                    }
                }
            }
        }
    }

    llvm::Function* callee = nullptr;
    const Sema::FunctionInfo* matchedInfo = nullptr;

    if (sema_ && !leaf.empty()) {
        for (const auto& info : sema_->functions) {
            if (info.name == leaf || info.name == dotted ||
                info.mangledName == leaf) {
                matchedInfo = &info;
                callee = getOrDeclareFunction(info);
                break;
            }
        }
    }

    if (!callee) {
        if (auto it = functions_.find(leaf); it != functions_.end()) {
            callee = it->second;
        }
    }

    if (!callee && !moduleName.empty() && !leaf.empty()) {
        std::string mangled = Sema::mangleFunction(moduleName, leaf);
        if (llvm::Function* existing = module->getFunction(mangled)) {
            callee = existing;
        } else {
            std::vector<llvm::Type*> paramTys;
            for (const auto& arg : call.arguments) {
                Types::TypeRef at = sema_ ? sema_->typeOf(arg.get()) : nullptr;
                paramTys.push_back(at ? lowerType(at) : typeFactory->createInt(32));
            }
            Types::TypeRef retType = sema_ ? sema_->typeOf(&call) : nullptr;
            llvm::Type* retTy = retType ? lowerType(retType) : typeFactory->createVoid();
            llvm::FunctionType* fnTy = llvm::FunctionType::get(retTy, paramTys, false);
            callee = llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage,
                                            mangled, module.get());
            functions_[mangled] = callee;
        }
    }

    if (!callee) {
        reportCodegenError("E40220", "call to unknown function '" +
                                         (dotted.empty() ? leaf : dotted) + "'");
        return nullptr;
    }

    llvm::FunctionType* fnTy = callee->getFunctionType();

    std::vector<llvm::Value*> args;
    args.reserve(call.arguments.size());
    for (std::size_t i = 0; i < call.arguments.size(); ++i) {
        llvm::Value* arg = generateExpression(call.arguments[i]);
        if (!arg) {
            return nullptr;
        }
        Types::TypeRef fromType =
            sema_ ? sema_->typeOf(call.arguments[i].get()) : nullptr;
        Types::TypeRef toType =
            (matchedInfo && i < matchedInfo->paramTypes.size())
                ? matchedInfo->paramTypes[i]
                : nullptr;
        if (toType) {
            arg = coerce(arg, fromType, toType);
        } else if (i < fnTy->getNumParams()) {
            llvm::Type* expected = fnTy->getParamType(i);
            if (arg->getType() != expected) {
                if (arg->getType()->isIntegerTy() && expected->isIntegerTy()) {
                    unsigned s = arg->getType()->getIntegerBitWidth();
                    unsigned d = expected->getIntegerBitWidth();
                    if (d < s) arg = builder->CreateTrunc(arg, expected);
                    else if (d > s) arg = builder->CreateSExt(arg, expected);
                } else if (arg->getType()->isIntegerTy() && expected->isPointerTy()) {
                    arg = builder->CreateIntToPtr(arg, expected);
                }
            }
        }
        args.push_back(arg);
    }

    if (fnTy->getReturnType()->isVoidTy()) {
        builder->CreateCall(callee, args);
        return nullptr;
    }
    return builder->CreateCall(callee, args, "call");
}


llvm::Value* CodeGenerator::generateIntrinsicCall(const AST::FunctionCallExpr& call,
                                                  const std::string& name,
                                                  bool& handled) {
    handled = true;
    auto genericLLVMType = [&](llvm::Type* fallback) -> llvm::Type* {
        if (!call.genericArgs.empty()) {
            Types::TypeRef t = types_.fromString(call.genericArgs.front());
            if (t && !t->isError() && !t->isVoid()) {
                return lowerType(t);
            }
        }
        Types::TypeRef rt = sema_ ? sema_->typeOf(&call) : nullptr;
        if (rt && !rt->isError() && !rt->isVoid()) {
            return lowerType(rt);
        }
        return fallback;
    };

    if (name == "asm") {
        if (call.arguments.empty()) {
            reportCodegenError("E40310", "asm requires a template string");
            return nullptr;
        }
        auto tmplLit = AST::ast_cast<AST::StringLiteral>(call.arguments[0]);
        std::string templ = tmplLit ? tmplLit->value : "";
        std::string constraints;
        std::vector<llvm::Value*> inputs;
        std::vector<llvm::Type*> inputTys;
        if (call.arguments.size() >= 2) {
            auto consLit = AST::ast_cast<AST::StringLiteral>(call.arguments[1]);
            constraints = consLit ? consLit->value : "";
            for (size_t i = 2; i < call.arguments.size(); ++i) {
                llvm::Value* v = generateExpression(call.arguments[i]);
                if (!v) return nullptr;
                inputs.push_back(v);
                inputTys.push_back(v->getType());
            }
        }
        Types::TypeRef retT = sema_ ? sema_->typeOf(&call) : nullptr;
        llvm::Type* retTy = (retT && !retT->isError() && !retT->isVoid())
                                ? lowerType(retT)
                                : typeFactory->createVoid();
        llvm::FunctionType* asmTy = llvm::FunctionType::get(retTy, inputTys, false);
        llvm::InlineAsm* ia = llvm::InlineAsm::get(asmTy, templ, constraints,
                                                    true);
        llvm::Value* result = builder->CreateCall(asmTy, ia, inputs);
        return retTy->isVoidTy() ? nullptr : result;
    }

    if (name == "volatileLoad") {
        if (call.arguments.empty()) { reportCodegenError("E40311", "volatileLoad requires a pointer"); return nullptr; }
        llvm::Value* ptr = generateExpression(call.arguments[0]);
        llvm::Type* ty = genericLLVMType(typeFactory->createInt(8));
        if (!ptr) return nullptr;
        llvm::LoadInst* ld = builder->CreateLoad(ty, ptr, "vload");
        ld->setVolatile(true);
        return ld;
    }
    if (name == "volatileStore") {
        if (call.arguments.size() < 2) { reportCodegenError("E40312", "volatileStore requires a pointer and a value"); return nullptr; }
        llvm::Value* ptr = generateExpression(call.arguments[0]);
        llvm::Value* val = generateExpression(call.arguments[1]);
        if (!ptr || !val) return nullptr;
        if (!call.genericArgs.empty() && val->getType()->isIntegerTy()) {
            llvm::Type* want = genericLLVMType(val->getType());
            if (want->isIntegerTy() && want != val->getType()) {
                unsigned d = want->getIntegerBitWidth();
                val = coerceIntTo(val, d);
            }
        }
        llvm::StoreInst* st = builder->CreateStore(val, ptr);
        st->setVolatile(true);
        return nullptr;
    }

    if (name == "atomicLoad") {
        if (call.arguments.empty()) { reportCodegenError("E40313", "atomicLoad requires a pointer"); return nullptr; }
        llvm::Value* ptr = generateExpression(call.arguments[0]);
        llvm::Type* ty = genericLLVMType(typeFactory->createInt(32));
        if (!ptr) return nullptr;
        llvm::LoadInst* ld = builder->CreateLoad(ty, ptr, "aload");
        ld->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
        ld->setAlignment(llvm::Align(ty->getPrimitiveSizeInBits() / 8 ? ty->getPrimitiveSizeInBits() / 8 : 1));
        return ld;
    }
    if (name == "atomicStore") {
        if (call.arguments.size() < 2) { reportCodegenError("E40314", "atomicStore requires a pointer and a value"); return nullptr; }
        llvm::Value* ptr = generateExpression(call.arguments[0]);
        llvm::Value* val = generateExpression(call.arguments[1]);
        if (!ptr || !val) return nullptr;
        llvm::StoreInst* st = builder->CreateStore(val, ptr);
        st->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
        unsigned bytes = val->getType()->getPrimitiveSizeInBits() / 8;
        st->setAlignment(llvm::Align(bytes ? bytes : 1));
        return nullptr;
    }
    if (name == "atomicFetchAdd") {
        if (call.arguments.size() < 2) { reportCodegenError("E40315", "atomicFetchAdd requires a pointer and a value"); return nullptr; }
        llvm::Value* ptr = generateExpression(call.arguments[0]);
        llvm::Value* val = generateExpression(call.arguments[1]);
        if (!ptr || !val) return nullptr;
        return builder->CreateAtomicRMW(llvm::AtomicRMWInst::Add, ptr, val,
                                        llvm::MaybeAlign(),
                                        llvm::AtomicOrdering::SequentiallyConsistent);
    }
    if (name == "atomicFence") {
        builder->CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
        return nullptr;
    }
    if (name == "atomicCompareExchange") {
        if (call.arguments.size() < 3) { reportCodegenError("E40316", "atomicCompareExchange requires pointer, expected, desired"); return nullptr; }
        llvm::Value* ptr = generateExpression(call.arguments[0]);
        llvm::Value* expected = generateExpression(call.arguments[1]);
        llvm::Value* desired = generateExpression(call.arguments[2]);
        if (!ptr || !expected || !desired) return nullptr;
        llvm::Value* cx = builder->CreateAtomicCmpXchg(
            ptr, expected, desired, llvm::MaybeAlign(),
            llvm::AtomicOrdering::SequentiallyConsistent,
            llvm::AtomicOrdering::SequentiallyConsistent);
        return builder->CreateExtractValue(cx, 1, "cas.ok");
    }

    if (name == "fnCall") {
        if (call.arguments.empty()) { reportCodegenError("E40317", "fnCall requires a target"); return nullptr; }
        llvm::Value* target = generateExpression(call.arguments[0]);
        if (!target) return nullptr;
        std::vector<llvm::Value*> args;
        std::vector<llvm::Type*> argTys;
        for (size_t i = 1; i < call.arguments.size(); ++i) {
            llvm::Value* v = generateExpression(call.arguments[i]);
            if (!v) return nullptr;
            args.push_back(v);
            argTys.push_back(v->getType());
        }
        Types::TypeRef retT = sema_ ? sema_->typeOf(&call) : nullptr;
        llvm::Type* retTy = (retT && !retT->isError() && !retT->isVoid())
                                ? lowerType(retT)
                                : typeFactory->createVoid();
        llvm::Value* fnPtr = target;
        if (target->getType()->isIntegerTy()) {
            fnPtr = builder->CreateIntToPtr(target, typeFactory->createPointer(), "fnptr");
        }
        llvm::FunctionType* fnTy = llvm::FunctionType::get(retTy, argTys, false);
        llvm::Value* result = builder->CreateCall(fnTy, fnPtr, args);
        return retTy->isVoidTy() ? nullptr : result;
    }

    if (name == "sizeof") {
        if (call.genericArgs.empty()) {
            reportCodegenError("E40318", "sizeof requires a type argument");
            return typeFactory->createConstInt(64, 0);
        }
        llvm::Type* measured = genericLLVMType(typeFactory->createInt(8));
        if (!measured || !measured->isSized()) {
            reportCodegenError("E40318",
                               "sizeof requires a sized type argument");
            return typeFactory->createConstInt(64, 0);
        }
        const llvm::DataLayout& dl = module->getDataLayout();
        uint64_t size = dl.getTypeAllocSize(measured);
        return typeFactory->createConstInt(64, size);
    }

    handled = false;
    return nullptr;
}


llvm::Value* CodeGenerator::generateBuiltinCall(const AST::BuiltinCallExpr& call) {
    Builtins::Builtin id = Builtins::lookup(call.name);
    const Builtins::BuiltinSpec& spec = Builtins::spec(id);

    if (id == Builtins::Builtin::Unknown) {
        reportCodegenError("E40300", "unknown builtin '@" + call.name + "'");
        return nullptr;
    }
    if (!spec.implemented) {
        reportCodegenError("E40301",
                           "builtin '@" + call.name + "' is not yet implemented");
        return nullptr;
    }

    switch (id) {
        case Builtins::Builtin::Sizeof: {
            Types::TypeRef t = (!call.arguments.empty() && sema_)
                                   ? sema_->typeOf(call.arguments[0].get())
                                   : nullptr;
            llvm::Type* ty = t ? lowerType(t) : typeFactory->createInt(32);
            uint64_t bytes = module->getDataLayout().getTypeAllocSize(ty);
            return typeFactory->createConstInt(64, bytes);
        }

        case Builtins::Builtin::Alignof: {
            Types::TypeRef t = (!call.arguments.empty() && sema_)
                                   ? sema_->typeOf(call.arguments[0].get())
                                   : nullptr;
            llvm::Type* ty = t ? lowerType(t) : typeFactory->createInt(32);
            uint64_t align = module->getDataLayout().getABITypeAlign(ty).value();
            return typeFactory->createConstInt(64, align);
        }

        case Builtins::Builtin::Strlen: {
            if (call.arguments.empty()) {
                return typeFactory->createConstInt(64, 0);
            }
            llvm::Value* ptr = generateExpression(call.arguments[0]);
            if (!ptr) {
                return nullptr;
            }
            llvm::Function* fn = currentFunction_;
            llvm::Type* i8Ty = typeFactory->createInt(8);
            llvm::Type* i64Ty = typeFactory->createInt(64);
            llvm::BasicBlock* preBB = builder->GetInsertBlock();
            llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "strlen.loop", fn);
            llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "strlen.done", fn);
            builder->CreateBr(loopBB);

            builder->SetInsertPoint(loopBB);
            llvm::PHINode* idx = builder->CreatePHI(i64Ty, 2, "i");
            idx->addIncoming(typeFactory->createConstInt(64, 0), preBB);
            llvm::Value* cur = builder->CreateGEP(i8Ty, ptr, idx, "cur");
            llvm::Value* ch = builder->CreateLoad(i8Ty, cur, "ch");
            llvm::Value* isNul = builder->CreateICmpEQ(
                ch, llvm::ConstantInt::get(i8Ty, 0), "isnul");
            llvm::Value* next = builder->CreateAdd(
                idx, typeFactory->createConstInt(64, 1), "next");
            idx->addIncoming(next, loopBB);
            builder->CreateCondBr(isNul, doneBB, loopBB);

            builder->SetInsertPoint(doneBB);
            return idx;
        }

        case Builtins::Builtin::Utf16: {
            auto lit = call.arguments.empty()
                           ? nullptr
                           : AST::ast_cast<AST::StringLiteral>(call.arguments[0]);
            if (!lit) {
                reportCodegenError("E40319", "@utf16 requires a string literal argument");
                return nullptr;
            }
            return getWideStringConstant(lit->value);
        }

        case Builtins::Builtin::Syscall: {
            if (call.arguments.empty()) {
                reportCodegenError("E40302", "@syscall requires at least the syscall number");
                return nullptr;
            }
            llvm::Type* i64Ty = typeFactory->createInt(64);
            std::vector<llvm::Value*> args;
            std::vector<llvm::Type*> argTys;
            for (const auto& a : call.arguments) {
                llvm::Value* v = generateExpression(a);
                if (!v) {
                    return nullptr;
                }
                if (v->getType()->isPointerTy()) {
                    v = builder->CreatePtrToInt(v, i64Ty, "arg.i64");
                } else if (v->getType()->isIntegerTy() &&
                           v->getType()->getIntegerBitWidth() != 64) {
                    v = builder->CreateSExt(v, i64Ty, "arg.i64");
                }
                args.push_back(v);
                argTys.push_back(i64Ty);
            }
            static const char* kRegs[] = {"{rax}", "{rdi}", "{rsi}",
                                          "{rdx}", "{r10}", "{r8}", "{r9}"};
            std::string constraints = "={rax}";
            for (std::size_t i = 0; i < args.size() && i < 7; ++i) {
                constraints += ",";
                constraints += kRegs[i];
            }
            constraints += ",~{rcx},~{r11},~{memory}";
            llvm::FunctionType* asmTy = llvm::FunctionType::get(i64Ty, argTys, false);
            llvm::InlineAsm* ia =
                llvm::InlineAsm::get(asmTy, "syscall", constraints, true);
            return builder->CreateCall(ia, args, "syscall");
        }

        case Builtins::Builtin::Panic: {
            llvm::Type* i64Ty = typeFactory->createInt(64);
            llvm::FunctionType* asmTy = llvm::FunctionType::get(
                i64Ty, {i64Ty, i64Ty}, false);
            llvm::InlineAsm* ia = llvm::InlineAsm::get(
                asmTy, "syscall", "={rax},{rax},{rdi},~{rcx},~{r11}", true);
            builder->CreateCall(
                ia, {typeFactory->createConstInt(64, 60),
                     typeFactory->createConstInt(64, 1)});
            builder->CreateUnreachable();
            llvm::BasicBlock* cont =
                llvm::BasicBlock::Create(*context, "after.panic", currentFunction_);
            builder->SetInsertPoint(cont);
            return nullptr;
        }

        case Builtins::Builtin::Bitcast: {
            if (call.arguments.empty()) {
                return nullptr;
            }
            llvm::Value* v = generateExpression(call.arguments[0]);
            Types::TypeRef to = sema_ ? sema_->typeOf(&call) : nullptr;
            return coerce(v, sema_ ? sema_->typeOf(call.arguments[0].get()) : nullptr, to);
        }

        case Builtins::Builtin::IntToPtr: {
            if (call.arguments.empty()) {
                return nullptr;
            }
            llvm::Value* v = generateExpression(call.arguments[0]);
            if (!v) {
                return nullptr;
            }
            return builder->CreateIntToPtr(v, typeFactory->createPointer(), "inttoptr");
        }

        case Builtins::Builtin::PtrToInt: {
            if (call.arguments.empty()) {
                return nullptr;
            }
            llvm::Value* v = generateExpression(call.arguments[0]);
            if (!v) {
                return nullptr;
            }
            return builder->CreatePtrToInt(v, typeFactory->createInt(64), "ptrtoint");
        }

        case Builtins::Builtin::Malloc: {
            if (runtimeOptions_.allocatorMode == Runtime::AllocatorMode::None) {
                reportCodegenError("E40320",
                                   "@malloc requires an allocator (--allocator runtime|external)");
                return nullptr;
            }
            llvm::Function* alloc = module->getFunction("__ins_alloc");
            if (!alloc) alloc = generateMallocFunction();
            if (!alloc) return nullptr;
            llvm::Value* size = call.arguments.empty()
                                    ? typeFactory->createConstInt(64, 0)
                                    : generateExpression(call.arguments[0]);
            llvm::Value* align = call.arguments.size() >= 2
                                     ? generateExpression(call.arguments[1])
                                     : typeFactory->createConstInt(64, 16);
            if (!size) return nullptr;
            size = coerceIntTo(size, 64);
            align = coerceIntTo(align, 64);
            return builder->CreateCall(alloc, {size, align}, "malloc");
        }

        case Builtins::Builtin::Free: {
            if (runtimeOptions_.allocatorMode == Runtime::AllocatorMode::None) {
                reportCodegenError("E40321", "@free requires an allocator");
                return nullptr;
            }
            llvm::Function* freeFn = module->getFunction("__ins_free");
            if (!freeFn) freeFn = generateFreeFunction();
            if (!freeFn || call.arguments.empty()) return nullptr;
            llvm::Value* ptr = generateExpression(call.arguments[0]);
            llvm::Value* size = call.arguments.size() >= 2
                                    ? coerceIntTo(generateExpression(call.arguments[1]), 64)
                                    : typeFactory->createConstInt(64, 0);
            llvm::Value* align = call.arguments.size() >= 3
                                     ? coerceIntTo(generateExpression(call.arguments[2]), 64)
                                     : typeFactory->createConstInt(64, 16);
            if (!ptr) return nullptr;
            builder->CreateCall(freeFn, {ptr, size, align});
            return nullptr;
        }

        case Builtins::Builtin::Realloc: {
            if (runtimeOptions_.allocatorMode == Runtime::AllocatorMode::None) {
                reportCodegenError("E40322", "@realloc requires an allocator");
                return nullptr;
            }
            llvm::Function* reallocFn = module->getFunction("__ins_realloc");
            if (!reallocFn) reallocFn = generateReallocFunction();
            if (!reallocFn || call.arguments.size() < 2) return nullptr;
            llvm::Value* ptr = generateExpression(call.arguments[0]);
            llvm::Value* size = coerceIntTo(generateExpression(call.arguments[1]), 64);
            llvm::Value* align = call.arguments.size() >= 3
                                     ? coerceIntTo(generateExpression(call.arguments[2]), 64)
                                     : typeFactory->createConstInt(64, 16);
            if (!ptr || !size) return nullptr;
            return builder->CreateCall(reallocFn, {ptr, size, align}, "realloc");
        }

        case Builtins::Builtin::Memcpy: {
            llvm::Function* fn = generateMemcpyFunction();
            if (!fn || call.arguments.size() < 3) return nullptr;
            llvm::Value* dst = generateExpression(call.arguments[0]);
            llvm::Value* src = generateExpression(call.arguments[1]);
            llvm::Value* n = coerceIntTo(generateExpression(call.arguments[2]), 64);
            if (!dst || !src || !n) return nullptr;
            builder->CreateCall(fn, {dst, src, n});
            return nullptr;
        }

        case Builtins::Builtin::Memset: {
            llvm::Function* fn = generateMemsetFunction();
            if (!fn || call.arguments.size() < 3) return nullptr;
            llvm::Value* ptr = generateExpression(call.arguments[0]);
            llvm::Value* val = generateExpression(call.arguments[1]);
            llvm::Value* n = coerceIntTo(generateExpression(call.arguments[2]), 64);
            if (!ptr || !val || !n) return nullptr;
            if (val->getType()->isIntegerTy() && !val->getType()->isIntegerTy(8)) {
                val = builder->CreateTrunc(val, typeFactory->createInt(8), "byte");
            }
            builder->CreateCall(fn, {ptr, val, n});
            return nullptr;
        }

        default:
            reportCodegenError("E40303",
                               "builtin '@" + call.name + "' is not yet implemented");
            return nullptr;
    }
}
