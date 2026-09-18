#include "emit.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>

#include "constexpr.h"

namespace insbind {
namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string hex64(std::uint64_t v) {
    const char* digits = "0123456789abcdef";
    std::string out = "0x";
    bool started = false;
    for (int shift = 60; shift >= 0; shift -= 4) {
        const unsigned d = static_cast<unsigned>((v >> shift) & 0xF);
        if (d || started || shift == 0) {
            out += digits[d];
            started = true;
        }
    }
    return out;
}

struct Emitter {
    const ParseResult& m;
    const EmitOptions& o;
    LayoutEngine layout;
    std::unordered_map<std::string, std::int64_t> enumValues;
    std::unordered_set<std::string> emittedTypes; // record/enum names emitted
    std::vector<std::string> warnings;

    Emitter(const ParseResult& model, const EmitOptions& opts)
        : m(model), o(opts), layout{model, opts.layout} {
        for (const CEnumDef& e : m.enums)
            for (const auto& [name, value] : e.values)
                enumValues[name] = value;
    }

    void warn(const std::string& w) { warnings.push_back(w); }

    // ------------------------------------------------------ type mapping

    // Resolves a type to its concrete form (typedef chains followed) and
    // renders it as Insty syntax. Returns "" when unmappable (caller skips).
    std::string ty(std::uint32_t idx, const std::string& context) {
        if (idx == kNoType || idx >= m.types.size()) return "";
        const CType& t = m.types[idx];
        switch (t.kind) {
            case CTypeKind::Void: return "void";
            case CTypeKind::Bool: return "bool";
            case CTypeKind::Int:
                return (t.isSigned ? "i" : "u") + std::to_string(t.bits);
            case CTypeKind::Float:
                if (t.bits == 32) return "f32";
                if (t.bits == 64) return "f64";
                warn("long double is not FFI-mappable; " + context +
                     " skipped");
                return "";
            case CTypeKind::Pointer: {
                // Resolve typedef chains on the pointee before judging:
                // ptr-to-function -> u64, ptr-to-incomplete-record -> u64
                // (the opaque-handle convention), const char -> text.
                const CType* target = &m.types[t.target];
                while (target->kind == CTypeKind::TypedefRef &&
                       target->target != kNoType) {
                    target = &m.types[target->target];
                }
                if (target->kind == CTypeKind::Function) return "u64";
                if (target->kind == CTypeKind::Record &&
                    m.records[target->target].incomplete) {
                    return "u64";
                }
                if (target->kind == CTypeKind::Void) return "void*";
                // Pointer to array: Insty has no T[N]* spelling; a pointer to
                // the element type is the same address (C-idiomatic).
                if (target->kind == CTypeKind::Array)
                    return ty(target->target, context) + "*";
                if (target->kind == CTypeKind::Int && target->bits == 8 &&
                    target->isSigned) {
                    return t.pointeeIsConst ? "text" : "u8*";
                }
                return ty(t.target, context) + "*";
            }
            case CTypeKind::Array:
                if (t.count == kNoCount) {
                    warn("incomplete array in " + context + " (skipped)");
                    return "";
                }
                return ty(t.target, context) + "[" + std::to_string(t.count) +
                       "]";
            case CTypeKind::Function:
                return "u64"; // reached only un-decayed (defensive)
            case CTypeKind::Record:
                return m.records[t.target].name;
            case CTypeKind::Enum:
                return m.enums[t.target].name;
            case CTypeKind::TypedefRef:
                return t.target != kNoType ? ty(t.target, context) : "";
        }
        return "";
    }

    // True when every type in a function signature is mappable.
    bool signatureMappable(const CType& fn) {
        if (fn.kind != CTypeKind::Function) return false;
        std::unordered_set<std::uint32_t> seen;
        auto ok = [&](std::uint32_t idx, auto& self) -> bool {
            if (idx == kNoType || idx >= m.types.size() || seen.count(idx))
                return idx != kNoType && idx < m.types.size();
            seen.insert(idx);
            const CType& t = m.types[idx];
            switch (t.kind) {
                case CTypeKind::Float: return t.bits != 80;
                case CTypeKind::Pointer: {
                    const CType& tgt = m.types[t.target];
                    if (tgt.kind == CTypeKind::Function) return true; // u64
                    if (tgt.kind == CTypeKind::Record &&
                        m.records[tgt.target].incomplete)
                        return true; // opaque u64
                    return self(t.target, self);
                }
                case CTypeKind::Array:
                    return t.count != kNoCount && self(t.target, self);
                case CTypeKind::TypedefRef:
                    return self(t.target, self);
                case CTypeKind::Function: return false; // un-decayed fn type
                default: return true;
            }
        };
        if (!ok(fn.target, ok)) return false;
        for (const std::uint32_t p : fn.params)
            if (!ok(p, ok)) return false;
        return true;
    }

