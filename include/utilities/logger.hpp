#pragma once


#include <iostream>
#include <sstream>
#include <string>

namespace Logging {

enum class Level {
    Debug,
    Info,
    Warning,
    Error,
    None
};

class Logger {
public:
    static Logger& instance();

    void setLevel(Level level) { level_ = level; }
    Level level() const { return level_; }

    template <typename... Args>
    void log(Level level, const std::string& component, Args&&... args) {
        if (level < level_) {
            return;
        }
        std::ostringstream stream;
        stream << "[" << levelName(level) << "] " << component << ": ";
        (stream << ... << args);
        std::cerr << stream.str() << "\n";
    }

private:
    Logger() = default;
    Level level_ = Level::Warning;
    static const char* levelName(Level level);
};

}

#define LOG_DEBUG(component, ...) \
    ::Logging::Logger::instance().log(::Logging::Level::Debug, component, __VA_ARGS__)
#define LOG_INFO(component, ...) \
    ::Logging::Logger::instance().log(::Logging::Level::Info, component, __VA_ARGS__)
#define LOG_WARNING(component, ...) \
    ::Logging::Logger::instance().log(::Logging::Level::Warning, component, __VA_ARGS__)
#define LOG_ERROR(component, ...) \
    ::Logging::Logger::instance().log(::Logging::Level::Error, component, __VA_ARGS__)
#define LOG_FUNCTION_ENTRY(component) \
    ::Logging::Logger::instance().log(::Logging::Level::Debug, component, "enter ", __func__)
#define LOG_FUNCTION_EXIT(component) \
    ::Logging::Logger::instance().log(::Logging::Level::Debug, component, "exit ", __func__)
