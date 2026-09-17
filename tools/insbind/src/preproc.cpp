#include "preproc.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "lexer.h"
#include "macro.h"
#include "ppeval.h"

namespace insbind {
namespace {

namespace fs = std::filesystem;

// Translation phase 2: each backslash-newline (or backslash-CRLF) is deleted,
// splicing physical lines into logical ones before tokenization.
std::string spliceLines(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    for (std::size_t i = 0; i < src.size(); ++i) {
        if (src[i] == '\\' && i + 1 < src.size()) {
            if (src[i + 1] == '\n') { ++i; continue; }
            if (src[i + 1] == '\r' && i + 2 < src.size() && src[i + 2] == '\n') {
                i += 2;
                continue;
            }
        }
        out += src[i];
    }
    return out;
}

std::string displayName(const std::string& path) {
    return fs::path(path).filename().string();
}

struct CondFrame {
    bool parentActive = false; // all enclosing groups active when pushed
    bool taken = false;        // some branch of this group already taken
    bool sawElse = false;
    bool branchActive = false; // current branch active (ignoring parents)
};

class Preprocessor {
public:
    explicit Preprocessor(PreprocessOptions opts) : opts_(std::move(opts)) {}

    PreprocessResult run() {
        installPredefines();
        for (const auto& [name, value] : opts_.defines)
            defineObject(name, value, "<command line>");
        if (!fs::exists(fs::path(opts_.mainFile))) {
            result_.errors.push_back(opts_.mainFile + ": file not found");
            return result_;
        }
        processFile(opts_.mainFile, 0);
        return result_;
    }

private:
    PreprocessOptions opts_;
    PreprocessResult result_;
    MacroTable macros_;
    std::vector<std::string> includeStack_; // canonical paths, cycle guard
    std::vector<std::string> pragmaOnce_;   // canonical paths

    std::uint32_t internFile(const std::string& path) {
        for (std::uint32_t i = 0; i < result_.files.size(); ++i)
            if (result_.files[i] == path) return i;
        result_.files.push_back(path);
        return static_cast<std::uint32_t>(result_.files.size() - 1);
    }

    void defineObject(const std::string& name, const std::string& value,
                      const std::string& where) {
        std::vector<Token> rep = lex(value);
        rep.erase(std::remove_if(rep.begin(), rep.end(),
                                 [](const Token& t) {
                                     return t.kind == TokenKind::End;
                                 }),
                  rep.end());
        Macro m;
        m.replacement = rep;
        const std::string warning = macros_.define(name, std::move(m));
        if (!warning.empty()) result_.warnings.push_back(where + ": " + warning);
    }

    void installPredefines() {
        defineObject("__STDC__", "1", "<builtin>");
        defineObject("__STDC_VERSION__", "201112L", "<builtin>");
        if (opts_.target.find("windows") != std::string::npos) {
            defineObject("_WIN32", "1", "<builtin>");
            defineObject("_WIN64", "1", "<builtin>");
            defineObject("_M_X64", "100", "<builtin>");
        } else if (opts_.target.find("linux") != std::string::npos) {
            defineObject("__linux__", "1", "<builtin>");
            defineObject("__unix__", "1", "<builtin>");
            defineObject("__x86_64__", "1", "<builtin>");
        }
    }

    // Resolves an include operand to a canonical path, or "" if not found.
    std::string resolveInclude(const std::string& name, bool angled,
                               const std::string& currentDir) const {
        auto tryBase = [&](const std::string& base) -> std::string {
            std::error_code ec;
            const fs::path candidate =
                fs::weakly_canonical(fs::path(base) / name, ec);
            if (!ec && fs::exists(candidate)) return candidate.string();
            return {};
        };
        if (!angled) {
            const std::string hit = tryBase(currentDir);
            if (!hit.empty()) return hit;
        }
        for (const std::string& base : opts_.includePaths) {
            const std::string hit = tryBase(base);
            if (!hit.empty()) return hit;
        }
        return {};
    }

    void processFile(const std::string& path, unsigned depth) {
        std::error_code ec;
        const std::string canonical =
            fs::weakly_canonical(fs::path(path), ec).string();
        const std::string& key = ec ? path : canonical;

        for (const std::string& active : includeStack_) {
            if (active == key) {
                result_.errors.push_back(displayName(path) +
                                         ": include cycle detected");
                return;
            }
        }
        for (const std::string& once : pragmaOnce_)
            if (once == key) return;

        if (depth > 100) {
            result_.errors.push_back(displayName(path) +
                                     ": include depth limit exceeded");
            return;
        }

        std::ifstream in(key, std::ios::binary);
        if (!in) {
            result_.errors.push_back(displayName(path) + ": cannot open");
            return;
        }
        std::ostringstream ss;
        ss << in.rdbuf();

        std::vector<Token> tokens = lex(spliceLines(ss.str()));
        // Lex errors are reported and dropped so directive parsing sees a
        // clean stream. The End token is dropped too: it would leak into the
        // output through the last line's expansion and truncate every file
        // that included this one (the parser reads the merged stream).
        tokens.erase(std::remove_if(tokens.begin(), tokens.end(),
                                    [&](const Token& t) {
                                        if (t.kind == TokenKind::End) return true;
                                        if (t.kind == TokenKind::Error) {
                                            result_.errors.push_back(
                                                displayName(key) + ":" +
                                                std::to_string(t.line) + ": " +
                                                t.spelling);
                                            return true;
                                        }
                                        return false;
                                    }),
                     tokens.end());

        includeStack_.push_back(key);
        processTokens(tokens, internFile(key),
                      fs::path(key).parent_path().string(), depth);
        includeStack_.pop_back();
    }

