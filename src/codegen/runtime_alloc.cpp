#include <codegen/codegen.hpp>
#include <cstdint>


void CodeGenerator::generateAllocatorRuntimeFunctions() {
    if (!runtimeOptions_.emitsAllocatorRuntime()) {
        return;
    }

    if (!targetSpec_.supportsLinuxSyscallRuntime() && !targetSpec_.isInstantOS) {
        reportCodegenError(
            "E40030",
            "the built-in allocator runtime is only available for Linux and InstantOS targets"
        );
        return;
    }

    generateMallocFunction();
    generateFreeFunction();
    generateReallocFunction();
}

llvm::Function* CodeGenerator::generateMallocFunction() {
    if (llvm::Function* existing = module->getFunction("__ins_alloc")) {
        if (!existing->empty() || runtimeOptions_.usesExternalAllocator()) {
            return existing;
        }
    }

    if (runtimeOptions_.usesExternalAllocator()) {
        llvm::FunctionType* externalType = llvm::FunctionType::get(
            typeFactory->createPointer(),
            {typeFactory->createInt(64), typeFactory->createInt(64)},
            false
        );
        return llvm::Function::Create(
            externalType,
            llvm::Function::ExternalLinkage,
            "__ins_alloc",
            module.get()
        );
    }
    if (!runtimeOptions_.emitsAllocatorRuntime()) {
        reportCodegenError("E40046", "allocator support is disabled");
        return nullptr;
    }

    const char* backend =
        targetSpec_.isInstantOS ? "core_alloc_mmap" : "core_alloc_brk";
    return emitCoreFunction(backend, "__ins_alloc");
}

llvm::Function* CodeGenerator::generateFreeFunction() {
    if (llvm::Function* existing = module->getFunction("__ins_free")) {
        if (!existing->empty() || runtimeOptions_.usesExternalAllocator()) {
            return existing;
        }
    }

    if (runtimeOptions_.usesExternalAllocator()) {
        llvm::FunctionType* externalType = llvm::FunctionType::get(
            typeFactory->createVoid(),
            {typeFactory->createPointer(), typeFactory->createInt(64), typeFactory->createInt(64)},
            false
        );
        return llvm::Function::Create(
            externalType,
            llvm::Function::ExternalLinkage,
            "__ins_free",
            module.get()
        );
    }
    if (!runtimeOptions_.emitsAllocatorRuntime()) {
        reportCodegenError("E40047", "allocator support is disabled");
        return nullptr;
    }

    return emitCoreFunction("__ins_free");
}

llvm::Function* CodeGenerator::generateReallocFunction() {
    if (llvm::Function* existing = module->getFunction("__ins_realloc")) {
        if (!existing->empty() || runtimeOptions_.usesExternalAllocator()) {
            return existing;
        }
    }

    if (runtimeOptions_.usesExternalAllocator()) {
        llvm::FunctionType* externalType = llvm::FunctionType::get(
            typeFactory->createPointer(),
            {typeFactory->createPointer(), typeFactory->createInt(64), typeFactory->createInt(64)},
            false
        );
        return llvm::Function::Create(
            externalType,
            llvm::Function::ExternalLinkage,
            "__ins_realloc",
            module.get()
        );
    }
    if (!runtimeOptions_.emitsAllocatorRuntime()) {
        reportCodegenError("E40048", "allocator support is disabled");
        return nullptr;
    }

    generateMallocFunction();
    generateFreeFunction();
    return emitCoreFunction("__ins_realloc");
}
