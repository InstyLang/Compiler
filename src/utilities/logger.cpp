#include <utilities/logger.hpp>

namespace Logging {

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

const char* Logger::levelName(Level level) {
    switch (level) {
        case Level::Debug: return "DEBUG";
        case Level::Info: return "INFO";
        case Level::Warning: return "WARN";
        case Level::Error: return "ERROR";
        case Level::None: return "NONE";
    }
    return "INFO";
}

}
