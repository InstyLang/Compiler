#include <codegen/codegen.hpp>
#include <llvm/IR/InlineAsm.h>
#include <cstdint>

namespace {
constexpr uint64_t kInstantOSExitSyscallNum = 2;
}

void CodeGenerator::generatePlatformRuntimeFunctions() {
    if (!runtimeOptions_.emitPlatformStart) {
        return;
    }

    if (targetSpec_.isInstantOS) {
        generateChkstkFunction();
    }

    generateStartFunction();
}

llvm::Function* CodeGenerator::generateChkstkFunction() {
    if (llvm::Function* existing = module->getFunction("__chkstk")) {
        return existing;
    }

    llvm::FunctionType* chkstkTy = llvm::FunctionType::get(typeFactory->createVoid(), {}, false);
    llvm::Function* chkstkFunc = llvm::Function::Create(
        chkstkTy,
        llvm::Function::LinkOnceODRLinkage,
        "__chkstk",
        module.get()
    );
    chkstkFunc->setComdat(module->getOrInsertComdat("__chkstk"));

    llvm::BasicBlock* entry = llvm::BasicBlock::Create(*context, "entry", chkstkFunc);
    llvm::IRBuilder<> b(entry);
    b.CreateRetVoid();

    return chkstkFunc;
}

llvm::Function* CodeGenerator::generateStartFunction() {
    if (moduleCtx.moduleName != "main") {
        return nullptr;
    }

    if (!moduleCtx.cimportParsers.empty()) {
        return nullptr;
    }

    const std::string& entryName = targetSpec_.entrySymbol;

    if (targetSpec_.isEfi) {
        llvm::Function* entryFunc = module->getFunction(entryName);
        if (entryFunc) {
            return entryFunc;
        }

        llvm::Type* handleTy = typeFactory->createPointer();
        llvm::Type* systemTableTy = typeFactory->createPointer();
        llvm::Type* statusTy = typeFactory->createInt(64);

        llvm::FunctionType* efiType = llvm::FunctionType::get(
            statusTy,
            {handleTy, systemTableTy},
            false
        );

        entryFunc = llvm::Function::Create(
            efiType,
            llvm::Function::ExternalLinkage,
            entryName,
            module.get()
        );

        auto argIt = entryFunc->arg_begin();
        llvm::Value* imageHandle = argIt++;
        imageHandle->setName("image_handle");
        llvm::Value* systemTable = argIt++;
        systemTable->setName("system_table");

        llvm::BasicBlock* entry = llvm::BasicBlock::Create(*context, "entry", entryFunc);
        llvm::IRBuilder<> b(entry);

        llvm::Function* mainFunc = module->getFunction("main");
        if (!mainFunc) {
            b.CreateRet(typeFactory->createConstInt(64, 1));
            return entryFunc;
        }

        llvm::Value* result = nullptr;
        if (mainFunc->arg_size() == 2) {
            result = b.CreateCall(mainFunc, {imageHandle, systemTable}, "efi_status");
        } else if (mainFunc->arg_size() == 0) {
            result = b.CreateCall(mainFunc, {}, "efi_status");
        } else {
            b.CreateRet(typeFactory->createConstInt(64, 2));
            return entryFunc;
        }

        if (mainFunc->getReturnType()->isIntegerTy(64)) {
            b.CreateRet(result);
        } else if (mainFunc->getReturnType()->isIntegerTy(32)) {
            b.CreateRet(b.CreateZExt(result, statusTy));
        } else if (mainFunc->getReturnType()->isVoidTy()) {
            b.CreateRet(typeFactory->createConstInt(64, 0));
        } else {
            b.CreateRet(typeFactory->createConstInt(64, 3));
        }

        return entryFunc;
    }

    if (targetSpec_.isInstantOS) {
        return generateInstantOSStartFunction();
    }

    if (!targetSpec_.supportsLinuxSyscallRuntime()) {
        return nullptr;
    }

    llvm::Function* startFunc = module->getFunction(entryName);
    if (startFunc) {
        return startFunc;
    }

    llvm::FunctionType* startType = llvm::FunctionType::get(
        typeFactory->createVoid(),
        {},
        false
    );

    startFunc = llvm::Function::Create(
        startType,
        llvm::Function::ExternalLinkage,
        entryName,
        module.get()
    );

    llvm::BasicBlock* entry = llvm::BasicBlock::Create(*context, "entry", startFunc);
    llvm::BasicBlock* setupStack = llvm::BasicBlock::Create(*context, "setup_stack", startFunc);
    llvm::BasicBlock* callMain = llvm::BasicBlock::Create(*context, "call_main", startFunc);
    llvm::BasicBlock* exitBlock = llvm::BasicBlock::Create(*context, "exit", startFunc);

    llvm::IRBuilder<> tmpBuilder(entry);

    llvm::FunctionType* stackSetupType = llvm::FunctionType::get(
        typeFactory->createVoid(),
        {},
        false
    );

    llvm::InlineAsm* stackSetup = llvm::InlineAsm::get(
        stackSetupType,
        "xor %rbp, %rbp\n\t"
        "and $$-16, %rsp",
        "~{rbp},~{rsp},~{dirflag},~{fpsr},~{flags}",
        true
    );

    tmpBuilder.CreateCall(stackSetup);
    tmpBuilder.CreateBr(setupStack);

    tmpBuilder.SetInsertPoint(setupStack);
    tmpBuilder.CreateBr(callMain);

    tmpBuilder.SetInsertPoint(callMain);

    llvm::Function* mainFunc = module->getFunction("main");
    if (!mainFunc) {
        llvm::FunctionType* syscallType = llvm::FunctionType::get(
            typeFactory->createInt(64),
            {typeFactory->createInt(64), typeFactory->createInt(64)},
            false
        );

        llvm::InlineAsm* exitSyscall = llvm::InlineAsm::get(
            syscallType,
            "syscall",
            "={rax},{rax},{rdi},~{rcx},~{r11}",
            true
        );

        tmpBuilder.CreateCall(
            exitSyscall,
            {typeFactory->createConstInt(64, 60), typeFactory->createConstInt(64, 1)}
        );

        tmpBuilder.CreateUnreachable();
        return startFunc;
    }

    llvm::Value* exitCode = tmpBuilder.CreateCall(mainFunc);

    llvm::Value* exitCode64;
    if (exitCode->getType()->isIntegerTy(32)) {
        exitCode64 = tmpBuilder.CreateSExt(exitCode, typeFactory->createInt(64));
    } else if (exitCode->getType()->isIntegerTy(64)) {
        exitCode64 = exitCode;
    } else {
        exitCode64 = typeFactory->createConstInt(64, 0);
    }

    llvm::Value* isNegative = tmpBuilder.CreateICmpSLT(exitCode64, typeFactory->createConstInt(64, 0));
    llvm::Value* clamped = tmpBuilder.CreateSelect(isNegative, typeFactory->createConstInt(64, 0), exitCode64);

    llvm::Value* isTooBig = tmpBuilder.CreateICmpSGT(clamped, typeFactory->createConstInt(64, 255));
    exitCode64 = tmpBuilder.CreateSelect(isTooBig, typeFactory->createConstInt(64, 255), clamped);

    tmpBuilder.CreateBr(exitBlock);

    tmpBuilder.SetInsertPoint(exitBlock);

    llvm::FunctionType* syscallType = llvm::FunctionType::get(
        typeFactory->createInt(64),
        {typeFactory->createInt(64), typeFactory->createInt(64)},
        false
    );

    llvm::InlineAsm* exitSyscall = llvm::InlineAsm::get(
        syscallType,
        "syscall",
        "={rax},{rax},{rdi},~{rcx},~{r11}",
        true
    );

    tmpBuilder.CreateCall(
        exitSyscall,
        {typeFactory->createConstInt(64, 60), exitCode64}
    );

    tmpBuilder.CreateUnreachable();

    return startFunc;
}