    void processTokens(const std::vector<Token>& tokens, std::uint32_t fileIdx,
                       const std::string& dir, unsigned depth) {
        const std::string disp = displayName(result_.files[fileIdx]);
        Expander expander(macros_, disp, result_.errors);

        IfEvalContext ifCtx{macros_,
                            expander,
                            [&](const std::string& name, bool angled) {
                                return !resolveInclude(name, angled, dir).empty();
                            },
                            result_.errors,
                            ""};

        // Group into logical lines on atLineStart (first token always starts
        // one), then walk directives with a per-file conditional stack.
        std::vector<CondFrame> conds;
        auto currentActive = [&] {
            for (const CondFrame& f : conds)
                if (!f.branchActive) return false;
            return true;
        };

        std::size_t i = 0;
        while (i < tokens.size()) {
            std::size_t j = i + 1;
            while (j < tokens.size() && !tokens[j].atLineStart) ++j;
            std::vector<Token> line(tokens.begin() + static_cast<long long>(i),
                                    tokens.begin() + static_cast<long long>(j));
            i = j;
            if (line.empty()) continue;

            const bool isDirective = line[0].kind == TokenKind::Hash;
            if (!isDirective) {
                if (!currentActive()) continue;
                std::vector<Token> expanded = expander.expandLine(line);
                for (Token& t : expanded) t.file = fileIdx;
                if (!expanded.empty()) expanded.front().atLineStart = true;
                result_.tokens.insert(result_.tokens.end(), expanded.begin(),
                                      expanded.end());
                continue;
            }

            const std::string name =
                line.size() > 1 ? line[1].spelling : std::string();
            std::vector<Token> rest;
            if (line.size() > 2) rest.assign(line.begin() + 2, line.end());
            const std::string where =
                disp + ":" + std::to_string(line[0].line);

            const bool isCond = name == "if" || name == "ifdef" ||
                                name == "ifndef" || name == "elif" ||
                                name == "else" || name == "endif";
            if (!isCond && !currentActive()) continue;

            if (name == "if" || name == "ifdef" || name == "ifndef") {
                CondFrame f;
                f.parentActive = currentActive();
                bool cond = false;
                if (f.parentActive) {
                    if (name == "if") {
                        ifCtx.location = where;
                        cond = evalIf(rest, ifCtx);
                    } else {
                        if (rest.size() != 1 ||
                            rest[0].kind != TokenKind::Identifier) {
                            result_.errors.push_back(
                                where + ": malformed #" + name);
                        } else {
                            cond = macros_.isDefined(rest[0].spelling);
                            if (name == "ifndef") cond = !cond;
                        }
                    }
                }
                f.taken = cond || !f.parentActive;
                f.branchActive = cond;
                conds.push_back(f);
            } else if (name == "elif") {
                if (conds.empty()) {
                    result_.errors.push_back(where + ": #elif without #if");
                } else if (conds.back().sawElse) {
                    result_.errors.push_back(where + ": #elif after #else");
                } else {
                    CondFrame& f = conds.back();
                    if (!f.parentActive || f.taken) {
                        f.branchActive = false;
                    } else {
                        ifCtx.location = where;
                        const bool cond = evalIf(rest, ifCtx);
                        f.taken = f.taken || cond;
                        f.branchActive = cond;
                    }
                }
            } else if (name == "else") {
                if (conds.empty()) {
                    result_.errors.push_back(where + ": #else without #if");
                } else if (conds.back().sawElse) {
                    result_.errors.push_back(where + ": duplicate #else");
                } else {
                    CondFrame& f = conds.back();
                    f.sawElse = true;
                    f.branchActive = f.parentActive && !f.taken;
                    f.taken = true;
                }
            } else if (name == "endif") {
                if (conds.empty())
                    result_.errors.push_back(where + ": #endif without #if");
                else
                    conds.pop_back();
            } else if (name == "define") {
                handleDefine(rest, where, fileIdx);
            } else if (name == "undef") {
                if (rest.size() == 1 && rest[0].kind == TokenKind::Identifier)
                    macros_.undef(rest[0].spelling);
                else
                    result_.errors.push_back(where + ": malformed #undef");
            } else if (name == "include") {
                handleInclude(rest, where, dir, depth, disp);
            } else if (name == "pragma") {
                if (!rest.empty() && rest[0].spelling == "once")
                    pragmaOnce_.push_back(result_.files[fileIdx]);
                // All other pragmas are ignored (incl. pack -- see layout stage).
            } else if (name == "error") {
                result_.errors.push_back(where + ": #error " + joinSpellings(rest));
            } else if (name == "warning") {
                result_.warnings.push_back(where + ": #warning " +
                                           joinSpellings(rest));
            } else if (name == "line" || name.empty()) {
                // #line and the null directive are accepted and ignored.
            } else {
                result_.warnings.push_back(where + ": unknown directive #" +
                                           name + " (ignored)");
            }
        }

        if (!conds.empty())
            result_.errors.push_back(disp + ": unterminated #if at end of file");
    }

