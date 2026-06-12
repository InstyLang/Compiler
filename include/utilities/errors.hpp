#pragma once


#include <memory>
#include <string>
#include <vector>

namespace ErrorReporting {

enum class ErrorLevel {
    Error,
    Warning,
    Info,
    Hint
};

struct SourceLocation {
    int line = 0;
    int column = 0;
    int length = 1;
    int offset = -1;
};

struct Diagnostic {
    std::string code;
    std::string message;
    std::string hint;
    ErrorLevel level = ErrorLevel::Error;
    SourceLocation location;
};

class ErrorReporter {
public:
    ErrorReporter(std::string source, std::string filename);

    void report(const Diagnostic& diagnostic);
    void report(ErrorLevel level, std::string code, std::string message,
                SourceLocation location, std::string hint = "");

    void error(std::string code, std::string message, SourceLocation location, std::string hint = "");
    void warning(std::string code, std::string message, SourceLocation location, std::string hint = "");

    bool hasError() const;
    bool hasDiagnostics() const { return !diagnostics_.empty(); }
    const std::vector<Diagnostic>& getDiagnostics() const { return diagnostics_; }
    void clear() { diagnostics_.clear(); errorCount_ = 0; }

    const std::string& filename() const { return filename_; }
    const std::string& source() const { return source_; }

    std::string format(const Diagnostic& diagnostic) const;
    void printAll() const;

private:
    std::string source_;
    std::string filename_;
    std::vector<Diagnostic> diagnostics_;
    int errorCount_ = 0;

    std::string lineText(int line) const;
};

extern std::unique_ptr<ErrorReporter> globalErrorReporter;

void initErrorReporter(const std::string& source, const std::string& filename);
void cleanupErrorReporter();

void reportError(const std::string& code, const std::string& message,
                 const SourceLocation& location, const std::string& hint = "");
void reportWarning(const std::string& code, const std::string& message,
                   const SourceLocation& location, const std::string& hint = "");

}