    std::string linkAttr() const {
        if (o.linkName.empty()) return "";
        return std::string(o.linkIsDll ? "[dll(\"" : "[lib(\"") + o.linkName +
               "\")] ";
    }

    // True when Insty can express this record's layout exactly. A field with
    // _Alignas beyond its natural alignment cannot be (Insty aligns fields
    // naturally); such records are emitted but flagged, and the abi-check
    // leaves them out rather than enshrine a failing expectation.
    bool expressibleInInsty(const CRecordDef& def) {
        for (const CField& f : def.fields) {
            if (f.alignAs && f.alignAs > layout.typeAlign(f.type)) return false;
        }
        return true;
    }

    // --------------------------------------------------------- records

    std::string emitRecord(const CRecordDef& def) {
        const std::uint32_t idx = recordIndexOf(def.name);
        const RecordLayout rl = layout.record(idx);
        std::string out;
        std::string attr;
        if (def.isUnion && rl.align > 1)
            attr = "[align(" + std::to_string(rl.align) + ")] ";
        else if (def.alignAs)
            attr = "[align(" + std::to_string(def.alignAs) + ")] ";
        if (!expressibleInInsty(def)) {
            out += "// warning: record '" + def.name +
                   "' has an over-aligned field (_Alignas); Insty lays it\n";
            out += "// out differently than C. Do not pass it by value.\n";
            warn("record '" + def.name +
                 "' has unexpressible field alignment");
        }
        out += "export struct " + attr + def.name + " {\n";

        std::vector<std::string> fieldLines;
        auto joinFields = [&] {
            for (std::size_t i = 0; i < fieldLines.size(); ++i) {
                out += "    " + fieldLines[i];
                out += i + 1 < fieldLines.size() ? ",\n" : "\n";
            }
        };

        if (def.isUnion) {
            fieldLines.push_back("u8[" + std::to_string(rl.size) + "] _data");
            joinFields();
            out += "}\n\n";
            for (std::size_t fi = 0; fi < def.fields.size(); ++fi) {
                const CField& f = def.fields[fi];
                // Array members yield a pointer to the element type (same
                // address as the first element); Insty has no T[N]* spelling.
                const CType& ft0 = m.types[f.type];
                const bool isArray = ft0.kind == CTypeKind::Array;
                const std::string mt =
                    isArray ? ty(ft0.target, "union member " + f.name)
                            : ty(f.type, "union member " + f.name);
                if (mt.empty()) continue;
                out += "export fun " + lower(def.name) + "_" + f.name + "(" +
                       def.name + "* u) -> " + mt + "* {\n";
                out += "    unsafe {\n";
                out += "        return cast<" + mt + "*>(u)\n";
                out += "    }\n}\n\n";
            }
            return out;
        }

        // Plain record: fields in order, bitfield runs as storage units.
        std::string bfAccessors;
        for (std::size_t fi = 0; fi < def.fields.size(); ++fi) {
            const CField& f = def.fields[fi];
            const FieldLayout& fl = rl.fields[fi];
            if (f.bitfield) {
                // Start of a new storage unit?
                const bool newUnit =
                    fi == 0 || !def.fields[fi - 1].bitfield ||
                    rl.fields[fi - 1].unitOffset != fl.unitOffset;
                const std::string unitField =
                    "_bf" + std::to_string(fl.unitOffset);
                if (newUnit) {
                    const std::string ut = ty(f.type, "bitfield unit");
                    fieldLines.push_back(ut + " " + unitField);
                }
                if (f.name.empty()) continue; // padding: no accessor
                const std::string ft = ty(f.type, "bitfield " + f.name);
                if (ft.empty()) continue;
                const std::uint64_t mask =
                    f.bits >= 64 ? ~0ull : ((1ull << f.bits) - 1);
                bfAccessors += "export fun " + lower(def.name) + "_" + f.name + "(" +
                               def.name + "* s) -> " + ft + " {\n";
                bfAccessors += "    unsafe {\n";
                bfAccessors += "        return ";
                if (fl.bitOffset)
                    bfAccessors += "(s." + unitField + " >> " +
                                   std::to_string(fl.bitOffset) + ") & " +
                                   hex64(mask) + "\n";
                else if (f.bits < 64)
                    bfAccessors += "s." + unitField + " & " + hex64(mask) +
                                   "\n";
                else
                    bfAccessors += "s." + unitField + "\n";
                bfAccessors += "    }\n}\n\n";
                continue;
            }
            if (f.anonymous) {
                fieldLines.push_back(ty(f.type, "anonymous member") + " _u");
                continue;
            }
            const std::string ft = ty(f.type, "field " + f.name);
            if (ft.empty()) {
                fieldLines.push_back("// field '" + f.name +
                                     "' skipped (unmappable)");
                continue;
            }
            fieldLines.push_back(ft + " " + f.name);
        }
        joinFields();
        out += "}\n\n";
        out += bfAccessors;
        return out;
    }

