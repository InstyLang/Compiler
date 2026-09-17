#include "model.h"

#include <string>

namespace insbind {
namespace {

std::string typeStr(const ParseResult& r, std::uint32_t idx);

std::string paramStr(const ParseResult& r, const CType& t) {
    std::string out;
    for (std::size_t i = 0; i < t.params.size(); ++i) {
        if (i) out += ", ";
        out += typeStr(r, t.params[i]);
        if (i < t.paramNames.size() && !t.paramNames[i].empty()) {
            out += ' ';
            out += t.paramNames[i];
        }
    }
    if (t.variadic) {
        if (!t.params.empty()) out += ", ";
        out += "...";
    }
    return out;
}

std::string typeStr(const ParseResult& r, std::uint32_t idx) {
    if (idx == kNoType || idx >= r.types.size()) return "?";
    const CType& t = r.types[idx];
    switch (t.kind) {
        case CTypeKind::Void: return "void";
        case CTypeKind::Bool: return "bool";
        case CTypeKind::Int:
            return (t.isSigned ? "s" : "u") + std::to_string(t.bits);
        case CTypeKind::Float:
            return "f" + std::to_string(t.bits);
        case CTypeKind::Pointer:
            return "ptr(" + typeStr(r, t.target) + ")";
        case CTypeKind::Array:
            return typeStr(r, t.target) + "[" +
                   (t.count == kNoCount ? "" : std::to_string(t.count)) + "]";
        case CTypeKind::Function:
            return "fn(" + paramStr(r, t) + ") -> " + typeStr(r, t.target);
        case CTypeKind::Record: {
            std::string s = t.name.empty() ? "<anon>" : t.name;
            if (t.target != kNoType && t.target < r.records.size() &&
                r.records[t.target].incomplete)
                s += " (incomplete)";
            return "record " + s;
        }
        case CTypeKind::Enum: {
            std::string s = t.name.empty() ? "<anon>" : t.name;
            if (t.target != kNoType && t.target < r.enums.size() &&
                r.enums[t.target].incomplete)
                s += " (incomplete)";
            return "enum " + s;
        }
        case CTypeKind::TypedefRef:
            return t.name;
    }
    return "?";
}

} // namespace

std::string dumpModel(const ParseResult& r) {
    std::string out;
    for (const CDecl& decl : r.decls) {
        if (const auto* td = std::get_if<CTypedefDecl>(&decl)) {
            out += "typedef " + td->name + " = " + typeStr(r, td->type) + "\n";
        } else if (const auto* rec = std::get_if<CRecordDef>(&decl)) {
            out += rec->isUnion ? "union " : "record ";
            out += rec->name;
            if (rec->alignAs) out += " align(" + std::to_string(rec->alignAs) + ")";
            out += " {\n";
            for (const CField& f : rec->fields) {
                out += "  ";
                out += f.anonymous ? "(anon)" : (f.name.empty() ? "(pad)" : f.name);
                out += ": " + typeStr(r, f.type);
                if (f.bitfield) out += " : " + std::to_string(f.bits);
                out += "\n";
            }
            out += "}\n";
        } else if (const auto* en = std::get_if<CEnumDef>(&decl)) {
            out += "enum " + en->name + " {\n";
            for (const auto& [name, value] : en->values)
                out += "  " + name + " = " + std::to_string(value) + "\n";
            out += "}\n";
        } else if (const auto* fn = std::get_if<CFunctionDecl>(&decl)) {
            const CType& t = r.types[fn->type];
            out += "fun " + fn->name + "(" + paramStr(r, t) + ") -> " +
                   typeStr(r, t.target);
            if (fn->noReturn) out += " [noreturn]";
            if (fn->staticInline) out += " [static-inline]";
            out += "\n";
        } else if (const auto* g = std::get_if<CGlobalDecl>(&decl)) {
            out += "global " + g->name + ": " + typeStr(r, g->type);
            if (g->isExtern) out += " [extern]";
            out += "\n";
        }
    }
    for (const std::string& e : r.errors) out += "! " + e + "\n";
    for (const std::string& w : r.warnings) out += "!warn " + w + "\n";
    return out;
}

} // namespace insbind
