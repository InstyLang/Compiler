#include <backend/const_eval.hpp>

#include <string>

namespace Backend {

SizeAlign scalarSizeAlign(Types::TypeRef t) {
    if (!t) return {8, 8};
    switch (t->kind) {
        case Types::Kind::Int:
        case Types::Kind::Float:
        // A C-style enum carries its declared underlying width (registered by
        // TypeContext::registerEnumUnderlying), so it sizes like the integer it is
        // rather than falling through to the 8-byte default below.
        case Types::Kind::Enum: {
            unsigned bytes = t->bitWidth > 0 ? static_cast<unsigned>(t->bitWidth) / 8 : 8;
            if (bytes == 0) bytes = 1;
            return {bytes, bytes};
        }
        case Types::Kind::Bool:
            return {1, 1};
        case Types::Kind::Pointer:
        case Types::Kind::Text:
            return {8, 8};
        case Types::Kind::Slice:
            return {16, 8};
        case Types::Kind::Any:
            return {24, 8};
        case Types::Kind::Object:
            return {8, 8};
        case Types::Kind::Closure:
            return {8, 8};
        case Types::Kind::Array: {
            SizeAlign el = scalarSizeAlign(t->element);
            std::uint64_t n =
                t->arrayLength > 0 ? static_cast<std::uint64_t>(t->arrayLength) : 0;
            return {el.size * n, el.align};
        }
        default:
            return {8, 8};
    }
}

bool evalConstInt(const AST::ExprAST* e, __int128& out) {
    if (!e) return false;
    switch (e->nodeType()) {
        case AST::NodeType::IntegerLiteral:
            out = static_cast<const AST::IntegerLiteral*>(e)->value;
            return true;
        case AST::NodeType::BoolLiteral:
            out = static_cast<const AST::BoolLiteral*>(e)->value ? 1 : 0;
            return true;
        case AST::NodeType::UnaryExpr: {
            const auto* u = static_cast<const AST::UnaryExpr*>(e);
            __int128 v = 0;
            if (!evalConstInt(u->operand.get(), v)) return false;
            if (u->op == "-") { out = -v; return true; }
            if (u->op == "+") { out = v; return true; }
            if (u->op == "!") { out = (v == 0) ? 1 : 0; return true; }
            if (u->op == "~") { out = ~v; return true; }
            return false;
        }
        case AST::NodeType::BinaryOperation: {
            const auto* b = static_cast<const AST::BinaryOperationExpr*>(e);
            __int128 l = 0, r = 0;
            if (!evalConstInt(b->lhs.get(), l)) return false;
            if (!evalConstInt(b->rhs.get(), r)) return false;
            const std::string& op = b->op;
            if (op == "+") { out = l + r; return true; }
            if (op == "-") { out = l - r; return true; }
            if (op == "*") { out = l * r; return true; }
            // Use 64-bit division/modulo: 128-bit variants would pull in
            // compiler-rt helpers (__divti3/__modti3) the static link lacks, and
            // constant initializers never need more than 64-bit precision here.
            if (op == "/") {
                if (r == 0) return false;
                out = static_cast<long long>(l) / static_cast<long long>(r);
                return true;
            }
            if (op == "%") {
                if (r == 0) return false;
                out = static_cast<long long>(l) % static_cast<long long>(r);
                return true;
            }
            if (op == "&") { out = l & r; return true; }
            if (op == "|") { out = l | r; return true; }
            if (op == "^") { out = l ^ r; return true; }
            return false;
        }
        case AST::NodeType::ShiftOperation: {
            const auto* s = static_cast<const AST::ShiftOperationExpr*>(e);
            __int128 l = 0, r = 0;
            if (!evalConstInt(s->lhs.get(), l)) return false;
            if (!evalConstInt(s->rhs.get(), r)) return false;
            if (r < 0 || r >= 128) return false;
            if (s->op == "<<") { out = l << static_cast<int>(r); return true; }
            if (s->op == ">>") { out = l >> static_cast<int>(r); return true; }
            return false;
        }
        default:
            return false;
    }
}

}  // namespace Backend
