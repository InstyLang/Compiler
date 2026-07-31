
#include <parser/parser.hpp>
#include <lexer/lexer.hpp>

#include <memory>
#include <string>

namespace ecxlit {
std::shared_ptr<AST::IntegerLiteral> makeInteger(const std::string& raw);
std::shared_ptr<AST::FloatLiteral> makeFloat(const std::string& raw);
std::shared_ptr<AST::BoolLiteral> makeBool(bool value);
std::shared_ptr<AST::StringLiteral> makeString(const std::string& decoded);
}

AST::NodePtr ecxParseArrayLiteral(Parser& parser);
AST::NodePtr ecxParseStructInstantiation(Parser& parser, const Token& nameTok);

namespace {
bool g_allowStructLiteral = true;

std::string parseNewElementTypeName(Parser& parser) {
    std::string spelling;

    if (parser.check(TokenType::KwVolatile)) {
        parser.advance();
        spelling += "volatile ";
    }

    if (parser.check(TokenType::Identifier) || isPrimitiveTypeName(parser.current().value)) {
        spelling += parser.current().value;
        parser.advance();
    } else if (parser.check(TokenType::KwThis)) {
        spelling += "this";
        parser.advance();
    } else {
        parser.error("E1101", "expected a type name", "e.g. `new i32`, `new Point`, or `new Point[n]`");
        return spelling;
    }

    while (parser.check(TokenType::Dot) && parser.peek().type == TokenType::Identifier) {
        parser.advance();
        spelling += ".";
        spelling += parser.current().value;
        parser.advance();
    }

    if (parser.check(TokenType::Lt)) {
        spelling += "<";
        parser.advance();
        bool firstArg = true;
        while (!parser.atEnd() && !parser.check(TokenType::Gt)) {
            if (!firstArg) {
                if (!parser.match(TokenType::Comma)) {
                    break;
                }
                spelling += ", ";
            }
            firstArg = false;
            spelling += parser.parseTypeName();
        }
        parser.expect(TokenType::Gt, "E1102", "'>' to close generic arguments");
        parser.match(TokenType::Gt);
        spelling += ">";
    }

    while (parser.check(TokenType::Star)) {
        parser.advance();
        spelling += "*";
    }

    return spelling;
}
}
void ecxSetAllowStructLiteral(bool allow) { g_allowStructLiteral = allow; }
bool ecxAllowStructLiteral() { return g_allowStructLiteral; }


// Anonymous function (non-capturing lambda):
//     |Type a, Type b| => a + b            (expression body)
//     |Type a| => { ...; return v }        (block body)
// Lowered by lifting the body into a synthesized top-level function; the lambda
// expression's value is that function's address (a function pointer). The
// return type is inferred by sema (the synthesized function is declared with an
// `auto` return that sema resolves from the body).
AST::NodePtr Parser::parseLambda() {
    const Token& start = current();
    expect(TokenType::Pipe, "E1440", "'|' to begin a lambda's parameters");
    match(TokenType::Pipe);

    auto fn = std::make_shared<AST::FunctionDeclaration>();
    fn->name = "__lambda_" + std::to_string(lambdaCounter_++);
    fn->hasBody = true;
    fn->returnType = "auto";

    skipNewlines();
    while (!atEnd() && !check(TokenType::Pipe)) {
        AST::Parameter p;
        if (check(TokenType::KwVolatile)) {
            p.isVolatile = true;
            advance();
        }
        p.type = parseTypeName();
        if (check(TokenType::Identifier)) {
            p.name = current().value;
            advance();
        } else {
            error("E1442", "expected lambda parameter name",
                  "lambda parameters are `Type name`, e.g. |i32 x| => x + 1");
        }
        fn->parameters.push_back(std::move(p));
        skipNewlines();
        if (!match(TokenType::Comma)) {
            break;
        }
        skipNewlines();
    }
    expect(TokenType::Pipe, "E1443", "'|' to close lambda parameters");
    match(TokenType::Pipe);

    expect(TokenType::FatArrow, "E1444", "'=>' after lambda parameters");
    match(TokenType::FatArrow);

    skipNewlines();
    if (check(TokenType::LBrace)) {
        fn->body = parseBlock();
    } else {
        // Single-expression body: `=> expr` is sugar for `{ return expr }`.
        auto ret = std::make_shared<AST::ReturnStatement>();
        ret->returnValue = parseExpression();
        fillRange(*ret, start);
        fn->body.push_back(ret);
    }
    fillRange(*fn, start);

    auto lam = std::make_shared<AST::LambdaExpr>();
    lam->name = fn->name;
    lam->function = fn;
    fillRange(*lam, start);
    return lam;
}

