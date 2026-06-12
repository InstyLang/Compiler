#pragma once


#include <string>
#include <vector>

namespace Utilities {

std::string readFile(const std::string& path);

bool writeFile(const std::string& path, const std::string& content);

std::string trim(const std::string& value);
std::vector<std::string> split(const std::string& value, char delimiter);
bool endsWith(const std::string& value, const std::string& suffix);
bool startsWith(const std::string& value, const std::string& prefix);

std::string executableDirectory();

}
