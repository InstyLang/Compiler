#pragma once


#include <optional>
#include <string>
#include <unordered_map>

namespace Toml {

class Table {
public:
    bool loadFile(const std::string& path, std::string& errorMessage);
    bool loadString(const std::string& content, std::string& errorMessage);

    bool has(const std::string& key) const;
    std::string getString(const std::string& key, const std::string& fallback = "") const;
    std::optional<long long> getInt(const std::string& key) const;
    std::optional<bool> getBool(const std::string& key) const;

    const std::unordered_map<std::string, std::string>& values() const { return values_; }

private:
    std::unordered_map<std::string, std::string> values_;
};

}