AST::NodePtr Parser::parsePrimary() {
    const Token& start = current();

    // `asm [keep(rax), ...]( <NASM text> )`: a raw assembly block. Recognized
    // here, ahead of the Identifier case below, because that case would return a
    // plain identifier and the postfix parser would then read `asm [...]` as an
    // index expression.
    //
    // Only the block form is taken: the legacy `asm("syscall", "={rax}", n)` stays
    // an ordinary identifier so it continues to parse as a call, which is how the
    // selector already receives it. The two are told apart by the bracketed
    // directive list, or by the parenthesis not being followed by a string.
    if (start.type == TokenType::Identifier && start.value == "asm") {
        const bool hasDirectives = peek().type == TokenType::LBracket;
        const bool bareBlock =
            peek().type == TokenType::LParen && index_ + 2 < tokens_.size() &&
            tokens_[index_ + 2].type != TokenType::StringLiteral &&
            tokens_[index_ + 2].type != TokenType::RParen;
        if (hasDirectives || bareBlock) {
            advance();
            auto node = std::make_shared<AST::InlineAsmExpr>();
            node->isBlock = true;
            if (hasDirectives) {
                node->attributes = parseAttributes();
            }
            expect(TokenType::LParen, "E1305", "'(' after asm");
            const Token open = current();
            match(TokenType::LParen);
            parseRawAsmBody(open, *node);
            fillRange(*node, start);
            return node;
        }
    }

    switch (start.type) {
        case TokenType::IntegerLiteral: {
            advance();
            auto node = ecxlit::makeInteger(start.value);
            fillRange(*node, start, start);
            return node;
        }
        case TokenType::FloatLiteral: {
            advance();
            auto node = ecxlit::makeFloat(start.value);
            fillRange(*node, start, start);
            return node;
        }
        case TokenType::StringLiteral: {
            advance();
            auto node = ecxlit::makeString(start.value);
            fillRange(*node, start, start);
            return node;
        }
        case TokenType::CharLiteral: {
            advance();
            auto node = std::make_shared<AST::IntegerLiteral>();
            node->raw = start.value;
            node->value = start.value.empty()
                              ? 0
                              : static_cast<long long>(
                                    static_cast<unsigned char>(start.value[0]));
            fillRange(*node, start, start);
            return node;
        }
        case TokenType::KwTrue: {
            advance();
            auto node = ecxlit::makeBool(true);
            fillRange(*node, start, start);
            return node;
        }
        case TokenType::KwFalse: {
            advance();
            auto node = ecxlit::makeBool(false);
            fillRange(*node, start, start);
            return node;
        }
        case TokenType::KwThis: {
            advance();
            auto node = std::make_shared<AST::IdentifierExpr>();
            node->name = "this";
            fillRange(*node, start, start);
            return node;
        }
        case TokenType::Identifier: {
            if (peek().type == TokenType::LBrace && ecxAllowStructLiteral()) {
                return ecxParseStructInstantiation(*this, start);
            }
            advance();
            auto node = std::make_shared<AST::IdentifierExpr>();
            node->name = start.value;
            fillRange(*node, start, start);
            return node;
        }
        case TokenType::LParen: {
            advance();
            AST::NodePtr inner = parseExpression();
            expect(TokenType::RParen, "E1300", "')' to close grouping");
            match(TokenType::RParen);
            return inner;
        }
        case TokenType::LBracket: {
            return ecxParseArrayLiteral(*this);
        }
        case TokenType::Pipe: {
            // Anonymous function: `|params| => expr` or `|params| => { ... }`.
            // A leading `|` can only begin a lambda here (bitwise-or is infix).
            return parseLambda();
        }
        case TokenType::At: {
            advance();
            auto node = std::make_shared<AST::BuiltinCallExpr>();
            if (check(TokenType::Identifier)) {
                node->name = current().value;
                advance();
            } else {
                error("E1301", "expected builtin name after '@'",
                      "e.g. @syscall(...)");
            }
            if (check(TokenType::Lt)) {
                advance();
                while (!atEnd() && !check(TokenType::Gt)) {
                    node->genericArgs.push_back(parseTypeName());
                    if (!match(TokenType::Comma)) break;
                }
                expect(TokenType::Gt, "E1302", "'>' to close builtin generics");
                match(TokenType::Gt);
            }
            node->arguments = parseArguments();
            fillRange(*node, start);
            return node;
        }
        case TokenType::KwNew: {
            advance();
            auto node = std::make_shared<AST::NewExpression>();
            node->typeName = parseNewElementTypeName(*this);
            if (check(TokenType::LBracket)) {
                advance();
                node->arraySize = parseExpression();
                expect(TokenType::RBracket, "E1303", "']' to close new[]");
                match(TokenType::RBracket);
            }
            if (check(TokenType::LParen)) {
                node->arguments = parseArguments();
            }
            fillRange(*node, start);
            return node;
        }
        case TokenType::KwDelete: {
            advance();
            auto node = std::make_shared<AST::DeleteExpression>();
            node->operand = parseUnary();
            fillRange(*node, start);
            return node;
        }
        default:
            break;
    }

    if (start.type == TokenType::Identifier && start.value == "asm") {
        advance();
        auto node = std::make_shared<AST::InlineAsmExpr>();
        if (match(TokenType::Lt)) {
            node->returnType = parseTypeName();
            expect(TokenType::Gt, "E1304", "'>' after asm return type");
            match(TokenType::Gt);
        }
        expect(TokenType::LParen, "E1305", "'(' after asm");
        match(TokenType::LParen);
        if (check(TokenType::StringLiteral)) {
            node->templateString = current().value;
            advance();
        }
        if (match(TokenType::Comma)) {
            if (check(TokenType::StringLiteral)) {
                node->constraints = current().value;
                advance();
            }
            while (match(TokenType::Comma)) {
                node->inputs.push_back(parseExpression());
            }
        }
        expect(TokenType::RParen, "E1306", "')' to close asm");
        match(TokenType::RParen);
        fillRange(*node, start);
        return node;
    }

    error("E1307", "unexpected token in expression",
          "expected a value, name, or '('");
    auto placeholder = std::make_shared<AST::IdentifierExpr>();
    placeholder->name = "";
    fillRange(*placeholder, start, start);
    if (!atEnd()) advance();
    return placeholder;
}
