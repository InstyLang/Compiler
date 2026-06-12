#include <sema/sema.hpp>


namespace Sema {

std::string mangleFunction(const std::string& moduleName, const std::string& functionName) {
    if (moduleName.empty() || moduleName == "main") {
        return functionName;
    }
    return moduleName + "_" + functionName;
}

std::string mangleMethod(const std::string& typeName, const std::string& methodName,
                         const std::vector<std::string>& paramTypes) {
    std::string mangled = typeName + "_" + methodName;
    for (const auto& param : paramTypes) {
        mangled += "_" + param;
    }
    return mangled;
}

std::string operatorMangleName(const std::string& symbol) {
    if (symbol == "+") return "add";
    if (symbol == "-") return "sub";
    if (symbol == "*") return "mul";
    if (symbol == "/") return "div";
    if (symbol == "%") return "rem";
    if (symbol == "==") return "eq";
    if (symbol == "!=") return "neq";
    if (symbol == "<") return "lt";
    if (symbol == ">") return "gt";
    if (symbol == "<=") return "le";
    if (symbol == ">=") return "ge";
    if (symbol == "&") return "and";
    if (symbol == "|") return "or";
    if (symbol == "^") return "xor";
    if (symbol == "<<") return "shl";
    if (symbol == ">>") return "shr";
    if (symbol == "[]") return "index";
    return symbol;
}

std::string mangleClassMember(const std::string& className, const std::string& memberName,
                              bool isConstructor, bool isOperator,
                              const std::string& operatorSymbol,
                              const std::vector<std::string>& paramTypes) {
    if (isConstructor) {
        std::string m = className + "_constructor";
        for (const auto& p : paramTypes) {
            m += "_" + p;
        }
        return m;
    }
    if (isOperator) {
        return className + "_operator_" + operatorMangleName(operatorSymbol);
    }
    return className + "_" + memberName;
}

std::string mangleGenericInstance(const std::string& templateName,
                                  const std::vector<std::string>& typeArgs) {
    std::string m = templateName;
    for (const auto& arg : typeArgs) {
        m += "_";
        for (char ch : arg) {
            switch (ch) {
                case '*': m += "ptr"; break;
                case '[':
                case ']':
                case ' ': break;
                case '<':
                case '>':
                case ',': m += "_"; break;
                default: m += ch; break;
            }
        }
    }
    return m;
}

}
