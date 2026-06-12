#pragma once

#include <string>

namespace Runtime {

enum class AllocatorMode {
    None,
    Runtime,
    External
};

enum class PanicStrategy {
    Abort,
    Handler
};

struct Options {
    bool freestanding = false;
    bool emitPlatformStart = true;
    AllocatorMode allocatorMode = AllocatorMode::None;
    PanicStrategy panicStrategy = PanicStrategy::Abort;
    std::string panicHandler;

    bool allowsHeap() const {
        return allocatorMode != AllocatorMode::None;
    }

    bool emitsAllocatorRuntime() const {
        return allocatorMode == AllocatorMode::Runtime;
    }

    bool usesExternalAllocator() const {
        return allocatorMode == AllocatorMode::External;
    }
};

}
