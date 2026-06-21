#include <plugin/plugin_manager.hpp>

#include <filesystem>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace fs = std::filesystem;

namespace {

// Thin cross-platform wrappers over the dynamic-loading APIs. POSIX uses
// dlopen/dlsym/dlclose; Windows uses LoadLibrary/GetProcAddress/FreeLibrary.
void* openLibrary(const std::string& path) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(LoadLibraryA(path.c_str()));
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void* findSymbol(void* handle, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(
        GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
#else
    return dlsym(handle, name);
#endif
}

void closeLibrary(void* handle) {
#if defined(_WIN32)
    FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}

} // namespace

namespace Plugins {

PluginManager::~PluginManager() {
    unloadAll();
}

bool PluginManager::loadPlugin(const std::string& path) {
    void* handle = openLibrary(path);
    if (!handle) {
        return false;
    }

    auto infoFn = reinterpret_cast<EcxPluginInfoFn>(findSymbol(handle, "ecx_plugin_info"));
    if (!infoFn) {
        closeLibrary(handle);
        return false;
    }

    const EcxPluginInfo* info = infoFn();
    if (!info || info->abiVersion != INSTY_PLUGIN_ABI_VERSION) {
        closeLibrary(handle);
        return false;
    }

    LoadedPlugin plugin;
    plugin.path = path;
    plugin.name = info->name ? info->name : "";
    plugin.version = info->version ? info->version : "";
    plugin.handle = handle;
    plugins_.push_back(plugin);
    return true;
}

int PluginManager::loadFromDirectory(const std::string& directory) {
    if (!fs::is_directory(directory)) {
        return 0;
    }
    int loaded = 0;
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string ext = entry.path().extension().string();
        if (ext == ".so" || ext == ".dylib" || ext == ".dll") {
            if (loadPlugin(entry.path().string())) {
                ++loaded;
            }
        }
    }
    return loaded;
}

void PluginManager::unloadAll() {
    for (auto& plugin : plugins_) {
        if (plugin.handle) {
            closeLibrary(plugin.handle);
            plugin.handle = nullptr;
        }
    }
    plugins_.clear();
}

}
