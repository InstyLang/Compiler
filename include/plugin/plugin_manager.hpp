#pragma once


#include <string>
#include <vector>

#include <plugin/plugin_interface.hpp>

namespace Plugins {

struct LoadedPlugin {
    std::string path;
    std::string name;
    std::string version;
    void* handle = nullptr;
};

class PluginManager {
public:
    ~PluginManager();

    int loadFromDirectory(const std::string& directory);
    bool loadPlugin(const std::string& path);
    const std::vector<LoadedPlugin>& plugins() const { return plugins_; }
    void unloadAll();

private:
    std::vector<LoadedPlugin> plugins_;
};

}
