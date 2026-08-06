#include <compiler/comptime.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include <extra/builtins.hpp>

namespace Comptime {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

}  // namespace

// Matches the target's architecture, operating system or CLI name, plus a few
// family aliases so common questions stay short. "wasm" is the one that earns
// its keep: a module rarely cares whether it is wasm32-wasi or a bare wasm32.
bool targetMatches(const Targeting::TargetSpec& target, const std::string& rawName) {
    const std::string name = lower(rawName);
    if (name.empty()) return false;

    if (name == lower(target.arch)) return true;
    if (name == lower(target.os)) return true;
    if (name == lower(target.cliName)) return true;
    if (name == lower(target.abi)) return true;

    // Families.
    if (name == "wasm") return target.isWasmModule();
    if (name == "windows") return target.isWindowsLike;
    if (name == "unix") return target.isLinux() || target.isApple;
    if (name == "posix") return target.isLinux() || target.isApple;
    if (name == "apple" || name == "darwin") return target.isApple;
    if (name == "linux") return target.isLinux();
    if (name == "uefi" || name == "efi") return target.isEfi;
    if (name == "instantos") return target.isInstantOS;
    if (name == "freestanding") return target.isFreestandingExecutable();
    if (name == "x86_64" || name == "x64") return lower(target.arch) == "x86_64";
    if (name == "arm64" || name == "aarch64") {
        const std::string a = lower(target.arch);
        return a == "arm64" || a == "aarch64";
    }
    return false;
}

namespace {

// Evaluates a `#if` condition. Only the forms that can be answered without a
// program are accepted; anything else is reported rather than assumed false, so
// a typo cannot quietly drop a branch.
class ConditionEvaluator {
public:
    ConditionEvaluator(const Targeting::TargetSpec& target, std::string& errorOut)
        : target_(target), error_(errorOut) {}

    bool eval(const AST::ExprAST* expr, bool& out) {
        if (!expr) return fail("empty compile-time condition");
        switch (expr->nodeType()) {
            case AST::NodeType::BoolLiteral:
                out = static_cast<const AST::BoolLiteral*>(expr)->value;
                return true;

            case AST::NodeType::IntegerLiteral:
                out = static_cast<const AST::IntegerLiteral*>(expr)->value != 0;
                return true;

            case AST::NodeType::UnaryExpr: {
                const auto* u = static_cast<const AST::UnaryExpr*>(expr);
                if (u->op != "!") {
                    return fail("unary '" + u->op +
                                "' is not allowed in a compile-time condition");
                }
                bool inner = false;
                if (!eval(u->operand.get(), inner)) return false;
                out = !inner;
                return true;
            }

            case AST::NodeType::LogicalOperation: {
                const auto* b = static_cast<const AST::LogicalOperationExpr*>(expr);
                bool lhs = false;
                if (!eval(b->left.get(), lhs)) return false;
                // Short-circuit, so `@targetIs("windows") && <windows-only>` does
                // not have to be evaluable on other targets.
                if (b->op == "&&") {
                    if (!lhs) {
                        out = false;
                        return true;
                    }
                    return eval(b->right.get(), out);
                }
                if (b->op == "||") {
                    if (lhs) {
                        out = true;
                        return true;
                    }
                    return eval(b->right.get(), out);
                }
                return fail("operator '" + b->op +
                            "' is not allowed in a compile-time condition");
            }

            case AST::NodeType::BuiltinCall: {
                const auto* call = static_cast<const AST::BuiltinCallExpr*>(expr);
                if (Builtins::lookup(call->name) != Builtins::Builtin::TargetIs) {
                    return fail("'@" + call->name +
                                "' cannot be evaluated at compile time; a `#if` "
                                "condition may only use @targetIs, boolean literals, "
                                "and ! && ||");
                }
                if (call->arguments.size() != 1 || !call->arguments[0] ||
                    call->arguments[0]->nodeType() != AST::NodeType::StringLiteral) {
                    return fail("@targetIs expects one string literal, e.g. "
                                "@targetIs(\"wasm\")");
                }
                const auto* lit =
                    static_cast<const AST::StringLiteral*>(call->arguments[0].get());
                out = targetMatches(target_, lit->value);
                return true;
            }

            default:
                return fail("this expression cannot be evaluated at compile time; a "
                            "`#if` condition may only use @targetIs, boolean "
                            "literals, and ! && ||");
        }
    }

private:
    bool fail(const std::string& message) {
        if (error_.empty()) error_ = message;
        return false;
    }