    std::uint32_t recordIndexOf(const std::string& name) const {
        for (std::uint32_t i = 0; i < m.records.size(); ++i)
            if (m.records[i].name == name) return i;
        return kNoType;
    }

    // ------------------------------------------------------------ enums

    std::string emitEnum(const CEnumDef& def) {
        unsigned bits;
        bool isSigned;
        LayoutEngine::enumUnderlying(def, bits, isSigned);
        const std::string base =
            (isSigned ? "i" : "u") + std::to_string(bits);
        std::string out = "export enum " + def.name + " : " + base + " {\n";
        std::string deferred;
        bool first = true;
        for (std::size_t i = 0; i < def.values.size(); ++i) {
            const auto& [name, value] = def.values[i];
            // Insty enum variant values must be non-negative literals:
            // negative ones become constant accessors after the enum.
            if (value < 0) {
                deferred += "export fun " + lower(def.name) + "_" + lower(name) +
                            "() -> " + base + " {\n";
                deferred += "    return " + std::to_string(value) + "\n";
                deferred += "}\n\n";
                continue;
            }
            if (!first) out += ",\n";
            out += "    " + name + " = " + std::to_string(value);
            first = false;
        }
        out += first ? "    _unused\n" : "\n";
        out += "}\n\n";
        out += deferred;
        return out;
    }

    // --------------------------------------------------------- functions

    std::string emitFunction(const CFunctionDecl& fn) {
        const CType& t = m.types[fn.type];
        if (fn.staticInline)
            return "// skipped static inline function: " + fn.name + "\n\n";
        if (fn.variadic)
            return "// skipped variadic function (needs a C shim): " +
                   fn.name + "\n\n";
        if (!signatureMappable(t))
            return "// skipped function with unmappable signature: " +
                   fn.name + "\n\n";
        std::string out = "export extern fun " + linkAttr() + fn.name + "(";
        for (std::size_t i = 0; i < t.params.size(); ++i) {
            if (i) out += ", ";
            out += ty(t.params[i], "parameter of " + fn.name);
            // C allows unnamed parameters; Insty requires `Type name`.
            if (i < t.paramNames.size() && !t.paramNames[i].empty())
                out += " " + t.paramNames[i];
            else
                out += " _a" + std::to_string(i);
        }
        out += ") -> " + ty(t.target, "return of " + fn.name) + "\n\n";
        return out;
    }

    // --------------------------------------------------------- constants

    std::string emitConstants() {
        if (!o.macros) return "";
        std::string out;
        std::unordered_map<std::string, std::int64_t> values;
        std::vector<std::string> order;
        for (const auto& [name, replacement] : *o.macros) {
            std::size_t pos = 0;
            std::int64_t value = 0;
            std::string err;
            if (replacement.empty()) continue;
            if (!evalConstExpr(replacement, &pos, &value, &err, &enumValues))
                continue; // not an integer constant: not our business
            if (pos != replacement.size()) continue;
            if (!values.count(name)) order.push_back(name);
            values[name] = value; // last definition wins
        }
        for (const std::string& name : order) {
            // C integer constants are int-valued in practice: i32 when the
            // value fits (so they pass straight into i32 parameters), i64
            // beyond that, u64 when even i64 cannot hold it.
            const std::int64_t v = values[name];
            const std::string rt =
                (v >= -2147483648LL && v <= 2147483647LL) ? "i32"
                : (v >= -9223372036854775807LL - 1 && static_cast<std::uint64_t>(v) <= 9223372036854775807ULL)
                      ? "i64"
                      : "u64";
            out += "export fun " + lower(name) + "() -> " + rt + " {\n";
            out += "    return " + std::to_string(v) + "\n";
            out += "}\n\n";
        }
        return out;
    }

