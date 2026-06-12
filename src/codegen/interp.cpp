#include <codegen/codegen.hpp>
#include <codegen/codegen_internal.hpp>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>


namespace {
constexpr uint64_t kInterpBufferSize = 1024;
}

static llvm::Value* emitStrlen(CodeGenerator* gen, llvm::IRBuilder<>& b,
                               LLVMTypeFactory* tf, llvm::Value* s) {
    llvm::LLVMContext& ctx = b.getContext();
    llvm::Type* i64 = tf->createInt(64);
    llvm::Type* i8 = tf->createInt(8);
    llvm::Function* fn = b.GetInsertBlock()->getParent();
    auto* loop = llvm::BasicBlock::Create(ctx, "sl.loop", fn);
    auto* done = llvm::BasicBlock::Create(ctx, "sl.done", fn);
    llvm::BasicBlock* pre = b.GetInsertBlock();
    b.CreateBr(loop);
    b.SetInsertPoint(loop);
    llvm::PHINode* idx = b.CreatePHI(i64, 2, "sl.i");
    idx->addIncoming(tf->createConstInt(64, 0), pre);
    llvm::Value* ch = b.CreateLoad(i8, b.CreateGEP(i8, s, idx, "sl.p"), "sl.c");
    llvm::Value* isNul = b.CreateICmpEQ(ch, tf->createConstInt(8, 0), "sl.nul");
    llvm::Value* nidx = b.CreateAdd(idx, tf->createConstInt(64, 1), "sl.ni");
    idx->addIncoming(nidx, loop);
    b.CreateCondBr(isNul, done, loop);
    b.SetInsertPoint(done);
    return idx;
}

llvm::Value* CodeGenerator::emitFormatValue(llvm::Value* buf, llvm::Value* cap,
                                            llvm::Value* off, llvm::Value* value,
                                            Types::TypeRef type,
                                            unsigned recursionDepth) {
    llvm::IRBuilder<>& b = *builder;
    llvm::Type* i64 = typeFactory->createInt(64);
    llvm::Type* i8 = typeFactory->createInt(8);

    if (!type) {
        return off;
    }

    switch (type->kind) {
        case Types::Kind::Int: {
            if (type->bitWidth == 8) {
                auto* fn = b.GetInsertBlock()->getParent();
                auto* emit = llvm::BasicBlock::Create(*context, "ch.emit", fn);
                auto* cont = llvm::BasicBlock::Create(*context, "ch.cont", fn);
                llvm::BasicBlock* pre = b.GetInsertBlock();
                llvm::Value* room = b.CreateICmpULT(off, cap, "ch.room");
                b.CreateCondBr(room, emit, cont);
                b.SetInsertPoint(emit);
                llvm::Value* byte = b.CreateTrunc(value, i8, "ch.byte");
                b.CreateStore(byte, b.CreateGEP(i8, buf, off, "ch.p"));
                llvm::Value* o1 = b.CreateAdd(off, typeFactory->createConstInt(64, 1), "ch.o");
                b.CreateBr(cont);
                b.SetInsertPoint(cont);
                llvm::PHINode* ph = b.CreatePHI(i64, 2, "ch.off");
                ph->addIncoming(off, pre);
                ph->addIncoming(o1, emit);
                return ph;
            }
            llvm::Value* wide = type->isSigned
                                    ? b.CreateSExt(value, i64, "iwide")
                                    : b.CreateZExt(value, i64, "iwide");
            llvm::Value* isSigned = typeFactory->createConstInt(1, type->isSigned ? 1 : 0);
            return b.CreateCall(fmtInt_(), {buf, cap, off, wide, isSigned}, "ioff");
        }
        case Types::Kind::Bool: {
            llvm::Value* tStr = getStringConstant("true");
            llvm::Value* fStr = getStringConstant("false");
            llvm::Value* sel = b.CreateSelect(value, tStr, fStr, "boolstr");
            llvm::Value* len = b.CreateSelect(
                value, typeFactory->createConstInt(64, 4),
                typeFactory->createConstInt(64, 5), "boollen");
            return b.CreateCall(fmtRaw_(), {buf, cap, off, sel, len}, "boff");
        }
        case Types::Kind::Float: {
            llvm::Value* dbl = value;
            if (type->bitWidth != 64) {
                dbl = b.CreateFPExt(value, typeFactory->createFloat(64), "fext");
            }
            return b.CreateCall(fmtFloat_(), {buf, cap, off, dbl}, "foff");
        }
        case Types::Kind::Text: {
            llvm::Value* len = emitStrlen(this, b, typeFactory, value);
            return b.CreateCall(fmtRaw_(), {buf, cap, off, value, len}, "soff");
        }
        case Types::Kind::Struct:
        case Types::Kind::Class:
            return emitFormatAggregate(buf, cap, off, value, type, recursionDepth);
        default:
            return off;
    }
}

