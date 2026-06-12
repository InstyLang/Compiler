#pragma once


#include <map>
#include <optional>
#include <string>
#include <vector>

struct Symbol {
    std::string name;
    std::string type;
    bool isGlobal = false;
    bool isParameter = false;
    int declarationLine = 0;
    int declarationColumn = 0;
    int length = 0;
    std::string docs;
    std::string preview;

    Symbol() = default;

    Symbol(std::string name_, std::string type_, bool isGlobal_, bool isParameter_,
           int declarationLine_, int declarationColumn_, int length_,
           std::string docs_, std::string preview_)
        : name(std::move(name_)),
          type(std::move(type_)),
          isGlobal(isGlobal_),
          isParameter(isParameter_),
          declarationLine(declarationLine_),
          declarationColumn(declarationColumn_),
          length(length_),
          docs(std::move(docs_)),
          preview(std::move(preview_)) {}
};

class ScopeManager {
public:
    ScopeManager() { pushScope(); }

    void pushScope();
    void popScope();
    void reset();

    bool declare(const Symbol& symbol);

    std::optional<Symbol> lookup(const std::string& name) const;

    std::optional<Symbol> lookupLocal(const std::string& name) const;

    const std::map<std::string, Symbol>& getGlobalSymbols() const;

    size_t scopeDepth() const { return scopes_.size(); }

private:
    std::vector<std::map<std::string, Symbol>> scopes_;
};