    // --------------------------------------------------------- abi check

    std::string emitAbiCheck(const std::string& body) {
        std::string out = "module main\n\nimport std::io\n\n";
        // The binding declarations are inlined (module line stripped) so the
        // check is one self-contained file; unused externs are never linked.
        out += body;
        out += "fun main() -> i32 {\n";
        out += "    i32 fails = 0\n";
        out += "    unsafe {\n";
        for (const CDecl& decl : m.decls) {
            const auto* rec = std::get_if<CRecordDef>(&decl);
            if (!rec || rec->incomplete || !expressibleInInsty(*rec)) continue;
            const std::uint32_t idx = recordIndexOf(rec->name);
            const RecordLayout rl = layout.record(idx);
            out += "        if " + rec->name + ".insize != " +
                   std::to_string(rl.size) + " {\n";
            out += "            io.println(\"FAIL " + rec->name +
                   ".insize != " + std::to_string(rl.size) + "\")\n";
            out += "            fails = fails + 1\n        }\n";
            if (rec->isUnion) continue;
            for (std::size_t fi = 0; fi < rec->fields.size(); ++fi) {
                const CField& f = rec->fields[fi];
                if (f.name.empty() && !f.anonymous) continue;
                const std::string fname =
                    f.bitfield ? "_bf" + std::to_string(rl.fields[fi].unitOffset)
                    : f.anonymous ? "_u" : f.name;
                // Only the first field of a bitfield unit is checked.
                if (f.bitfield && fi > 0 && rec->fields[fi - 1].bitfield &&
                    rl.fields[fi - 1].unitOffset == rl.fields[fi].unitOffset)
                    continue;
                out += "        " + rec->name + " _chk_" + rec->name +
                       std::to_string(fi) + "\n";
                out += "        if cast<u64>(&(_chk_" + rec->name +
                       std::to_string(fi) + ")." + fname + ") - cast<u64>(&(_chk_" +
                       rec->name + std::to_string(fi) + ")) != " +
                       std::to_string(rl.fields[fi].offset) + " {\n";
                out += "            io.println(\"FAIL " + rec->name + "." +
                       fname + " offset != " +
                       std::to_string(rl.fields[fi].offset) + "\")\n";
                out += "            fails = fails + 1\n        }\n";
            }
        }
        out += "    }\n";
        out += "    if fails == 0 {\n";
        out += "        io.println(\"abi-check: all layouts match\")\n";
        out += "    }\n";
        out += "    return fails\n}\n";
        return out;
    }

    // ------------------------------------------------------------- main

    EmitResult run() {
        EmitResult result;
        std::string body;
        for (const CDecl& decl : m.decls) {
            if (const auto* td = std::get_if<CTypedefDecl>(&decl)) {
                body += "// typedef " + td->name + " = " + ty(td->type, "typedef") + "\n\n";
            } else if (const auto* rec = std::get_if<CRecordDef>(&decl)) {
                if (rec->incomplete || !emittedTypes.insert(rec->name).second)
                    continue;
                body += emitRecord(*rec);
            } else if (const auto* en = std::get_if<CEnumDef>(&decl)) {
                if (!emittedTypes.insert(en->name).second) continue;
                body += emitEnum(*en);
            } else if (const auto* fn = std::get_if<CFunctionDecl>(&decl)) {
                body += emitFunction(*fn);
            } else if (const auto* g = std::get_if<CGlobalDecl>(&decl)) {
                body += "// extern global (needs an accessor shim): " + g->name + "\n\n";
                warn("extern global skipped: " + g->name);
            }
        }
        body += emitConstants();

        result.bindings = "// Generated by insbind. Conventions mirror the hand-written\n";
        result.bindings += "// windows::*/unix::* bindings: opaque handles and function\n";
        result.bindings += "// pointers are u64, C strings are text (const) or u8*\n";
        result.bindings += "// (mutable), constants are zero-argument functions.\n\n";
        result.bindings += "module " + o.moduleName + "\n\n";
        result.bindings += body;
        result.warnings = warnings;
        if (o.abiCheck) result.abiCheck = emitAbiCheck(body);
        return result;
    }
};

} // namespace

EmitResult emit(const ParseResult& model, const EmitOptions& opts) {
    return Emitter(model, opts).run();
}

} // namespace insbind
