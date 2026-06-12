#include <plugin/plugin_manager.hpp>

#include <filesystem>

#include <dlfcn.h>

namespace fs = std::filesystem;

namespace Plugins {

PluginManager::~PluginManager() {
    unloadAll();
}

bool PluginManager::loadPlugin(const std::string& path) {
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        return false;
    }

    auto infoFn = reinterpret_cast<EcxPluginInfoFn>(dlsym(handle, "ecx_plugin_info"));
    if (!infoFn) {
        dlclose(handle);
        return false;
    }

    const EcxPluginInfo* info = infoFn();
    if (!info || info->abiVersion != INSTY_PLUGIN_ABI_VERSION) {
        dlclose(handle);
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
        if (ext == ".so" || ext == ".dylib") {
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
            dlclose(plugin.handle);
            plugin.handle = nullptr;
        }
    }
    plugins_.clear();
}

}
