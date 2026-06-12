#pragma once


#include <llvm/IR/Module.h>

namespace Optimizer {

bool optimizeModule(llvm::Module& module, int optLevel);

}
