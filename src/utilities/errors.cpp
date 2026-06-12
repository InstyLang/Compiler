#include <utilities/errors.hpp>

#include <iostream>
#include <sstream>

namespace ErrorReporting {

std::unique_ptr<ErrorReporter> globalErrorReporter;

ErrorReporter::ErrorReporter(std::string source, std::string filename)
    : source_(std::move(source)), filename_(std::move(filename)) {}

void ErrorReporter::report(const Diagnostic& diagnostic) {
    if (diagnostic.level == ErrorLevel::Error) {
        ++errorCount_;
    }
    diagnostics_.push_back(diagnostic);
}

void ErrorReporter::report(ErrorLevel level, std::string code, std::string message,
                           SourceLocation location, std::string hint) {
    Diagnostic diagnostic;
    diagnostic.level = level;
    diagnostic.code = std::move(code);
    diagnostic.message = std::move(message);
    diagnostic.location = location;
    diagnostic.hint = std::move(hint);
    report(diagnostic);
}

void ErrorReporter::error(std::string code, std::string message, SourceLocation location,
                          std::string hint) {
    report(ErrorLevel::Error, std::move(code), std::move(message), location, std::move(hint));
}

void ErrorReporter::warning(std::string code, std::string message, SourceLocation location,
                            std::string hint) {
    report(ErrorLevel::Warning, std::move(code), std::move(message), location, std::move(hint));
}

bool ErrorReporter::hasError() const {
    return errorCount_ > 0;
}

std::string ErrorReporter::lineText(int line) const {
    if (line <= 0) {
        return "";
    }
    int current = 1;
    size_t start = 0;
    while (current < line && start < source_.size()) {
        if (source_[start] == '\n') {
            ++current;
        }
        ++start;
    }
    if (current != line) {
        return "";
    }
    size_t end = source_.find('\n', start);
    if (end == std::string::npos) {
        end = source_.size();
    }
    return source_.substr(start, end - start);
}

static const char* levelName(ErrorLevel level) {
    switch (level) {
        case ErrorLevel::Error: return "error";
        case ErrorLevel::Warning: return "warning";
        case ErrorLevel::Info: return "info";
        case ErrorLevel::Hint: return "hint";
    }
    return "error";
}

std::string ErrorReporter::format(const Diagnostic& diagnostic) const {
    std::ostringstream out;
    out << filename_;
    if (diagnostic.location.line > 0) {
        out << ":" << diagnostic.location.line << ":" << diagnostic.location.column;
    }
    out << ": " << levelName(diagnostic.level);
    if (!diagnostic.code.empty()) {
        out << "[" << diagnostic.code << "]";
    }
    out << ": " << diagnostic.message << "\n";

    const std::string snippet = lineText(diagnostic.location.line);
    if (!snippet.empty()) {
        out << "    " << snippet << "\n";
        int caretCol = diagnostic.location.column > 0 ? diagnostic.location.column : 1;
        out << "    " << std::string(static_cast<size_t>(caretCol - 1), ' ')
            << std::string(static_cast<size_t>(std::max(1, diagnostic.location.length)), '^') << "\n";
    }
    if (!diagnostic.hint.empty()) {
        out << "    hint: " << diagnostic.hint << "\n";
    }
    return out.str();
}

void ErrorReporter::printAll() const {
    for (const auto& diagnostic : diagnostics_) {
        std::cerr << format(diagnostic);
    }
}

void initErrorReporter(const std::string& source, const std::string& filename) {
    globalErrorReporter = std::make_unique<ErrorReporter>(source, filename);
}

void cleanupErrorReporter() {
    globalErrorReporter.reset();
}

void reportError(const std::string& code, const std::string& message,
                 const SourceLocation& location, const std::string& hint) {
    if (globalErrorReporter) {
        globalErrorReporter->error(code, message, location, hint);
    }
}

void reportWarning(const std::string& code, const std::string& message,
                   const SourceLocation& location, const std::string& hint) {
    if (globalErrorReporter) {
        globalErrorReporter->warning(code, message, location, hint);
    }
}

}
