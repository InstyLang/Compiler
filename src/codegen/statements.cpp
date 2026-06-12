#include <codegen/codegen.hpp>
#include <codegen/codegen_internal.hpp>

#include <memory>
#include <utility>
#include <vector>

#include <llvm/IR/DerivedTypes.h>

namespace {
}

bool CodeGenerator::blockTerminates(const AST::NodeList& body) const {
    if (body.empty()) {
        return false;
    }
    const AST::NodePtr& last = body.back();
    if (!last) {
        return false;
    }
    switch (last->nodeType()) {
        case AST::NodeType::ReturnStatement:
        case AST::NodeType::BreakStatement:
        case AST::NodeType::SkipStatement:
            return true;
        default:
            return false;
    }
}

void CodeGenerator::generateBlock(const AST::NodeList& body) {
    pushScope();
    for (const auto& stmt : body) {
        llvm::BasicBlock* current = builder->GetInsertBlock();
        if (current && current->getTerminator()) {
            break;
        }
        generateStatement(stmt);
    }
    popScope();
}

void CodeGenerator::generateStatement(const AST::NodePtr& stmt) {
    if (!stmt) {
        return;
    }

    switch (stmt->nodeType()) {
        case AST::NodeType::VariableDeclaration: {
            auto decl = std::static_pointer_cast<AST::VariableDeclarationExpr>(stmt);
            Types::TypeRef declType = sema_ ? sema_->typeOf(decl.get()) : nullptr;
            llvm::Type* llvmTy = declType ? lowerType(declType)
                                          : typeFactory->createInt(32);

            llvm::AllocaInst* slot =
                builder->CreateAlloca(llvmTy, nullptr, decl->identifier);
            declareLocal(decl->identifier, slot, declType);

            if (decl->initialValue) {
                llvm::Value* init = generateExpression(decl->initialValue);
                if (init) {
                    Types::TypeRef initType =
                        sema_ ? sema_->typeOf(decl->initialValue.get()) : nullptr;
                    init = coerce(init, initType, declType);
                    builder->CreateStore(init, slot);
                }
            }
            return;
        }

        case AST::NodeType::AssignmentExpr: {
            auto assign = std::static_pointer_cast<AST::AssignmentExpr>(stmt);
            Types::TypeRef targetType = nullptr;
            llvm::Value* addr = generateLValue(assign->target, targetType);
            if (!addr) {
                return;
            }
            llvm::Value* value = generateExpression(assign->value);
            if (!value) {
                return;
            }
            Types::TypeRef valueType =
                sema_ ? sema_->typeOf(assign->value.get()) : nullptr;
            value = coerce(value, valueType, targetType);
            builder->CreateStore(value, addr);
            return;
        }

        case AST::NodeType::IfStatement: {
            auto ifs = std::static_pointer_cast<AST::IfStatement>(stmt);
            llvm::Value* cond = generateExpression(ifs->condition);
            if (!cond) {
                return;
            }
            if (!cond->getType()->isIntegerTy(1)) {
                cond = builder->CreateICmpNE(
                    cond, llvm::ConstantInt::get(cond->getType(), 0), "ifcond");
            }

            llvm::Function* fn = currentFunction_;
            bool hasElse = !ifs->alternate.empty();
            llvm::BasicBlock* thenBB = llvm::BasicBlock::Create(*context, "if.then", fn);
            llvm::BasicBlock* elseBB =
                hasElse ? llvm::BasicBlock::Create(*context, "if.else", fn) : nullptr;
            llvm::BasicBlock* mergeBB =
                llvm::BasicBlock::Create(*context, "if.end", fn);

            builder->CreateCondBr(cond, thenBB, hasElse ? elseBB : mergeBB);

            builder->SetInsertPoint(thenBB);
            generateBlock(ifs->consequent);
            if (!builder->GetInsertBlock()->getTerminator()) {
                builder->CreateBr(mergeBB);
            }

            if (hasElse) {
                builder->SetInsertPoint(elseBB);
                generateBlock(ifs->alternate);
                if (!builder->GetInsertBlock()->getTerminator()) {
                    builder->CreateBr(mergeBB);
                }
            }

            builder->SetInsertPoint(mergeBB);
            return;
        }

        case AST::NodeType::WhenStatement: {
            auto when = std::static_pointer_cast<AST::WhenStatement>(stmt);
            llvm::Value* cond = generateExpression(when->condition);
            if (!cond) {
                return;
            }
            if (!cond->getType()->isIntegerTy(1)) {
                cond = builder->CreateICmpNE(
                    cond, llvm::ConstantInt::get(cond->getType(), 0), "whencond");
            }
            llvm::Function* fn = currentFunction_;
            llvm::BasicBlock* thenBB = llvm::BasicBlock::Create(*context, "when.then", fn);
            llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "when.end", fn);
            builder->CreateCondBr(cond, thenBB, mergeBB);
            builder->SetInsertPoint(thenBB);
            generateBlock(when->consequent);
            if (!builder->GetInsertBlock()->getTerminator()) {
                builder->CreateBr(mergeBB);
            }
            builder->SetInsertPoint(mergeBB);
            return;
        }

        case AST::NodeType::SwitchStatement: {
            auto sw = std::static_pointer_cast<AST::SwitchStatement>(stmt);
            llvm::Function* fn = currentFunction_;
            llvm::BasicBlock* mergeBB =
                llvm::BasicBlock::Create(*context, "switch.end", fn);

            std::vector<std::pair<llvm::BasicBlock*, const AST::SwitchArm*>> bodies;
            bool defaulted = false;

            for (const auto& arm : sw->arms) {
                llvm::BasicBlock* armBB =
                    llvm::BasicBlock::Create(*context, "switch.arm", fn);
                bodies.emplace_back(armBB, &arm);

                if (arm.isDefault) {
                    if (!builder->GetInsertBlock()->getTerminator()) {
                        builder->CreateBr(armBB);
                    }
                    defaulted = true;
                    continue;
                }

                for (size_t i = 0; i < arm.patterns.size(); ++i) {
                    auto eq = std::make_shared<AST::EqualityCheckExpr>();
                    eq->op = "==";
                    eq->left = sw->subject;
                    eq->right = arm.patterns[i];
                    llvm::Value* cond = generateExpression(eq);
                    if (!cond) {
                        cond = typeFactory->createConstInt(1, 0);
                    }
                    if (!cond->getType()->isIntegerTy(1)) {
                        cond = builder->CreateICmpNE(
                            cond, llvm::ConstantInt::get(cond->getType(), 0),
                            "switchcond");
                    }
                    llvm::BasicBlock* missBB =
                        llvm::BasicBlock::Create(*context, "switch.next", fn);
                    builder->CreateCondBr(cond, armBB, missBB);
                    builder->SetInsertPoint(missBB);
                }
            }

            if (!defaulted && !builder->GetInsertBlock()->getTerminator()) {
                builder->CreateBr(mergeBB);
            }

            for (const auto& entry : bodies) {
                builder->SetInsertPoint(entry.first);
                generateBlock(entry.second->body);
                if (!builder->GetInsertBlock()->getTerminator()) {
                    builder->CreateBr(mergeBB);
                }
            }

            builder->SetInsertPoint(mergeBB);
            return;
        }

        case AST::NodeType::WhileLoop: {
            auto loop = std::static_pointer_cast<AST::WhileLoop>(stmt);
            llvm::Function* fn = currentFunction_;
            llvm::BasicBlock* headerBB = llvm::BasicBlock::Create(*context, "while.cond", fn);
            llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "while.body", fn);
            llvm::BasicBlock* exitBB = llvm::BasicBlock::Create(*context, "while.end", fn);

            builder->CreateBr(headerBB);
            builder->SetInsertPoint(headerBB);
            llvm::Value* cond = generateExpression(loop->condition);
            if (cond && !cond->getType()->isIntegerTy(1)) {
                cond = builder->CreateICmpNE(
                    cond, llvm::ConstantInt::get(cond->getType(), 0), "whilecond");
            }
            if (!cond) {
                cond = typeFactory->createConstInt(1, 0);
            }
            builder->CreateCondBr(cond, bodyBB, exitBB);

            llvm::BasicBlock* savedBreak = currentLoopBreak_;
            llvm::BasicBlock* savedContinue = currentLoopContinue_;
            currentLoopBreak_ = exitBB;
            currentLoopContinue_ = headerBB;

            builder->SetInsertPoint(bodyBB);
            generateBlock(loop->body);
            if (!builder->GetInsertBlock()->getTerminator()) {
                builder->CreateBr(headerBB);
            }

            currentLoopBreak_ = savedBreak;
            currentLoopContinue_ = savedContinue;

            builder->SetInsertPoint(exitBB);
            return;
        }

        case AST::NodeType::InfiniteLoop: {
            auto loop = std::static_pointer_cast<AST::InfiniteLoop>(stmt);
            llvm::Function* fn = currentFunction_;
            llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "loop.body", fn);
            llvm::BasicBlock* exitBB = llvm::BasicBlock::Create(*context, "loop.end", fn);

            builder->CreateBr(bodyBB);

            llvm::BasicBlock* savedBreak = currentLoopBreak_;
            llvm::BasicBlock* savedContinue = currentLoopContinue_;
            currentLoopBreak_ = exitBB;
            currentLoopContinue_ = bodyBB;

            builder->SetInsertPoint(bodyBB);
            generateBlock(loop->body);
            if (!builder->GetInsertBlock()->getTerminator()) {
                builder->CreateBr(bodyBB);
            }

            currentLoopBreak_ = savedBreak;
            currentLoopContinue_ = savedContinue;

            builder->SetInsertPoint(exitBB);
            return;
        }

        case AST::NodeType::ReturnStatement: {
            auto ret = std::static_pointer_cast<AST::ReturnStatement>(stmt);
            if (ret->returnValue) {
                llvm::Value* value = generateExpression(ret->returnValue);
                Types::TypeRef valueType =
                    sema_ ? sema_->typeOf(ret->returnValue.get()) : nullptr;
                value = coerce(value, valueType, currentReturnType_);
                if (value && currentFunction_ &&
                    !currentFunction_->getReturnType()->isVoidTy()) {
                    builder->CreateRet(value);
                } else {
                    builder->CreateRetVoid();
                }
            } else {
                builder->CreateRetVoid();
            }
            return;
        }

        case AST::NodeType::BreakStatement: {
            if (currentLoopBreak_) {
                builder->CreateBr(currentLoopBreak_);
            }
            return;
        }

        case AST::NodeType::SkipStatement: {
            if (currentLoopContinue_) {
                builder->CreateBr(currentLoopContinue_);
            }
            return;
        }

        case AST::NodeType::UnsafeBlock: {
            auto unsafe = std::static_pointer_cast<AST::UnsafeBlock>(stmt);
            bool saved = unsafeContext_;
            unsafeContext_ = true;
            generateBlock(unsafe->body);
            unsafeContext_ = saved;
            return;
        }

        default:
            generateExpression(stmt);
            return;
    }
}
