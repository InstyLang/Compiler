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

}
