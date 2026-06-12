#pragma once


#include <string>

namespace Env {

std::string get(const char* name, const std::string& fallback = "");
bool isSet(const char* name);

}
