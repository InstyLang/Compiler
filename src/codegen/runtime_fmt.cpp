#include <codegen/codegen.hpp>

#include <llvm/IR/Function.h>


void CodeGenerator::generateFormatRuntimeFunctions() {
}

llvm::Function* CodeGenerator::fmtRaw_() {
    return emitCoreFunction("__ins_fmt_raw");
}

llvm::Function* CodeGenerator::fmtInt_() {
    return emitCoreFunction("__ins_fmt_int");
}

llvm::Function* CodeGenerator::fmtFloat_() {
    return emitCoreFunction("__ins_fmt_float");
}

llvm::Function* CodeGenerator::fmtFinish_() {
    return emitCoreFunction("__ins_fmt_finish");
}