    static std::string joinSpellings(const std::vector<Token>& tokens) {
        std::string out;
        bool first = true;
        for (const Token& t : tokens) {
            if (!first) out += ' ';
            first = false;
            out += t.spelling;
        }
        return out;
    }

    void handleDefine(const std::vector<Token>& rest, const std::string& where,
                      std::uint32_t fileIdx) {
        if (rest.empty() || rest[0].kind != TokenKind::Identifier) {
            result_.errors.push_back(where + ": malformed #define");
            return;
        }
        Macro m;
        std::size_t pos = 1;
        // Function-like only when '(' follows the name with NO whitespace.
        if (pos < rest.size() && rest[pos].kind == TokenKind::LParen &&
            !rest[pos].leadingSpace) {
            m.functionLike = true;
            ++pos;
            bool expectParam = true;
            while (pos < rest.size() && rest[pos].kind != TokenKind::RParen) {
                const Token& t = rest[pos];
                if (expectParam && t.kind == TokenKind::Identifier) {
                    m.params.push_back(t.spelling);
                    expectParam = false;
                } else if (expectParam && t.kind == TokenKind::Ellipsis) {
                    m.variadic = true;
                    expectParam = false;
                } else if (!expectParam && t.kind == TokenKind::Comma) {
                    expectParam = true;
                } else {
                    result_.errors.push_back(where +
                                             ": malformed macro parameter list");
                    return;
                }
                ++pos;
            }
            if (pos >= rest.size()) {
                result_.errors.push_back(where +
                                         ": unterminated macro parameter list");
                return;
            }
            ++pos; // past ')'
        }
        m.replacement.assign(rest.begin() + static_cast<long long>(pos),
                             rest.end());
        const std::string warning =
            macros_.define(rest[0].spelling, std::move(m));
        if (!warning.empty())
            result_.warnings.push_back(where + ": " + warning);
        // Export main-file object-like macros for the bind stage (constants
        // become accessor functions). fileIdx 0 is the main file.
        if (fileIdx == 0) {
            const Macro* defined = macros_.find(rest[0].spelling);
            if (defined && !defined->functionLike)
                result_.objectMacros.emplace_back(rest[0].spelling,
                                                  defined->replacement);
        }
    }

    void handleInclude(std::vector<Token> rest, const std::string& where,
                       const std::string& dir, unsigned depth,
                       const std::string& disp) {
        Expander expander(macros_, disp, result_.errors);
        std::string name;
        bool angled = false;
        for (int attempt = 0; attempt < 2 && name.empty(); ++attempt) {
            if (!rest.empty() && rest[0].kind == TokenKind::StringLiteral) {
                const std::string& s = rest[0].spelling;
                name = s.size() >= 2 ? s.substr(1, s.size() - 2) : "";
            } else if (!rest.empty() && rest[0].kind == TokenKind::Lt) {
                std::string inner;
                std::size_t k = 1;
                for (; k < rest.size() && rest[k].kind != TokenKind::Gt; ++k)
                    inner += rest[k].spelling;
                if (k < rest.size()) {
                    name = inner;
                    angled = true;
                }
            } else if (attempt == 0) {
                rest = expander.expandLine(rest); // computed include
            }
        }
        if (name.empty()) {
            result_.errors.push_back(where + ": malformed #include");
            return;
        }
        const std::string resolved = resolveInclude(name, angled, dir);
        if (resolved.empty()) {
            result_.errors.push_back(where + ": include not found: " + name);
            return;
        }
        processFile(resolved, depth + 1);
    }
};

} // namespace

PreprocessResult preprocess(const PreprocessOptions& options) {
    return Preprocessor(options).run();
}

std::string dumpPreprocessed(const PreprocessResult& result) {
    std::string out;
    std::uint32_t curFile = ~0u;
    for (const Token& t : result.tokens) {
        if (t.kind == TokenKind::End) continue;
        if (t.file != curFile) {
            curFile = t.file;
            out += "#file \"";
            out += t.file < result.files.size()
                       ? fs::path(result.files[t.file]).filename().string()
                       : "?";
            out += "\"\n";
        }
        out += std::to_string(t.line);
        out += ':';
        out += std::to_string(t.col);
        out += ' ';
        out += tokenKindName(t.kind);
        out += ' ';
        out += t.spelling;
        out += '\n';
    }
    for (const std::string& e : result.errors) {
        out += "! ";
        out += e;
        out += '\n';
    }
    for (const std::string& w : result.warnings) {
        out += "!warn ";
        out += w;
        out += '\n';
    }
    return out;
}

} // namespace insbind