    const Targeting::TargetSpec& target_;
    std::string& error_;
};

class Resolver {
public:
    Resolver(const Targeting::TargetSpec& target, AST::ProgramRoot& program,
             std::string& errorOut)
        : target_(target), program_(program), error_(errorOut),
          eval_(target, errorOut) {}

    bool ok() const { return error_.empty(); }

    // Rewrites `list` in place: every compile-time conditional is replaced by the
    // statements of whichever branch is taken (or by nothing).
    void resolveList(AST::NodeList& list) {
        AST::NodeList out;
        out.reserve(list.size());
        for (auto& node : list) {
            if (!node) continue;
            if (node->nodeType() == AST::NodeType::CompileTimeIf) {
                auto* cond = static_cast<AST::CompileTimeIfExpr*>(node.get());
                AST::NodeList* taken = nullptr;
                for (auto& branch : cond->branches) {
                    if (!branch.condition) {  // the `#else`
                        taken = &branch.body;
                        break;
                    }
                    bool value = false;
                    if (!eval_.eval(branch.condition.get(), value)) return;
                    if (value) {
                        taken = &branch.body;
                        break;
                    }
                }
                if (taken) {
                    // Resolve nested conditionals before splicing them in.
                    resolveList(*taken);
                    if (!ok()) return;
                    for (auto& inner : *taken) {
                        // An import inside the taken branch has to join the
                        // module's import list, which the parser only fills for
                        // imports it saw at top level. This is what makes a
                        // dependency conditional rather than merely its uses.
                        recordImport(inner);
                        out.push_back(std::move(inner));
                    }
                }
                continue;
            }
            resolveNode(node.get());
            if (!ok()) return;
            out.push_back(std::move(node));
        }
        list = std::move(out);
    }

private:
    void recordImport(const AST::NodePtr& node) {
        if (!node || node->nodeType() != AST::NodeType::ImportStatement) return;
        const auto* imp = static_cast<const AST::ImportStatement*>(node.get());
        if (imp->moduleName.empty()) return;
        for (const auto& existing : program_.imports) {
            if (existing == imp->moduleName) return;
        }
        program_.imports.push_back(imp->moduleName);
    }

    // Descends into every node that owns statements. Expressions cannot contain a
    // compile-time conditional -- the parser only produces one in statement
    // position -- so only statement lists are visited.
    void resolveNode(AST::ExprAST* node) {
        if (!node) return;
        switch (node->nodeType()) {
            case AST::NodeType::FunctionDeclaration:
                resolveList(static_cast<AST::FunctionDeclaration*>(node)->body);
                break;
            case AST::NodeType::IfStatement: {
                auto* s = static_cast<AST::IfStatement*>(node);
                resolveList(s->consequent);
                if (ok()) resolveList(s->alternate);
                break;
            }
            case AST::NodeType::WhileLoop:
                resolveList(static_cast<AST::WhileLoop*>(node)->body);
                break;
            case AST::NodeType::InfiniteLoop:
                resolveList(static_cast<AST::InfiniteLoop*>(node)->body);
                break;
            case AST::NodeType::ForLoop:
                resolveList(static_cast<AST::ForLoop*>(node)->body);
                break;
            case AST::NodeType::WhenStatement:
                resolveList(static_cast<AST::WhenStatement*>(node)->consequent);
                break;
            case AST::NodeType::UnsafeBlock:
                resolveList(static_cast<AST::UnsafeBlock*>(node)->body);
                break;
            case AST::NodeType::SwitchStatement: {
                auto* s = static_cast<AST::SwitchStatement*>(node);
                for (auto& arm : s->arms) {
                    resolveList(arm.body);
                    if (!ok()) return;
                }
                break;
            }
            case AST::NodeType::ClassDeclaration: {
                auto* s = static_cast<AST::ClassDeclaration*>(node);
                for (auto& method : s->methods) {
                    resolveList(method.body);
                    if (!ok()) return;
                }
                break;
            }
            default:
                break;
        }
    }

    const Targeting::TargetSpec& target_;
    AST::ProgramRoot& program_;
    std::string& error_;
    ConditionEvaluator eval_;
};

}  // namespace

bool resolve(AST::ProgramRoot& program, const Targeting::TargetSpec& target,
             ErrorReporting::ErrorReporter* reporter, std::string& errorOut) {
    (void)reporter;
    errorOut.clear();
    Resolver resolver(target, program, errorOut);
    resolver.resolveList(program.body);
    return resolver.ok();
}

}  // namespace Comptime
