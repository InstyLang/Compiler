#include <utilities/config.hpp>
#include <utilities/env.hpp>
#include <utilities/toml.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace Toml {

namespace {

std::string trim(std::string value) {
    auto begin = std::find_if_not(value.begin(), value.end(),
                                  [](unsigned char ch) { return std::isspace(ch); });
    auto end = std::find_if_not(value.rbegin(), value.rend(),
                                [](unsigned char ch) { return std::isspace(ch); }).base();
    if (begin >= end) {
        return "";
    }
    return std::string(begin, end);
}

std::string stripComment(const std::string& line) {
    bool inString = false;
    char quote = '\0';
    for (size_t i = 0; i < line.size(); ++i) {
        char ch = line[i];
        if ((ch == '"' || ch == '\'') && (i == 0 || line[i - 1] != '\\')) {
            if (!inString) {
                inString = true;
                quote = ch;
            } else if (quote == ch) {
                inString = false;
            }
        }
        if (ch == '#' && !inString) {
            return line.substr(0, i);
        }
    }
    return line;
}

std::string parseScalar(std::string raw) {
    raw = trim(stripComment(std::move(raw)));
    if (raw.size() >= 2) {
        char first = raw.front();
        char last = raw.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            return raw.substr(1, raw.size() - 2);
        }
    }
    return raw;
}

}

bool Table::loadFile(const std::string& path, std::string& errorMessage) {
    std::ifstream file(path);
    if (!file.is_open()) {
        errorMessage = "could not open '" + path + "'";
        return false;
    }
    std::ostringstream stream;
    stream << file.rdbuf();
    return loadString(stream.str(), errorMessage);
}

bool Table::loadString(const std::string& content, std::string& errorMessage) {
    values_.clear();
    std::stringstream stream(content);
    std::string line;
    int lineNumber = 0;
    while (std::getline(stream, line)) {
        ++lineNumber;
        line = trim(stripComment(line));
        if (line.empty()) {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            continue;
        }
        size_t equals = line.find('=');
        if (equals == std::string::npos) {
            errorMessage = "invalid line " + std::to_string(lineNumber);
            return false;
        }
        std::string key = trim(line.substr(0, equals));
        std::string value = parseScalar(line.substr(equals + 1));
        values_[key] = value;
    }
    return true;
}

bool Table::has(const std::string& key) const {
    return values_.find(key) != values_.end();
}

std::string Table::getString(const std::string& key, const std::string& fallback) const {
    auto it = values_.find(key);
    return it == values_.end() ? fallback : it->second;
}

std::optional<long long> Table::getInt(const std::string& key) const {
    auto it = values_.find(key);
    if (it == values_.end()) {
        return std::nullopt;
    }
    try {
        return std::stoll(it->second);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<bool> Table::getBool(const std::string& key) const {
    auto it = values_.find(key);
    if (it == values_.end()) {
        return std::nullopt;
    }
    std::string value = it->second;
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (value == "true") return true;
    if (value == "false") return false;
    return std::nullopt;
}

}

namespace Env {

std::string get(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return (value && *value) ? std::string(value) : fallback;
}

bool isSet(const char* name) {
    const char* value = std::getenv(name);
    return value && *value;
}

}
