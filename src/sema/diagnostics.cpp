#include <sema/checker.hpp>


namespace Sema {

ErrorReporting::SourceLocation Checker::locOf(const AST::ExprAST* node) {
    ErrorReporting::SourceLocation loc;
    if (!node) {
        loc.line = 0;
        loc.column = 0;
        loc.length = 1;
        loc.offset = -1;
        return loc;
    }
    const AST::SourceRange& r = node->range;
    loc.line = r.startLine > 0 ? r.startLine : 0;
    loc.column = r.startColumn > 0 ? r.startColumn : 0;
    loc.offset = r.startOffset;

    if (r.startOffset >= 0 && r.endOffset >= r.startOffset) {
        int len = r.endOffset - r.startOffset;
        loc.length = len > 0 ? len : 1;
    } else {
        loc.length = 1;
    }
    return loc;
}

void Checker::emit(const std::string& code, const std::string& message,
                   const AST::ExprAST* at, const std::string& hint) {
    if (!reporter_) {
        return;
    }
    reporter_->error(code, message, locOf(at), hint);
}

bool Checker::alreadyErrored(const AST::ExprAST* node) {
    if (!node) {
        return false;
    }
    auto it = errored_.find(node);
    return it != errored_.end() && it->second;
}

void Checker::markErrored(const AST::ExprAST* node) {
    if (node) {
        errored_[node] = true;
    }
}

Types::TypeRef Checker::record(const AST::ExprAST* node, Types::TypeRef type) {
    Types::TypeRef resolved = type ? type : types_.errorType();
    if (node) {
        result_.exprTypes[node] = resolved;
    }
    return resolved;
}

bool Checker::isIntLiteral(const AST::NodePtr& node) const {
    if (!node) return false;
    switch (node->nodeType()) {
        case AST::NodeType::IntegerLiteral:
            return true;
        case AST::NodeType::BinaryOperation: {
            auto* bin = static_cast<AST::BinaryOperationExpr*>(node.get());
            return isIntLiteral(bin->lhs) && isIntLiteral(bin->rhs);
        }
        case AST::NodeType::ShiftOperation: {
            auto* sh = static_cast<AST::ShiftOperationExpr*>(node.get());
            return isIntLiteral(sh->lhs) && isIntLiteral(sh->rhs);
        }
        case AST::NodeType::UnaryExpr: {
            auto* un = static_cast<AST::UnaryExpr*>(node.get());
            return isIntLiteral(un->operand);
        }
        default:
            return false;
    }
}

bool Checker::isFloatLiteral(const AST::NodePtr& node) const {
    if (!node) return false;
    switch (node->nodeType()) {
        case AST::NodeType::FloatLiteral:
            return true;
        case AST::NodeType::BinaryOperation: {
            auto* bin = static_cast<AST::BinaryOperationExpr*>(node.get());
            return isFloatLiteral(bin->lhs) && isFloatLiteral(bin->rhs);
        }
        case AST::NodeType::UnaryExpr: {
            auto* un = static_cast<AST::UnaryExpr*>(node.get());
            return isFloatLiteral(un->operand);
        }
        default:
            return false;
    }
}

bool Checker::foldIntLiteral(const AST::NodePtr& node, unsigned __int128& bits,
                             bool& ok) const {
    if (!node) {
        ok = false;
        return false;
    }
    switch (node->nodeType()) {
        case AST::NodeType::IntegerLiteral: {
            bits = static_cast<unsigned __int128>(
                static_cast<const AST::IntegerLiteral*>(node.get())->value);
            return true;
        }
        case AST::NodeType::UnaryExpr: {
            auto* un = static_cast<const AST::UnaryExpr*>(node.get());
            if (!foldIntLiteral(un->operand, bits, ok)) return false;
            if (un->op == "-") bits = static_cast<unsigned __int128>(0) - bits;
            else if (un->op == "!") bits = bits == 0 ? 1 : 0;
            else if (un->op == "~") bits = ~bits;
            else if (un->op != "+") {
                ok = false;
                return false;
            }
            return true;
        }
        case AST::NodeType::BinaryOperation: {
            auto* bin = static_cast<const AST::BinaryOperationExpr*>(node.get());
            unsigned __int128 lhs = 0;
            unsigned __int128 rhs = 0;
            if (!foldIntLiteral(bin->lhs, lhs, ok) || !foldIntLiteral(bin->rhs, rhs, ok)) {
                return false;
            }
            if (bin->op == "+") bits = lhs + rhs;
            else if (bin->op == "-") bits = lhs - rhs;
            else if (bin->op == "*") bits = lhs * rhs;
            else if (bin->op == "/") {
                if (rhs == 0) { ok = false; return false; }
                bits = lhs / rhs;
            } else if (bin->op == "%") {
                if (rhs == 0) { ok = false; return false; }
                bits = lhs % rhs;
            } else if (bin->op == "&") bits = lhs & rhs;
            else if (bin->op == "|") bits = lhs | rhs;
            else if (bin->op == "^") bits = lhs ^ rhs;
            else {
                ok = false;
                return false;
            }
            return true;
        }
        case AST::NodeType::ShiftOperation: {
            auto* sh = static_cast<const AST::ShiftOperationExpr*>(node.get());
            unsigned __int128 lhs = 0;
            unsigned __int128 rhs = 0;
            if (!foldIntLiteral(sh->lhs, lhs, ok) || !foldIntLiteral(sh->rhs, rhs, ok)) {
                return false;
            }
            const unsigned amt = static_cast<unsigned>(rhs);
            if (amt >= 128) {
                bits = 0;
                return true;
            }
            if (sh->op == "<<") bits = lhs << amt;
            else if (sh->op == ">>") bits = lhs >> amt;
            else {
                ok = false;
                return false;
            }
            return true;
        }
        default:
            ok = false;
            return false;
    }
}

}