llvm::Value* CodeGenerator::emitFormatAggregate(llvm::Value* buf, llvm::Value* cap,
                                                llvm::Value* off, llvm::Value* value,
                                                Types::TypeRef type,
                                                unsigned recursionDepth) {
    llvm::IRBuilder<>& b = *builder;

    auto rawLit = [&](llvm::Value* curOff, const std::string& s) -> llvm::Value* {
        llvm::Value* ptr = getStringConstant(s);
        llvm::Value* len = typeFactory->createConstInt(64, s.size());
        return b.CreateCall(fmtRaw_(), {buf, cap, curOff, ptr, len});
    };

    if (sema_) {
        for (const auto& ci : sema_->classes) {
            if (ci.name != type->name) continue;
            auto it = ci.methodMangled.find("toString");
            if (it != ci.methodMangled.end()) {
                if (llvm::Function* fn = module->getFunction(it->second)) {
                    llvm::Value* thisPtr = materializeAddr(value, type);
                    llvm::Value* str = b.CreateCall(fn, {thisPtr}, "tostr");
                    llvm::Value* len = emitStrlen(this, b, typeFactory, str);
                    return b.CreateCall(fmtRaw_(), {buf, cap, off, str, len});
                }
            }
            break;
        }
    }

    if (recursionDepth >= 8) {
        return rawLit(off, type->name + " { .. }");
    }

    auto& state = CodegenInternal::stateFor(this);
    auto sit = state.structs.find(type->name);
    if (sit == state.structs.end() || sit->second.fields.empty()) {
        return rawLit(off, type->name + " {}");
    }
    const auto& layout = sit->second;
    llvm::Value* thisPtr = materializeAddr(value, type);

    llvm::Value* curOff = rawLit(off, type->name + " { ");
    for (std::size_t i = 0; i < layout.fields.size(); ++i) {
        const auto& f = layout.fields[i];
        std::string prefix = (i == 0 ? "" : ", ") + f.first + ": ";
        curOff = rawLit(curOff, prefix);
        llvm::Value* fieldPtr = b.CreateStructGEP(
            layout.llvmType, thisPtr, static_cast<unsigned>(i), f.first + ".addr");
        llvm::Type* fieldLL = f.second ? lowerType(f.second)
                                       : typeFactory->createInt(32);
        if (f.second && (f.second->kind == Types::Kind::Struct ||
                         f.second->kind == Types::Kind::Class)) {
            curOff = emitFormatValue(buf, cap, curOff, fieldPtr, f.second,
                                     recursionDepth + 1);
        } else {
            llvm::Value* fv = b.CreateLoad(fieldLL, fieldPtr, f.first);
            curOff = emitFormatValue(buf, cap, curOff, fv, f.second,
                                     recursionDepth + 1);
        }
    }
    return rawLit(curOff, " }");
}

llvm::Value* CodeGenerator::materializeAddr(llvm::Value* value, Types::TypeRef type) {
    if (value->getType()->isPointerTy()) {
        return value;
    }
    llvm::IRBuilder<>& b = *builder;
    llvm::Type* ll = type ? lowerType(type) : value->getType();
    llvm::Value* slot = b.CreateAlloca(ll, nullptr, "spill");
    b.CreateStore(value, slot);
    return slot;
}

llvm::Value* CodeGenerator::generateInterpolation(const AST::StringLiteral& literal) {
    llvm::IRBuilder<>& b = *builder;
    llvm::Type* i8 = typeFactory->createInt(8);
    llvm::Type* i64 = typeFactory->createInt(64);

    const uint64_t total = kInterpBufferSize + 1;
    llvm::Value* cap = typeFactory->createConstInt(64, kInterpBufferSize);

    llvm::Value* buf = nullptr;
    if (runtimeOptions_.allowsHeap()) {
        llvm::Function* alloc = generateMallocFunction();
        buf = b.CreateCall(
            alloc,
            {typeFactory->createConstInt(64, total), typeFactory->createConstInt(64, 1)},
            "ibuf");
    } else {
        llvm::Value* arr = b.CreateAlloca(
            llvm::ArrayType::get(i8, total), nullptr, "ibuf");
        llvm::Value* zero = typeFactory->createConstInt(32, 0);
        buf = b.CreateInBoundsGEP(
            llvm::ArrayType::get(i8, total), arr, {zero, zero}, "ibuf.ptr");
    }

    llvm::Value* off = typeFactory->createConstInt(64, 0);

    const auto& lits = literal.literalParts;
    const auto& exprs = literal.exprParts;
    for (std::size_t i = 0; i < lits.size(); ++i) {
        const std::string& seg = lits[i];
        if (!seg.empty()) {
            llvm::Value* ptr = getStringConstant(seg);
            llvm::Value* len = typeFactory->createConstInt(64, seg.size());
            off = b.CreateCall(fmtRaw_(), {buf, cap, off, ptr, len}, "loff");
        }
        if (i < exprs.size() && exprs[i]) {
            llvm::Value* v = generateExpression(exprs[i]);
            Types::TypeRef t = sema_ ? sema_->typeOf(exprs[i].get()) : nullptr;
            if (v && t) {
                off = emitFormatValue(buf, cap, off, v, t, 0);
            }
        }
    }

    b.CreateCall(fmtFinish_(), {buf, cap, off});
    return buf;
}
