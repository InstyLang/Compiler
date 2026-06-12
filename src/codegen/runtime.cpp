#include <codegen/codegen.hpp>


void CodeGenerator::generateRuntimeFunctions() {
    generateCoreRuntimeFunctions();
    generateAllocatorRuntimeFunctions();
    generateHostedStdRuntimeFunctions();
    generatePlatformRuntimeFunctions();
}
