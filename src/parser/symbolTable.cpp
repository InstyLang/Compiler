#include <parser/scope_manager.hpp>

void ScopeManager::pushScope() {
    scopes_.emplace_back();
}

void ScopeManager::popScope() {
    if (scopes_.size() > 1) {
        scopes_.pop_back();
    }
}

void ScopeManager::reset() {
    scopes_.clear();
    scopes_.emplace_back();
}

bool ScopeManager::declare(const Symbol& symbol) {
    if (scopes_.empty()) {
        scopes_.emplace_back();
    }
    auto& scope = scopes_.back();
    if (scope.find(symbol.name) != scope.end()) {
        return false;
    }
    scope.emplace(symbol.name, symbol);
    return true;
}

std::optional<Symbol> ScopeManager::lookup(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) {
            return found->second;
        }
    }
    return std::nullopt;
}

std::optional<Symbol> ScopeManager::lookupLocal(const std::string& name) const {
    if (scopes_.empty()) {
        return std::nullopt;
    }
    const auto& scope = scopes_.back();
    auto found = scope.find(name);
    if (found != scope.end()) {
        return found->second;
    }
    return std::nullopt;
}

const std::map<std::string, Symbol>& ScopeManager::getGlobalSymbols() const {
    static const std::map<std::string, Symbol> empty;
    if (scopes_.empty()) {
        return empty;
    }
    return scopes_.front();
}
