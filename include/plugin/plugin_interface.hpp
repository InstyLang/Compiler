#pragma once


#include <cstdint>

extern "C" {

#define INSTY_PLUGIN_ABI_VERSION 1u

struct EcxPluginInfo {
    uint32_t abiVersion;
    const char* name;
    const char* version;
};

typedef const EcxPluginInfo* (*EcxPluginInfoFn)(void);

}
