#include <optimizer/optimizer.hpp>

#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/OptimizationLevel.h>

namespace Optimizer {

bool optimizeModule(llvm::Module& module, int optLevel) {
    if (optLevel <= 0) {
        return true;
    }

    llvm::PassBuilder passBuilder;
    llvm::LoopAnalysisManager loopAM;
    llvm::FunctionAnalysisManager functionAM;
    llvm::CGSCCAnalysisManager cgsccAM;
    llvm::ModuleAnalysisManager moduleAM;

    passBuilder.registerModuleAnalyses(moduleAM);
    passBuilder.registerCGSCCAnalyses(cgsccAM);
    passBuilder.registerFunctionAnalyses(functionAM);
    passBuilder.registerLoopAnalyses(loopAM);
    passBuilder.crossRegisterProxies(loopAM, functionAM, cgsccAM, moduleAM);

    llvm::OptimizationLevel level = llvm::OptimizationLevel::O2;
    switch (optLevel) {
        case 1: level = llvm::OptimizationLevel::O1; break;
        case 2: level = llvm::OptimizationLevel::O2; break;
        default: level = llvm::OptimizationLevel::O3; break;
    }

    llvm::ModulePassManager mpm = passBuilder.buildPerModuleDefaultPipeline(level);
    mpm.run(module, moduleAM);
    return true;
}

}
