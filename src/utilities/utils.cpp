#include <utilities/utils.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#if defined(__linux__)
#include <limits.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace Utilities {

std::string readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return "";
    }
    std::ostringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

bool writeFile(const std::string& path, const std::string& content) {
    fs::path target(path);
    if (target.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }
    file << content;
    return file.good();
}

std::string trim(const std::string& value) {
    auto begin = std::find_if_not(value.begin(), value.end(),
                                  [](unsigned char ch) { return std::isspace(ch); });
    auto end = std::find_if_not(value.rbegin(), value.rend(),
                                [](unsigned char ch) { return std::isspace(ch); }).base();
    if (begin >= end) {
        return "";
    }
    return std::string(begin, end);
}

std::vector<std::string> split(const std::string& value, char delimiter) {
    std::vector<std::string> parts;
    std::stringstream stream(value);
    std::string part;
    while (std::getline(stream, part, delimiter)) {
        parts.push_back(part);
    }
    return parts;
}

bool endsWith(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool startsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

std::string executableDirectory() {
#if defined(__linux__)
    char buffer[PATH_MAX];
    ssize_t count = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (count > 0) {
        buffer[count] = '\0';
        return fs::path(buffer).parent_path().string();
    }
#endif
    return fs::current_path().string();
}

}
