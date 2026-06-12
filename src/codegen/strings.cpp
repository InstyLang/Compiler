#include <codegen/codegen.hpp>

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>

llvm::Value* CodeGenerator::getStringConstant(const std::string& text) {
    if (auto it = stringConstants_.find(text); it != stringConstants_.end()) {
        llvm::GlobalVariable* gv = it->second;
        llvm::Value* zero = typeFactory->createConstInt(32, 0);
        llvm::Value* indices[] = {zero, zero};
        return builder->CreateInBoundsGEP(gv->getValueType(), gv, indices, "str");
    }

    llvm::LLVMContext& ctx = *context;

    llvm::Constant* dataArray =
        llvm::ConstantDataArray::getString(ctx, text, true);
    llvm::Type* arrayTy = dataArray->getType();

    auto* gv = new llvm::GlobalVariable(
        *module, arrayTy, true,
        llvm::GlobalValue::PrivateLinkage, dataArray,
        ".str." + std::to_string(stringConstants_.size()));
    gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    gv->setAlignment(llvm::Align(1));

    stringConstants_[text] = gv;

    llvm::Value* zero = typeFactory->createConstInt(32, 0);
    llvm::Value* indices[] = {zero, zero};
    return builder->CreateInBoundsGEP(arrayTy, gv, indices, "str");
}

llvm::Value* CodeGenerator::getWideStringConstant(const std::string& text) {
    if (auto it = wideStringConstants_.find(text); it != wideStringConstants_.end()) {
        llvm::GlobalVariable* gv = it->second;
        llvm::Value* zero = typeFactory->createConstInt(32, 0);
        llvm::Value* indices[] = {zero, zero};
        return builder->CreateInBoundsGEP(gv->getValueType(), gv, indices, "wstr");
    }

    llvm::LLVMContext& ctx = *context;
    llvm::Type* i16Ty = typeFactory->createInt(16);

    std::vector<llvm::Constant*> units;
    units.reserve(text.size() + 1);
    for (unsigned char c : text) {
        units.push_back(llvm::ConstantInt::get(i16Ty, static_cast<uint16_t>(c)));
    }
    units.push_back(llvm::ConstantInt::get(i16Ty, 0));

    llvm::ArrayType* arrayTy = llvm::ArrayType::get(i16Ty, units.size());
    llvm::Constant* dataArray = llvm::ConstantArray::get(arrayTy, units);

    auto* gv = new llvm::GlobalVariable(
        *module, arrayTy, true,
        llvm::GlobalValue::PrivateLinkage, dataArray,
        ".wstr." + std::to_string(wideStringConstants_.size()));
    gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    gv->setAlignment(llvm::Align(2));

    wideStringConstants_[text] = gv;

    llvm::Value* zero = typeFactory->createConstInt(32, 0);
    llvm::Value* indices[] = {zero, zero};
    return builder->CreateInBoundsGEP(arrayTy, gv, indices, "wstr");
}