llvm::Function* CodeGenerator::generateInstantOSStartFunction() {
    const std::string& entryName = targetSpec_.entrySymbol;

    llvm::Function* entryFunc = module->getFunction(entryName);
    if (entryFunc) {
        return entryFunc;
    }

    llvm::FunctionType* entryType = llvm::FunctionType::get(
        typeFactory->createVoid(),
        {},
        false
    );

    entryFunc = llvm::Function::Create(
        entryType,
        llvm::Function::ExternalLinkage,
        entryName,
        module.get()
    );

    llvm::BasicBlock* entry = llvm::BasicBlock::Create(*context, "entry", entryFunc);
    llvm::IRBuilder<> b(entry);

    llvm::Value* exitCode = typeFactory->createConstInt(64, 1);
    llvm::Function* mainFunc = module->getFunction("main");
    if (mainFunc) {
        if (mainFunc->arg_size() != 0) {
            exitCode = typeFactory->createConstInt(64, 2);
        } else if (mainFunc->getReturnType()->isVoidTy()) {
            b.CreateCall(mainFunc, {});
            exitCode = typeFactory->createConstInt(64, 0);
        } else {
            llvm::Value* result = b.CreateCall(mainFunc, {}, "main_result");
            if (mainFunc->getReturnType()->isIntegerTy(64)) {
                exitCode = result;
            } else if (mainFunc->getReturnType()->isIntegerTy(32)) {
                exitCode = b.CreateSExt(result, typeFactory->createInt(64), "exit_code");
            }
        }
    }

    llvm::FunctionType* syscallType = llvm::FunctionType::get(
        typeFactory->createInt(64),
        {typeFactory->createInt(64), typeFactory->createInt(64)},
        false
    );

    llvm::InlineAsm* exitSyscall = llvm::InlineAsm::get(
        syscallType,
        "syscall",
        "={rax},{rax},{rbx},~{rcx},~{r11},~{memory}",
        true
    );

    b.CreateCall(
        exitSyscall,
        {typeFactory->createConstInt(64, kInstantOSExitSyscallNum), exitCode}
    );
    b.CreateUnreachable();

    return entryFunc;
}
