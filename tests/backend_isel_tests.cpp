// Instruction-selector tests: hand-build small Insty AST functions plus a
// matching SemaResult, run them through selection -> allocation -> lowering,
// and (where a linker is available) link + execute to verify the result.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <extra/ast.hpp>
#include <extra/type_system.hpp>
#include <sema/sema.hpp>

#include <backend/abi.hpp>
#include <backend/coff_writer.hpp>
#include <backend/elf_writer.hpp>
#include <backend/isel.hpp>
#include <backend/lower.hpp>
#include <backend/machine_code.hpp>
#include <backend/regalloc.hpp>

using namespace Backend;

static int g_failures = 0;
static void check(bool cond, const char* what) {
    std::printf(cond ? "ok: %s\n" : "FAIL: %s\n", what);
    if (!cond) ++g_failures;
}

// --- tiny AST builders -------------------------------------------------------
static AST::NodePtr intLit(long long v) {
    auto n = std::make_shared<AST::IntegerLiteral>();
    n->value = v;
    return n;
}
static AST::NodePtr floatLit(double v) {
    auto n = std::make_shared<AST::FloatLiteral>();
    n->value = v;
    return n;
}
// Float literal whose precision type is registered (so the selector picks f32 vs
// f64 storage/encoding).
static AST::NodePtr floatLitT(Sema::SemaResult& sema, Types::TypeRef ty, double v) {
    auto n = std::make_shared<AST::FloatLiteral>();
    n->value = v;
    sema.exprTypes[n.get()] = ty;
    return n;
}
static AST::NodePtr ident(const std::string& name) {
    auto n = std::make_shared<AST::IdentifierExpr>();
    n->name = name;
    return n;
}
// Identifier whose type is registered (lets the selector pick signed/unsigned
// comparisons from operand types).
static AST::NodePtr identT(Sema::SemaResult& sema, Types::TypeRef ty,
                           const std::string& name) {
    auto n = std::make_shared<AST::IdentifierExpr>();
    n->name = name;
    sema.exprTypes[n.get()] = ty;
    return n;
}
static AST::NodePtr binop(const std::string& op, AST::NodePtr l, AST::NodePtr r) {
    auto n = std::make_shared<AST::BinaryOperationExpr>();
    n->op = op;
    n->lhs = std::move(l);
    n->rhs = std::move(r);
    return n;
}
// Binary op whose result type (drives signed/unsigned div, wrap width) is
// registered in the given SemaResult.
static AST::NodePtr binopT(Sema::SemaResult& sema, Types::TypeRef ty,
                           const std::string& op, AST::NodePtr l, AST::NodePtr r) {
    auto n = std::make_shared<AST::BinaryOperationExpr>();
    n->op = op;
    n->lhs = std::move(l);
    n->rhs = std::move(r);
    sema.exprTypes[n.get()] = ty;
    return n;
}
static AST::NodePtr shiftT(Sema::SemaResult& sema, Types::TypeRef ty,
                           const std::string& op, AST::NodePtr l, AST::NodePtr r) {
    auto n = std::make_shared<AST::ShiftOperationExpr>();
    n->op = op;
    n->lhs = std::move(l);
    n->rhs = std::move(r);
    sema.exprTypes[n.get()] = ty;
    return n;
}
static AST::NodePtr unaryT(Sema::SemaResult& sema, Types::TypeRef ty,
                           const std::string& op, AST::NodePtr operand) {
    auto n = std::make_shared<AST::UnaryExpr>();
    n->op = op;
    n->operand = std::move(operand);
    sema.exprTypes[n.get()] = ty;
    return n;
}
static AST::NodePtr eqT(Sema::SemaResult& sema, Types::TypeRef ty,
                        const std::string& op, AST::NodePtr l, AST::NodePtr r) {
    auto n = std::make_shared<AST::EqualityCheckExpr>();
    n->op = op;
    n->left = std::move(l);
    n->right = std::move(r);
    sema.exprTypes[n.get()] = ty;
    return n;
}
static AST::NodePtr logicalT(Sema::SemaResult& sema, Types::TypeRef ty,
                             const std::string& op, AST::NodePtr l, AST::NodePtr r) {
    auto n = std::make_shared<AST::LogicalOperationExpr>();
    n->op = op;
    n->left = std::move(l);
    n->right = std::move(r);
    sema.exprTypes[n.get()] = ty;
    return n;
}
static AST::NodePtr ret(AST::NodePtr v) {
    auto n = std::make_shared<AST::ReturnStatement>();
    n->returnValue = std::move(v);
    return n;
}
static AST::NodePtr varDecl(const std::string& name, AST::NodePtr init) {
    auto n = std::make_shared<AST::VariableDeclarationExpr>();
    n->identifier = name;
    n->initialValue = std::move(init);
    return n;
}
static AST::NodePtr assign(const std::string& name, AST::NodePtr val) {
    auto n = std::make_shared<AST::AssignmentExpr>();
    n->target = ident(name);
    n->value = std::move(val);
    return n;
}
static std::shared_ptr<AST::FunctionCallExpr> call(const std::string& callee,
                                                   AST::NodeList args) {
    auto n = std::make_shared<AST::FunctionCallExpr>();
    n->callee = ident(callee);
    n->arguments = std::move(args);
    return n;
}
static AST::NodePtr builtin(const std::string& name, AST::NodeList args) {
    auto n = std::make_shared<AST::BuiltinCallExpr>();
    n->name = name;
    n->arguments = std::move(args);
    return n;
}
// Typed variable declaration (registers the decl node's resolved type).
static AST::NodePtr varDeclT(Sema::SemaResult& sema, Types::TypeRef ty,
                             const std::string& name, AST::NodePtr init) {
    auto n = std::make_shared<AST::VariableDeclarationExpr>();
    n->identifier = name;
    n->initialValue = std::move(init);
    sema.exprTypes[n.get()] = ty;
    return n;
}
// &x  (node type = pointer-to-operand-type, registered by caller).
static AST::NodePtr addrOf(Sema::SemaResult& sema, Types::TypeRef ptrTy,
                           AST::NodePtr operand) {
    auto n = std::make_shared<AST::AddressOfExpr>();
    n->operand = std::move(operand);
    sema.exprTypes[n.get()] = ptrTy;
    return n;
}
// *p  (node type = pointee type).
static AST::NodePtr deref(Sema::SemaResult& sema, Types::TypeRef pointee,
                          AST::NodePtr operand) {
    auto n = std::make_shared<AST::DereferenceExpr>();
    n->operand = std::move(operand);
    sema.exprTypes[n.get()] = pointee;
    return n;
}
// cast<to>(expr)  (node type = destination type).
static AST::NodePtr castT(Sema::SemaResult& sema, Types::TypeRef toTy,
                          const std::string& spelling, AST::NodePtr expr) {
    auto n = std::make_shared<AST::CastExpr>();
    n->targetType = spelling;
    n->expression = std::move(expr);
    sema.exprTypes[n.get()] = toTy;
    return n;
}
// *target = value  (deref-assignment).
static AST::NodePtr assignDeref(AST::NodePtr derefTarget, AST::NodePtr val) {
    auto n = std::make_shared<AST::AssignmentExpr>();
    n->target = std::move(derefTarget);
    n->value = std::move(val);
    return n;
}
// base[index]  (computed member access). Node type = element type.
static AST::NodePtr indexT(Sema::SemaResult& sema, Types::TypeRef elemTy,
                           AST::NodePtr base, AST::NodePtr index) {
    auto n = std::make_shared<AST::MemberAccessExpr>();
    n->object = std::move(base);
    n->property = std::move(index);
    n->computed = true;
    sema.exprTypes[n.get()] = elemTy;
    return n;
}
// target = value  (generic assignment to any lvalue target node).
static AST::NodePtr assignTo(AST::NodePtr target, AST::NodePtr val) {
    auto n = std::make_shared<AST::AssignmentExpr>();
    n->target = std::move(target);
    n->value = std::move(val);
    return n;
}
// base.field  (non-computed member access). Node type = field type.
static AST::NodePtr memberT(Sema::SemaResult& sema, Types::TypeRef fieldTy,
                            AST::NodePtr base, const std::string& field) {
    auto n = std::make_shared<AST::MemberAccessExpr>();
    n->object = std::move(base);
    auto prop = std::make_shared<AST::IdentifierExpr>();
    prop->name = field;
    n->property = std::move(prop);
    n->computed = false;
    sema.exprTypes[n.get()] = fieldTy;
    return n;
}

// Selects + allocates + lowers a function, returns success and the bytes count.
static bool buildAndCheck(const Sema::SemaResult& sema, const Sema::FunctionInfo& info,
                          MachineCode& code, Abi abi, const char* label) {
    InstructionSelector isel(sema, abi);
    std::string err;
    auto fn = isel.select(info, err);
    if (!fn) {
        std::printf("  %s: selection failed: %s\n", label, err.c_str());
        return false;
    }
    LinearScanAllocator ra(makeAbi(abi));
    Allocation alloc = ra.run(*fn);
    Lowering low(code, makeAbi(abi));
    if (!low.emit(*fn, alloc, err)) {
        std::printf("  %s: lowering failed: %s\n", label, err.c_str());
        return false;
    }
    return true;
}

static std::unique_ptr<MFunction> selectOnly(const Sema::SemaResult& sema,
                                             const Sema::FunctionInfo& info,
                                             Abi abi, const char* label) {
    InstructionSelector isel(sema, abi);
    std::string err;
    auto fn = isel.select(info, err);
    if (!fn) {
        std::printf("  %s: selection failed: %s\n", label, err.c_str());
    }
    return fn;
}

static int countOpcode(const MFunction& fn, MOpcode op) {
    int count = 0;
    for (const auto& block : fn.blocks()) {
        for (const auto& inst : block.insts) {
            if (inst.op == op) ++count;
        }
    }
    return count;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Types::TypeContext types;
    Types::TypeRef i64 = types.intType(64, true);
    Sema::SemaResult sema;
    sema.ok = true;

    // --- add(a, b) { return a + b; } -----------------------------------------
    {
        auto decl = std::make_shared<AST::FunctionDeclaration>();
        decl->name = "add";
        decl->parameters = {{"a", "i64", false}, {"b", "i64", false}};
        decl->returnType = "i64";
        decl->body = {ret(binop("+", ident("a"), ident("b")))};

        Sema::FunctionInfo info;
        info.name = "add";
        info.paramTypes = {i64, i64};
        info.paramNames = {"a", "b"};
        info.returnType = i64;
        info.decl = decl.get();

        MachineCode code;
        bool ok = buildAndCheck(sema, info, code, Abi::SystemV, "add");
        check(ok, "add(a,b) selected+lowered (SysV)");
        check(ok && !code.text.bytes.empty(), "add produced code");
        check(code.findSymbol("add") >= 0, "add symbol defined");
        std::string e;
        ElfWriter::write(code, "isel_add.o", e);

        MachineCode codeWin;
        check(buildAndCheck(sema, info, codeWin, Abi::Win64, "add"),
              "add(a,b) selected+lowered (Win64)");
        CoffWriter::write(codeWin, "isel_add.obj", e);
    }

    // --- max(a, b) { if (a > b) { return a; } return b; } --------------------
    {
        auto ifS = std::make_shared<AST::IfStatement>();
        ifS->condition = binop(">", ident("a"), ident("b"));
        ifS->consequent = {ret(ident("a"))};

        auto decl = std::make_shared<AST::FunctionDeclaration>();
        decl->name = "maxv";
        decl->parameters = {{"a", "i64", false}, {"b", "i64", false}};
        decl->returnType = "i64";
        decl->body = {ifS, ret(ident("b"))};

        Sema::FunctionInfo info;
        info.name = "maxv";
        info.paramTypes = {i64, i64};
        info.paramNames = {"a", "b"};
        info.returnType = i64;
        info.decl = decl.get();

        MachineCode code;
        bool ok = buildAndCheck(sema, info, code, Abi::SystemV, "maxv");
        check(ok, "maxv with if/else selected+lowered");
        std::string e;
        if (ok) ElfWriter::write(code, "isel_maxv.o", e);
        MachineCode codeWin;
        if (buildAndCheck(sema, info, codeWin, Abi::Win64, "maxv"))
            CoffWriter::write(codeWin, "isel_maxv.obj", e);
    }

    // --- @realloc old-size-aware form releases the old allocation ------------
    {
        Types::TypeRef u8 = types.intType(8, false);
        Types::TypeRef pU8 = types.pointerType(u8);

        auto mallocDecl = std::make_shared<AST::FunctionDeclaration>();
        mallocDecl->name = "make_buf";
        mallocDecl->returnType = "u8*";
        mallocDecl->body = {ret(builtin("malloc", {intLit(24), intLit(16)}))};

        Sema::FunctionInfo mallocInfo;
        mallocInfo.name = "make_buf";
        mallocInfo.returnType = pU8;
        mallocInfo.decl = mallocDecl.get();

        MachineCode mallocCode;
        bool mallocOk = buildAndCheck(sema, mallocInfo, mallocCode, Abi::Win64, "make_buf");
        check(mallocOk, "@malloc(size, align) lowered");
        check(mallocCode.findSymbol("HeapAlloc") >= 0, "@malloc imports HeapAlloc on Win64");
        check(mallocCode.findSymbol("HeapFree") < 0, "@malloc alone does not import HeapFree");

        auto freeDecl = std::make_shared<AST::FunctionDeclaration>();
        freeDecl->name = "drop_buf";
        freeDecl->parameters = {{"p", "u8*", false}};
        freeDecl->returnType = "i64";
        freeDecl->body = {builtin("free", {identT(sema, pU8, "p"), intLit(24), intLit(16)}),
                          ret(intLit(0))};

        Sema::FunctionInfo freeInfo;
        freeInfo.name = "drop_buf";
        freeInfo.paramTypes = {pU8};
        freeInfo.paramNames = {"p"};
        freeInfo.returnType = i64;
        freeInfo.decl = freeDecl.get();

        MachineCode freeCode;
        bool freeOk = buildAndCheck(sema, freeInfo, freeCode, Abi::Win64, "drop_buf");
        check(freeOk, "@free(ptr, size, align) lowered");
        check(freeCode.findSymbol("HeapFree") >= 0, "@free imports HeapFree on Win64");

        auto legacyDecl = std::make_shared<AST::FunctionDeclaration>();
        legacyDecl->name = "legacy_grow_buf";
        legacyDecl->parameters = {{"p", "u8*", false}};
        legacyDecl->returnType = "u8*";
        legacyDecl->body = {ret(builtin("realloc",
                                        {identT(sema, pU8, "p"), intLit(32),
                                         intLit(16)}))};

        Sema::FunctionInfo legacyInfo;
        legacyInfo.name = "legacy_grow_buf";
        legacyInfo.paramTypes = {pU8};
        legacyInfo.paramNames = {"p"};
        legacyInfo.returnType = pU8;
        legacyInfo.decl = legacyDecl.get();

        MachineCode legacyCode;
        bool legacyOk =
            buildAndCheck(sema, legacyInfo, legacyCode, Abi::Win64, "legacy_grow_buf");
        check(legacyOk, "@realloc(ptr, new_size, align) lowered");
        check(legacyCode.findSymbol("HeapFree") < 0,
              "legacy @realloc form does not release without old size");

        auto decl = std::make_shared<AST::FunctionDeclaration>();
        decl->name = "grow_buf";
        decl->parameters = {{"p", "u8*", false}};
        decl->returnType = "u8*";
        decl->body = {ret(builtin("realloc",
                                  {identT(sema, pU8, "p"), intLit(16), intLit(32),
                                   intLit(16)}))};

        Sema::FunctionInfo info;
        info.name = "grow_buf";
        info.paramTypes = {pU8};
        info.paramNames = {"p"};
        info.returnType = pU8;
        info.decl = decl.get();

        MachineCode code;
        bool ok = buildAndCheck(sema, info, code, Abi::Win64, "grow_buf");
        check(ok, "@realloc(ptr, old_size, new_size, align) lowered");
        const std::int64_t heapFree = code.findSymbol("HeapFree");
        check(heapFree >= 0 && !code.symbols[static_cast<std::size_t>(heapFree)].dll.empty(),
              "@realloc old-size form imports HeapFree on Win64");
        std::string e;
        if (ok) CoffWriter::write(code, "isel_grow_buf.obj", e);
    }

    // --- new allocation failure guards: scalar and array new return null ------
    {
        Sema::SemaResult newSema;
        newSema.ok = true;
        Types::TypeRef u8 = types.intType(8, false);
        Types::TypeRef pU8 = types.pointerType(u8);

        auto scalarNew = std::make_shared<AST::NewExpression>();
        scalarNew->typeName = "u8";
        newSema.exprTypes[scalarNew.get()] = pU8;

        auto scalarDecl = std::make_shared<AST::FunctionDeclaration>();
        scalarDecl->name = "new_byte";
        scalarDecl->returnType = "u8*";
        scalarDecl->body = {ret(scalarNew)};

        Sema::FunctionInfo scalarInfo;
        scalarInfo.name = "new_byte";
        scalarInfo.returnType = pU8;
        scalarInfo.decl = scalarDecl.get();

        auto scalarFn = selectOnly(newSema, scalarInfo, Abi::Win64, "new_byte");
        check(static_cast<bool>(scalarFn), "new scalar selected");
        check(scalarFn && countOpcode(*scalarFn, MOpcode::Jcc) >= 1,
              "new scalar guards null allocation before initialization");

        auto arrayNew = std::make_shared<AST::NewExpression>();
        arrayNew->typeName = "u8";
        arrayNew->arraySize = intLit(4);
        newSema.exprTypes[arrayNew.get()] = pU8;

        auto arrayDecl = std::make_shared<AST::FunctionDeclaration>();
        arrayDecl->name = "new_bytes";
        arrayDecl->returnType = "u8*";
        arrayDecl->body = {ret(arrayNew)};

        Sema::FunctionInfo arrayInfo;
        arrayInfo.name = "new_bytes";
        arrayInfo.returnType = pU8;
        arrayInfo.decl = arrayDecl.get();

        auto arrayFn = selectOnly(newSema, arrayInfo, Abi::Win64, "new_bytes");
        check(static_cast<bool>(arrayFn), "new array selected");
        check(arrayFn && countOpcode(*arrayFn, MOpcode::Jcc) >= 1,
              "new array guards null allocation before header writes");
        check(arrayFn && countOpcode(*arrayFn, MOpcode::Div) >= 1,
              "new array checks element-count multiplication overflow");
    }

    // --- sum_to(n) { s=0; i=1; while (i<=n) { s=s+i; i=i+1; } return s; } -----
    {
        auto whileS = std::make_shared<AST::WhileLoop>();
        whileS->condition = binop("<=", ident("i"), ident("n"));
        whileS->body = {assign("s", binop("+", ident("s"), ident("i"))),
                        assign("i", binop("+", ident("i"), intLit(1)))};

        auto decl = std::make_shared<AST::FunctionDeclaration>();
        decl->name = "sum_to";
        decl->parameters = {{"n", "i64", false}};
        decl->returnType = "i64";
        decl->body = {varDecl("s", intLit(0)), varDecl("i", intLit(1)), whileS,
                      ret(ident("s"))};

        Sema::FunctionInfo info;
        info.name = "sum_to";
        info.paramTypes = {i64};
        info.paramNames = {"n"};
        info.returnType = i64;
        info.decl = decl.get();

        MachineCode code;
        bool ok = buildAndCheck(sema, info, code, Abi::SystemV, "sum_to");
        check(ok, "sum_to with while loop selected+lowered");
        std::string e;
        if (ok) ElfWriter::write(code, "isel_sum_to.o", e);
        MachineCode codeWin;
        if (buildAndCheck(sema, info, codeWin, Abi::Win64, "sum_to"))
            CoffWriter::write(codeWin, "isel_sum_to.obj", e);
    }

    // --- sum8(a..h) = a+b+...+h  and  caller8() = sum8(1..8) -> 36 -----------
    // Exercises outgoing stack arguments (SysV: 2 on stack; Win64: 4 on stack
    // plus 32B shadow space) and incoming stack arguments in the callee.
    {
        const char* names[8] = {"a", "b", "c", "d", "e", "f", "g", "h"};
        // body: return a+b+c+d+e+f+g+h
        AST::NodePtr sum = ident(names[0]);
        for (int i = 1; i < 8; ++i) sum = binop("+", sum, ident(names[i]));
        auto sumDecl = std::make_shared<AST::FunctionDeclaration>();
        sumDecl->name = "sum8";
        for (int i = 0; i < 8; ++i)
            sumDecl->parameters.push_back({names[i], "i64", false});
        sumDecl->returnType = "i64";
        sumDecl->body = {ret(sum)};

        Sema::FunctionInfo sumInfo;
        sumInfo.name = "sum8";
        sumInfo.paramTypes = {i64, i64, i64, i64, i64, i64, i64, i64};
        sumInfo.paramNames = {names[0], names[1], names[2], names[3],
                              names[4], names[5], names[6], names[7]};
        sumInfo.returnType = i64;
        sumInfo.decl = sumDecl.get();

        // caller8() { return sum8(1,2,3,4,5,6,7,8); }
        AST::NodeList args;
        for (int i = 1; i <= 8; ++i) args.push_back(intLit(i));
        auto callExpr = call("sum8", args);
        auto callerDecl = std::make_shared<AST::FunctionDeclaration>();
        callerDecl->name = "caller8";
        callerDecl->returnType = "i64";
        callerDecl->body = {ret(callExpr)};

        Sema::FunctionInfo callerInfo;
        callerInfo.name = "caller8";
        callerInfo.returnType = i64;
        callerInfo.decl = callerDecl.get();

        // Resolve the call target symbol (normally provided by Sema).
        sema.callTargets[callExpr.get()] = "sum8";

        for (Abi abi : {Abi::SystemV, Abi::Win64}) {
            const bool win = (abi == Abi::Win64);
            MachineCode codeSum, codeCaller;
            bool okSum = buildAndCheck(sema, sumInfo, codeSum, abi, "sum8");
            bool okCaller = buildAndCheck(sema, callerInfo, codeCaller, abi, "caller8");
            check(okSum, win ? "sum8 lowered (Win64)" : "sum8 lowered (SysV)");
            check(okCaller, win ? "caller8 lowered (Win64)" : "caller8 lowered (SysV)");
            std::string e;
            if (okSum && okCaller) {
                if (win) {
                    CoffWriter::write(codeSum, "isel_sum8.obj", e);
                    CoffWriter::write(codeCaller, "isel_caller8.obj", e);
                } else {
                    ElfWriter::write(codeSum, "isel_sum8.o", e);
                    ElfWriter::write(codeCaller, "isel_caller8.o", e);
                }
            }
        }
    }

    // --- narrow integer widths: i8 wrap, u8 zero-extend ----------------------
    // addi8(a:i8,b:i8)->i8 { return a+b; }   100+100 = 200 -> wraps to -56 (i8).
    // addu8(a:u8,b:u8)->u8 { return a+b; }   200+100 = 300 -> wraps to  44 (u8).
    {
        Types::TypeRef i8 = types.intType(8, true);
        Types::TypeRef u8 = types.intType(8, false);

        auto makeAdd = [&](const char* fname, Types::TypeRef ty,
                           Sema::FunctionInfo& info,
                           std::shared_ptr<AST::FunctionDeclaration>& declOut,
                           const char* tyspell) {
            auto bo = std::make_shared<AST::BinaryOperationExpr>();
            bo->op = "+";
            bo->lhs = ident("a");
            bo->rhs = ident("b");
            sema.exprTypes[bo.get()] = ty;  // result width drives the wrap
            auto r = std::make_shared<AST::ReturnStatement>();
            r->returnValue = bo;
            auto decl = std::make_shared<AST::FunctionDeclaration>();
            decl->name = fname;
            decl->parameters = {{"a", tyspell, false}, {"b", tyspell, false}};
            decl->returnType = tyspell;
            decl->body = {r};
            declOut = decl;
            info.name = fname;
            info.paramTypes = {ty, ty};
            info.paramNames = {"a", "b"};
            info.returnType = ty;
            info.decl = decl.get();
        };

        Sema::FunctionInfo si, ui;
        std::shared_ptr<AST::FunctionDeclaration> sd, ud;
        makeAdd("addi8", i8, si, sd, "i8");
        makeAdd("addu8", u8, ui, ud, "u8");

        std::string e;
        MachineCode ci, cu;
        check(buildAndCheck(sema, si, ci, Abi::Win64, "addi8"), "addi8 lowered (Win64)");
        check(buildAndCheck(sema, ui, cu, Abi::Win64, "addu8"), "addu8 lowered (Win64)");
        CoffWriter::write(ci, "isel_addi8.obj", e);
        CoffWriter::write(cu, "isel_addu8.obj", e);
        MachineCode ci2, cu2;
        if (buildAndCheck(sema, si, ci2, Abi::SystemV, "addi8"))
            ElfWriter::write(ci2, "isel_addi8.o", e);
        if (buildAndCheck(sema, ui, cu2, Abi::SystemV, "addu8"))
            ElfWriter::write(cu2, "isel_addu8.o", e);
    }

    // --- extended operators: / % & | ^ << >> (signed+unsigned), unary - ~ ----
    // Each function f(a,b) (or u(a) for unary) returns `a OP b`. Emitted as
    // Win64 COFF and linked + executed against expected values.
    {
        Types::TypeRef i64s = types.intType(64, true);
        Types::TypeRef u64 = types.intType(64, false);

        struct Case { const char* name; const char* tyspell; Types::TypeRef ty;
                      const char* op; bool shift; bool unary; };
        const Case cases[] = {
            {"opdiv",  "i64", i64s, "/",  false, false},
            {"opmod",  "i64", i64s, "%",  false, false},
            {"opudiv", "u64", u64,  "/",  false, false},
            {"opumod", "u64", u64,  "%",  false, false},
            {"opand",  "i64", i64s, "&",  false, false},
            {"opor",   "i64", i64s, "|",  false, false},
            {"opxor",  "i64", i64s, "^",  false, false},
            {"opshl",  "i64", i64s, "<<", true,  false},
            {"opsar",  "i64", i64s, ">>", true,  false},  // signed -> arithmetic
            {"opshr",  "u64", u64,  ">>", true,  false},  // unsigned -> logical
            {"opneg",  "i64", i64s, "-",  false, true},
            {"opnot",  "i64", i64s, "~",  false, true},
        };

        std::vector<std::shared_ptr<AST::FunctionDeclaration>> keepAlive;
        for (const auto& c : cases) {
            AST::NodePtr body;
            auto decl = std::make_shared<AST::FunctionDeclaration>();
            Sema::FunctionInfo info;
            if (c.unary) {
                body = unaryT(sema, c.ty, c.op, ident("a"));
                decl->parameters = {{"a", c.tyspell, false}};
                info.paramTypes = {c.ty};
                info.paramNames = {"a"};
            } else if (c.shift) {
                body = shiftT(sema, c.ty, c.op, ident("a"), ident("b"));
                decl->parameters = {{"a", c.tyspell, false}, {"b", c.tyspell, false}};
                info.paramTypes = {c.ty, c.ty};
                info.paramNames = {"a", "b"};
            } else {
                body = binopT(sema, c.ty, c.op, ident("a"), ident("b"));
                decl->parameters = {{"a", c.tyspell, false}, {"b", c.tyspell, false}};
                info.paramTypes = {c.ty, c.ty};
                info.paramNames = {"a", "b"};
            }
            decl->name = c.name;
            decl->returnType = c.tyspell;
            decl->body = {ret(body)};
            keepAlive.push_back(decl);
            info.name = c.name;
            info.returnType = c.ty;
            info.decl = decl.get();

            std::string e;
            MachineCode code;
            bool ok = buildAndCheck(sema, info, code, Abi::Win64, c.name);
            check(ok, c.name);
            if (ok) CoffWriter::write(code, (std::string("isel_") + c.name + ".obj").c_str(), e);
        }
    }

    // --- comparison-as-value (SetCC) and logical-not `!` --------------------
    // cmplt(a,b)  -> a <  b   (signed)
    // cmpult(a,b) -> a <  b   (unsigned -> setb, not setl)
    // cmpeq(a,b)  -> a == b
    // lnot(a)     -> !a
    {
        Types::TypeRef i64s = types.intType(64, true);
        Types::TypeRef u64 = types.intType(64, false);

        auto makeCmp = [&](const char* fname, const char* tyspell, Types::TypeRef ty,
                           AST::NodePtr cmpNode) {
            auto decl = std::make_shared<AST::FunctionDeclaration>();
            decl->name = fname;
            decl->parameters = {{"a", tyspell, false}, {"b", tyspell, false}};
            decl->returnType = "i64";
            decl->body = {ret(std::move(cmpNode))};
            Sema::FunctionInfo info;
            info.name = fname;
            info.paramTypes = {ty, ty};
            info.paramNames = {"a", "b"};
            info.returnType = i64s;
            info.decl = decl.get();
            std::string e;
            MachineCode code;
            bool ok = buildAndCheck(sema, info, code, Abi::Win64, fname);
            check(ok, fname);
            if (ok) CoffWriter::write(code, (std::string("isel_") + fname + ".obj").c_str(), e);
            return decl;  // keep the declaration alive
        };

        auto d1 = makeCmp("cmplt", "i64", i64s,
                          binopT(sema, i64s, "<", identT(sema, i64s, "a"),
                                 identT(sema, i64s, "b")));
        auto d2 = makeCmp("cmpult", "u64", u64,
                          binopT(sema, u64, "<", identT(sema, u64, "a"),
                                 identT(sema, u64, "b")));
        auto d3 = makeCmp("cmpeq", "i64", i64s,
                          eqT(sema, i64s, "==", identT(sema, i64s, "a"),
                              identT(sema, i64s, "b")));

        // lnot(a) -> !a
        {
            auto decl = std::make_shared<AST::FunctionDeclaration>();
            decl->name = "lnot";
            decl->parameters = {{"a", "i64", false}};
            decl->returnType = "i64";
            decl->body = {ret(unaryT(sema, i64s, "!", identT(sema, i64s, "a")))};
            Sema::FunctionInfo info;
            info.name = "lnot";
            info.paramTypes = {i64s};
            info.paramNames = {"a"};
            info.returnType = i64s;
            info.decl = decl.get();
            std::string e;
            MachineCode code;
            bool ok = buildAndCheck(sema, info, code, Abi::Win64, "lnot");
            check(ok, "lnot");
            if (ok) CoffWriter::write(code, "isel_lnot.obj", e);
        }
        (void)d1; (void)d2; (void)d3;
    }

    // --- short-circuit logical && / || ---------------------------------------
    {
        Sema::SemaResult sema;
        Types::TypeRef i64s = types.intType(64, true);

        auto makeLogical = [&](const char* fname, const std::string& op) {
            auto decl = std::make_shared<AST::FunctionDeclaration>();
            decl->name = fname;
            decl->parameters = {{"a", "i64", false}, {"b", "i64", false}};
            decl->returnType = "i64";
            decl->body = {ret(logicalT(sema, i64s, op,
                                       identT(sema, i64s, "a"),
                                       identT(sema, i64s, "b")))};
            Sema::FunctionInfo info;
            info.name = fname;
            info.paramTypes = {i64s, i64s};
            info.paramNames = {"a", "b"};
            info.returnType = i64s;
            info.decl = decl.get();
            std::string e;
            MachineCode code;
            bool ok = buildAndCheck(sema, info, code, Abi::Win64, fname);
            check(ok, fname);
            if (ok) CoffWriter::write(code, (std::string("isel_") + fname + ".obj").c_str(), e);
            return decl;  // keep alive
        };

        auto la = makeLogical("land", "&&");
        auto lo = makeLogical("lor", "||");
        (void)la; (void)lo;
    }

    // --- casts and pointers (address-of, dereference, store-through) ---------
    {
        Sema::SemaResult sema;
        Types::TypeRef i64s = types.intType(64, true);
        Types::TypeRef i32s = types.intType(32, true);
        Types::TypeRef u8u  = types.intType(8, false);
        Types::TypeRef pI64 = types.pointerType(i64s);
        Types::TypeRef pI32 = types.pointerType(i32s);

        std::vector<std::shared_ptr<AST::FunctionDeclaration>> keep;

        // roundtrip(x: i64): var v=x; var p=&v; *p = *p + 1; return v;
        // Exercises LeaSlot + LoadInd + StoreInd. Returns x+1.
        {
            auto decl = std::make_shared<AST::FunctionDeclaration>();
            decl->name = "roundtrip";
            decl->parameters = {{"x", "i64", false}};
            decl->returnType = "i64";
            // *p + 1
            auto loadStar = deref(sema, i64s, identT(sema, pI64, "p"));
            auto sum = binopT(sema, i64s, "+", std::move(loadStar), intLit(1));
            // *p = (*p + 1)
            auto starTarget = deref(sema, i64s, identT(sema, pI64, "p"));
            decl->body = {
                varDeclT(sema, i64s, "v", identT(sema, i64s, "x")),
                varDeclT(sema, pI64, "p", addrOf(sema, pI64, identT(sema, i64s, "v"))),
                assignDeref(std::move(starTarget), std::move(sum)),
                ret(identT(sema, i64s, "v")),
            };
            Sema::FunctionInfo info;
            info.name = "roundtrip";
            info.paramTypes = {i64s};
            info.paramNames = {"x"};
            info.returnType = i64s;
            info.decl = decl.get();
            std::string e;
            MachineCode code;
            bool ok = buildAndCheck(sema, info, code, Abi::Win64, "roundtrip");
            check(ok, "roundtrip");
            if (ok) CoffWriter::write(code, "isel_roundtrip.obj", e);
            keep.push_back(decl);
        }

        // derefload(p: i32*): return *p;   (narrow LoadInd with sign-extension)
        {
            auto decl = std::make_shared<AST::FunctionDeclaration>();
            decl->name = "derefload";
            decl->parameters = {{"p", "i32*", false}};
            decl->returnType = "i64";
            decl->body = {ret(deref(sema, i32s, identT(sema, pI32, "p")))};
            Sema::FunctionInfo info;
            info.name = "derefload";
            info.paramTypes = {pI32};
            info.paramNames = {"p"};
            info.returnType = i64s;
            info.decl = decl.get();
            std::string e;
            MachineCode code;
            bool ok = buildAndCheck(sema, info, code, Abi::Win64, "derefload");
            check(ok, "derefload");
            if (ok) CoffWriter::write(code, "isel_derefload.obj", e);
            keep.push_back(decl);
        }

        // derefstore(p: i32*, v: i64): *p = v; return 0;  (narrow StoreInd)
        {
            auto decl = std::make_shared<AST::FunctionDeclaration>();
            decl->name = "derefstore";
            decl->parameters = {{"p", "i32*", false}, {"v", "i64", false}};
            decl->returnType = "i64";
            auto starTarget = deref(sema, i32s, identT(sema, pI32, "p"));
            decl->body = {
                assignDeref(std::move(starTarget), identT(sema, i64s, "v")),
                ret(intLit(0)),
            };
            Sema::FunctionInfo info;
            info.name = "derefstore";
            info.paramTypes = {pI32, i64s};
            info.paramNames = {"p", "v"};
            info.returnType = i64s;
            info.decl = decl.get();
            std::string e;
            MachineCode code;
            bool ok = buildAndCheck(sema, info, code, Abi::Win64, "derefstore");
            check(ok, "derefstore");
            if (ok) CoffWriter::write(code, "isel_derefstore.obj", e);
            keep.push_back(decl);
        }

        // castnarrow(x: i64): return cast<u8>(x);  (truncate to 8 bits, zero-ext)
        {
            auto decl = std::make_shared<AST::FunctionDeclaration>();
            decl->name = "castnarrow";
            decl->parameters = {{"x", "i64", false}};
            decl->returnType = "i64";
            decl->body = {ret(castT(sema, u8u, "u8", identT(sema, i64s, "x")))};
            Sema::FunctionInfo info;
            info.name = "castnarrow";
            info.paramTypes = {i64s};
            info.paramNames = {"x"};
            info.returnType = i64s;
            info.decl = decl.get();
            std::string e;
            MachineCode code;
            bool ok = buildAndCheck(sema, info, code, Abi::Win64, "castnarrow");
            check(ok, "castnarrow");
            if (ok) CoffWriter::write(code, "isel_castnarrow.obj", e);
            keep.push_back(decl);
        }

        for (auto& d : keep) (void)d;
    }

    // --- array indexing & pointer arithmetic --------------------------------
    {
        Sema::SemaResult sema;
        Types::TypeRef i64s = types.intType(64, true);
        Types::TypeRef i32s = types.intType(32, true);
        Types::TypeRef u8u  = types.intType(8, false);
        Types::TypeRef pI64 = types.pointerType(i64s);
        Types::TypeRef pI32 = types.pointerType(i32s);
        Types::TypeRef pU8  = types.pointerType(u8u);

        std::vector<std::shared_ptr<AST::FunctionDeclaration>> keep;

        auto makeFn = [&](const char* fname,
                          std::vector<std::tuple<std::string, std::string, Types::TypeRef>> params,
                          AST::NodeList body) {
            auto decl = std::make_shared<AST::FunctionDeclaration>();
            decl->name = fname;
            decl->returnType = "i64";
            Sema::FunctionInfo info;
            info.name = fname;
            info.returnType = i64s;
            for (auto& [pn, ps, pt] : params) {
                decl->parameters.push_back({pn, ps, false});
                info.paramTypes.push_back(pt);
                info.paramNames.push_back(pn);
            }
            decl->body = std::move(body);
            info.decl = decl.get();
            std::string e;
            MachineCode code;
            bool ok = buildAndCheck(sema, info, code, Abi::Win64, fname);
            check(ok, fname);
            if (ok) CoffWriter::write(code, (std::string("isel_") + fname + ".obj").c_str(), e);
            keep.push_back(decl);
        };

        // idxload(p: i64*, i: i64): return p[i];   (scale by 8)
        makeFn("idxload", {{"p", "i64*", pI64}, {"i", "i64", i64s}},
               {ret(indexT(sema, i64s, identT(sema, pI64, "p"), identT(sema, i64s, "i")))});

        // idxstore(p: i64*, i: i64, v: i64): p[i] = v; return 0;
        makeFn("idxstore", {{"p", "i64*", pI64}, {"i", "i64", i64s}, {"v", "i64", i64s}},
               {assignTo(indexT(sema, i64s, identT(sema, pI64, "p"), identT(sema, i64s, "i")),
                         identT(sema, i64s, "v")),
                ret(intLit(0))});

        // idxbyte(p: u8*, i: i64): return p[i];   (scale by 1, zero-extend)
        makeFn("idxbyte", {{"p", "u8*", pU8}, {"i", "i64", i64s}},
               {ret(indexT(sema, u8u, identT(sema, pU8, "p"), identT(sema, i64s, "i")))});

        // ptradd(p: i32*, i: i64): return *(p + i);   (pointer arithmetic, scale 4)
        {
            auto sum = binopT(sema, pI32, "+", identT(sema, pI32, "p"),
                              identT(sema, i64s, "i"));
            makeFn("ptradd", {{"p", "i32*", pI32}, {"i", "i64", i64s}},
                   {ret(deref(sema, i32s, std::move(sum)))});
        }

        // ptrsub(p: i64*, i: i64): return *(p - i);  (negate index + SIB lea, scale 8)
        {
            auto diff = binopT(sema, pI64, "-", identT(sema, pI64, "p"),
                               identT(sema, i64s, "i"));
            makeFn("ptrsub", {{"p", "i64*", pI64}, {"i", "i64", i64s}},
                   {ret(deref(sema, i64s, std::move(diff)))});
        }

        // addrelem(p: i64*, i: i64): var q = &p[i]; *q = 77; return p[i];
        // Proves &a[i] yields the element address (write-through mutates p[i]).
        {
            auto elemForAddr = indexT(sema, i64s, identT(sema, pI64, "p"),
                                      identT(sema, i64s, "i"));
            auto q = addrOf(sema, pI64, std::move(elemForAddr));
            auto starQ = deref(sema, i64s, identT(sema, pI64, "q"));
            auto readBack = indexT(sema, i64s, identT(sema, pI64, "p"),
                                   identT(sema, i64s, "i"));
            makeFn("addrelem", {{"p", "i64*", pI64}, {"i", "i64", i64s}},
                   {varDeclT(sema, pI64, "q", std::move(q)),
                    assignDeref(std::move(starQ), intLit(77)),
                    ret(std::move(readBack))});
        }

        for (auto& d : keep) (void)d;
    }

    // --- constant-index folding into displacement ---------------------------
    {
        Sema::SemaResult sema;
        Types::TypeRef i64s = types.intType(64, true);
        Types::TypeRef i32s = types.intType(32, true);
        Types::TypeRef pI64 = types.pointerType(i64s);
        Types::TypeRef pI32 = types.pointerType(i32s);

        std::vector<std::shared_ptr<AST::FunctionDeclaration>> keep;
        auto makeFn = [&](const char* fname,
                          std::vector<std::tuple<std::string, std::string, Types::TypeRef>> params,
                          AST::NodeList body) {
            auto decl = std::make_shared<AST::FunctionDeclaration>();
            decl->name = fname;
            decl->returnType = "i64";
            Sema::FunctionInfo info;
            info.name = fname;
            info.returnType = i64s;
            for (auto& [pn, ps, pt] : params) {
                decl->parameters.push_back({pn, ps, false});
                info.paramTypes.push_back(pt);
                info.paramNames.push_back(pn);
            }
            decl->body = std::move(body);
            info.decl = decl.get();
            std::string e;
            MachineCode code;
            bool ok = buildAndCheck(sema, info, code, Abi::Win64, fname);
            check(ok, fname);
            if (ok) CoffWriter::write(code, (std::string("isel_") + fname + ".obj").c_str(), e);
            keep.push_back(decl);
        };

        // cload(p: i64*): return p[3];   (folds to [base + 24], no index reg)
        makeFn("cload", {{"p", "i64*", pI64}},
               {ret(indexT(sema, i64s, identT(sema, pI64, "p"), intLit(3)))});

        // cstore(p: i64*, v: i64): p[2] = v; return 0;  (store to [base + 16])
        makeFn("cstore", {{"p", "i64*", pI64}, {"v", "i64", i64s}},
               {assignTo(indexT(sema, i64s, identT(sema, pI64, "p"), intLit(2)),
                         identT(sema, i64s, "v")),
                ret(intLit(0))});

        // caddr(p: i32*): var q = &p[5]; return *q;  (lea/disp = base+20, i32)
        {
            auto q = addrOf(sema, pI32,
                            indexT(sema, i32s, identT(sema, pI32, "p"), intLit(5)));
            auto starQ = deref(sema, i32s, identT(sema, pI32, "q"));
            makeFn("caddr", {{"p", "i32*", pI32}},
                   {varDeclT(sema, pI32, "q", std::move(q)),
                    ret(std::move(starQ))});
        }

        for (auto& d : keep) (void)d;
    }

    // --- struct field access (offsets, struct-array stride, alignment) -------
    {
        Sema::SemaResult sema;
        sema.ok = true;
        Types::TypeRef i64s = types.intType(64, true);
        Types::TypeRef i8s  = types.intType(8, true);

        // struct Point { x: i64; y: i64; }   => x@0, y@8, size 16
        Types::TypeRef point  = types.namedType(Types::Kind::Struct, "Point");
        Types::TypeRef pPoint = types.pointerType(point);
        {
            Sema::StructInfo si;
            si.name = "Point";
            si.fields = {{"x", i64s}, {"y", i64s}};
            sema.structs.push_back(si);
        }
        // struct Mix { a: i8; b: i64; }      => a@0, b@8 (natural align), size 16
        Types::TypeRef mix  = types.namedType(Types::Kind::Struct, "Mix");
        Types::TypeRef pMix = types.pointerType(mix);
        {
            Sema::StructInfo si;
            si.name = "Mix";
            si.fields = {{"a", i8s}, {"b", i64s}};
            sema.structs.push_back(si);
        }

        std::vector<std::shared_ptr<AST::FunctionDeclaration>> keep;
        auto makeFn = [&](const char* fname,
                          std::vector<std::tuple<std::string, std::string, Types::TypeRef>> params,
                          AST::NodeList body) {
            auto decl = std::make_shared<AST::FunctionDeclaration>();
            decl->name = fname;
            decl->returnType = "i64";
            Sema::FunctionInfo info;
            info.name = fname;
            info.returnType = i64s;
            for (auto& [pn, ps, pt] : params) {
                decl->parameters.push_back({pn, ps, false});
                info.paramTypes.push_back(pt);
                info.paramNames.push_back(pn);
            }
            decl->body = std::move(body);
            info.decl = decl.get();
            std::string e;
            MachineCode code;
            bool ok = buildAndCheck(sema, info, code, Abi::Win64, fname);
            check(ok, fname);
            if (ok) CoffWriter::write(code, (std::string("isel_") + fname + ".obj").c_str(), e);
            keep.push_back(decl);
        };

        // Variant that returns a struct by value (sret). returnTy drives the ABI.
        auto makeFnRet = [&](const char* fname, Types::TypeRef returnTy,
                             const char* returnSpell,
                             std::vector<std::tuple<std::string, std::string, Types::TypeRef>> params,
                             AST::NodeList body) {
            auto decl = std::make_shared<AST::FunctionDeclaration>();
            decl->name = fname;
            decl->returnType = returnSpell;
            Sema::FunctionInfo info;
            info.name = fname;
            info.returnType = returnTy;
            for (auto& [pn, ps, pt] : params) {
                decl->parameters.push_back({pn, ps, false});
                info.paramTypes.push_back(pt);
                info.paramNames.push_back(pn);
            }
            decl->body = std::move(body);
            info.decl = decl.get();
            std::string e;
            MachineCode code;
            bool ok = buildAndCheck(sema, info, code, Abi::Win64, fname);
            check(ok, fname);
            if (ok) CoffWriter::write(code, (std::string("isel_") + fname + ".obj").c_str(), e);
            keep.push_back(decl);
        };
        (void)makeFnRet;
        // fldload(p: Point*): return p.y;        (load [base + 8])
        makeFn("fldload", {{"p", "Point*", pPoint}},
               {ret(memberT(sema, i64s, identT(sema, pPoint, "p"), "y"))});

        // fldstore(p: Point*, v: i64): p.x = v; return p.y;  (store [base+0]; y untouched)
        makeFn("fldstore", {{"p", "Point*", pPoint}, {"v", "i64", i64s}},
               {assignTo(memberT(sema, i64s, identT(sema, pPoint, "p"), "x"),
                         identT(sema, i64s, "v")),
                ret(memberT(sema, i64s, identT(sema, pPoint, "p"), "y"))});

        // fldaddr(p: Point*): var q = &p.y; *q = 99; return p.y;  (&p.y = base+8)
        {
            auto q = addrOf(sema, types.pointerType(i64s),
                            memberT(sema, i64s, identT(sema, pPoint, "p"), "y"));
            auto starQ = deref(sema, i64s, identT(sema, types.pointerType(i64s), "q"));
            makeFn("fldaddr", {{"p", "Point*", pPoint}},
                   {varDeclT(sema, types.pointerType(i64s), "q", std::move(q)),
                    assignDeref(std::move(starQ), intLit(99)),
                    ret(memberT(sema, i64s, identT(sema, pPoint, "p"), "y"))});
        }

        // arrfld(p: Point*, i: i64): return p[i].y;  (base + i*16 + 8, one SIB lea)
        {
            auto elem = indexT(sema, point, identT(sema, pPoint, "p"),
                               identT(sema, i64s, "i"));
            makeFn("arrfld", {{"p", "Point*", pPoint}, {"i", "i64", i64s}},
                   {ret(memberT(sema, i64s, std::move(elem), "y"))});
        }

        // arrfldc(p: Point*): return p[2].x;  (folds to disp base + 2*16 + 0 = 32)
        {
            auto elem = indexT(sema, point, identT(sema, pPoint, "p"), intLit(2));
            makeFn("arrfldc", {{"p", "Point*", pPoint}},
                   {ret(memberT(sema, i64s, std::move(elem), "x"))});
        }

        // mixfld(p: Mix*): return p.b;  (b aligned to 8 => load [base + 8])
        makeFn("mixfld", {{"p", "Mix*", pMix}},
               {ret(memberT(sema, i64s, identT(sema, pMix, "p"), "b"))});

        // localstruct(): var s: Point; s.x = 5; s.y = 7; return s.x + s.y;  => 12
        // Exercises an inline struct VALUE local (slot of sizeOf(Point)=16) whose
        // field lvalues are computed from the slot address (LeaSlot + disp).
        makeFn("localstruct", {},
               {varDeclT(sema, point, "s", nullptr),
                assignTo(memberT(sema, i64s, identT(sema, point, "s"), "x"), intLit(5)),
                assignTo(memberT(sema, i64s, identT(sema, point, "s"), "y"), intLit(7)),
                ret(binopT(sema, i64s, "+",
                           memberT(sema, i64s, identT(sema, point, "s"), "x"),
                           memberT(sema, i64s, identT(sema, point, "s"), "y")))});

        // byvalparam(s: Point): return s.x + s.y;  (struct param by hidden pointer)
        // The struct param's slot holds a pointer to caller storage; field access
        // loads that pointer then adds the field offset.
        makeFn("byvalparam", {{"s", "Point", point}},
               {ret(binopT(sema, i64s, "+",
                           memberT(sema, i64s, identT(sema, point, "s"), "x"),
                           memberT(sema, i64s, identT(sema, point, "s"), "y")))});

        // mutparam(s: Point, v: i64): s.x = v; return s.x;  (writes through hidden
        // pointer -> caller's struct is mutated).
        makeFn("mutparam", {{"s", "Point", point}, {"v", "i64", i64s}},
               {assignTo(memberT(sema, i64s, identT(sema, point, "s"), "x"),
                         identT(sema, i64s, "v")),
                ret(memberT(sema, i64s, identT(sema, point, "s"), "x"))});

        // callbyval(): var s: Point; s.x = 30; s.y = 12; return byvalparam(s);
        // Struct-by-value AT THE CALL SITE: the local struct is passed by hidden
        // pointer (its address) into byvalparam, which sums the fields => 42.
        {
            auto callExpr = call("byvalparam",
                                 {identT(sema, point, "s")});
            sema.exprTypes[callExpr.get()] = i64s;
            makeFn("callbyval", {},
                   {varDeclT(sema, point, "s", nullptr),
                    assignTo(memberT(sema, i64s, identT(sema, point, "s"), "x"), intLit(30)),
                    assignTo(memberT(sema, i64s, identT(sema, point, "s"), "y"), intLit(12)),
                    ret(std::move(callExpr))});
        }

        // makepoint(a: i64, b: i64): Point { var s: Point; s.x=a; s.y=b; return s; }
        // Struct return-by-value (sret): caller passes a hidden result pointer; the
        // local struct is copied through it and the pointer returned in RAX.
        makeFnRet("makepoint", point, "Point",
                  {{"a", "i64", i64s}, {"b", "i64", i64s}},
                  {varDeclT(sema, point, "s", nullptr),
                   assignTo(memberT(sema, i64s, identT(sema, point, "s"), "x"),
                            identT(sema, i64s, "a")),
                   assignTo(memberT(sema, i64s, identT(sema, point, "s"), "y"),
                            identT(sema, i64s, "b")),
                   ret(identT(sema, point, "s"))});

        // chainsret(): return makepoint(13, 29).y;  (caller sret: allocate temp,
        // call, read field from the returned struct => 29)
        {
            auto callExpr = call("makepoint", {intLit(13), intLit(29)});
            sema.exprTypes[callExpr.get()] = point;   // call returns a Point
            makeFn("chainsret", {},
                   {ret(memberT(sema, i64s, std::move(callExpr), "y"))});
        }

        // fwdsret(a: i64, b: i64): Point { return makepoint(a, b); }
        // sret-to-sret forwarding: the inner call's result temp is copied into the
        // outer function's caller-provided result pointer.
        {
            auto callExpr = call("makepoint",
                                 {identT(sema, i64s, "a"), identT(sema, i64s, "b")});
            sema.exprTypes[callExpr.get()] = point;
            makeFnRet("fwdsret", point, "Point",
                      {{"a", "i64", i64s}, {"b", "i64", i64s}},
                      {ret(std::move(callExpr))});
        }

        // copyinit(a, b): var s: Point; s.x=a; s.y=b; var t: Point = s; s.x=999;
        //                 return t.x;   (=> a; proves t is an independent copy)
        makeFn("copyinit", {{"a", "i64", i64s}, {"b", "i64", i64s}},
               {varDeclT(sema, point, "s", nullptr),
                assignTo(memberT(sema, i64s, identT(sema, point, "s"), "x"),
                         identT(sema, i64s, "a")),
                assignTo(memberT(sema, i64s, identT(sema, point, "s"), "y"),
                         identT(sema, i64s, "b")),
                varDeclT(sema, point, "t", identT(sema, point, "s")),
                assignTo(memberT(sema, i64s, identT(sema, point, "s"), "x"), intLit(999)),
                ret(memberT(sema, i64s, identT(sema, point, "t"), "x"))});

        // copyassign(dst: Point*, src: Point*): *dst = *src; return 0;
        // Whole-struct copy through pointers (both sides are deref lvalues).
        makeFn("copyassign", {{"dst", "Point*", pPoint}, {"src", "Point*", pPoint}},
               {assignTo(deref(sema, point, identT(sema, pPoint, "dst")),
                         deref(sema, point, identT(sema, pPoint, "src"))),
                ret(intLit(0))});

        // initfromcall(a, b): var t: Point = makepoint(a, b); return t.x + t.y;
        // Copy-initialize a local from an sret call result (=> a + b).
        {
            auto callExpr = call("makepoint",
                                 {identT(sema, i64s, "a"), identT(sema, i64s, "b")});
            sema.exprTypes[callExpr.get()] = point;
            makeFn("initfromcall", {{"a", "i64", i64s}, {"b", "i64", i64s}},
                   {varDeclT(sema, point, "t", std::move(callExpr)),
                    ret(binopT(sema, i64s, "+",
                               memberT(sema, i64s, identT(sema, point, "t"), "x"),
                               memberT(sema, i64s, identT(sema, point, "t"), "y")))});
        }

        for (auto& d : keep) (void)d;
    }

    // ---- f128 modeled as a 16-byte value; arithmetic calls __*tf3 --------
    {
        Sema::SemaResult sema;
        Types::TypeRef f128 = types.floatType(128);
        Types::TypeRef i64s = types.intType(64, true);

        auto decl = std::make_shared<AST::FunctionDeclaration>();
        decl->name = "qops";
        decl->returnType = "i64";

        auto add = binopT(sema, f128, "+",
                          identT(sema, f128, "a"), identT(sema, f128, "b"));
        auto sub = binopT(sema, f128, "-",
                          identT(sema, f128, "a"), identT(sema, f128, "b"));
        auto mul = binopT(sema, f128, "*",
                          identT(sema, f128, "a"), identT(sema, f128, "b"));
        auto div = binopT(sema, f128, "/",
                          identT(sema, f128, "a"), identT(sema, f128, "b"));
        decl->body = {
            varDeclT(sema, f128, "a", floatLitT(sema, f128, 6.0)),
            varDeclT(sema, f128, "b", floatLitT(sema, f128, 2.0)),
            varDeclT(sema, f128, "s", std::move(add)),
            varDeclT(sema, f128, "d", std::move(sub)),
            varDeclT(sema, f128, "m", std::move(mul)),
            varDeclT(sema, f128, "q", std::move(div)),
            ret(intLit(0))};

        Sema::FunctionInfo info;
        info.name = "qops";
        info.returnType = i64s;
        info.decl = decl.get();

        MachineCode code;
        bool ok = buildAndCheck(sema, info, code, Abi::SystemV, "qops");
        check(ok, "qops f128 selected+lowered");
        check(ok && code.findSymbol("__addtf3") >= 0, "qops references __addtf3");
        check(ok && code.findSymbol("__subtf3") >= 0, "qops references __subtf3");
        check(ok && code.findSymbol("__multf3") >= 0, "qops references __multf3");
        check(ok && code.findSymbol("__divtf3") >= 0, "qops references __divtf3");
    }

    // ---- Floating-point (double / f64) ----------------------------------
    {
        Sema::SemaResult sema;
        Types::TypeRef f64 = types.floatType(64);
        Types::TypeRef i64s = types.intType(64, true);
        Types::TypeRef pF64 = types.pointerType(f64);

        std::vector<std::shared_ptr<AST::FunctionDeclaration>> keep;

        auto makeFnF = [&](const char* fname, Types::TypeRef retTy, const char* retSpell,
                           std::vector<std::tuple<std::string, std::string, Types::TypeRef>> params,
                           AST::NodeList body) {
            auto decl = std::make_shared<AST::FunctionDeclaration>();
            decl->name = fname;
            decl->returnType = retSpell;
            Sema::FunctionInfo info;
            info.name = fname;
            info.returnType = retTy;
            for (auto& [pn, ps, pt] : params) {
                decl->parameters.push_back({pn, ps, false});
                info.paramTypes.push_back(pt);
                info.paramNames.push_back(pn);
            }
            decl->body = std::move(body);
            info.decl = decl.get();
            std::string e;
            MachineCode code;
            bool ok = buildAndCheck(sema, info, code, Abi::Win64, fname);
            check(ok, fname);
            if (ok) CoffWriter::write(code, (std::string("isel_") + fname + ".obj").c_str(), e);
            keep.push_back(decl);
        };

        // fret(): f64 { return 3.5; }
        makeFnF("fret", f64, "f64", {}, {ret(floatLit(3.5))});

        // fadd(a: f64, b: f64): f64 { return a + b; }
        makeFnF("fadd", f64, "f64", {{"a", "f64", f64}, {"b", "f64", f64}},
                {ret(binopT(sema, f64, "+", identT(sema, f64, "a"), identT(sema, f64, "b")))});

        // fmul(a: f64, b: f64): f64 { return a * b; }
        makeFnF("fmul", f64, "f64", {{"a", "f64", f64}, {"b", "f64", f64}},
                {ret(binopT(sema, f64, "*", identT(sema, f64, "a"), identT(sema, f64, "b")))});

        // fdiv(a: f64, b: f64): f64 { return a / b; }
        makeFnF("fdiv", f64, "f64", {{"a", "f64", f64}, {"b", "f64", f64}},
                {ret(binopT(sema, f64, "/", identT(sema, f64, "a"), identT(sema, f64, "b")))});

        // fneg(a: f64): f64 { return -a; }
        makeFnF("fneg", f64, "f64", {{"a", "f64", f64}},
                {ret(unaryT(sema, f64, "-", identT(sema, f64, "a")))});

        // flocal(a: f64): f64 { var t: f64 = a * 2.0; return t + 1.0; }
        makeFnF("flocal", f64, "f64", {{"a", "f64", f64}},
                {varDeclT(sema, f64, "t",
                          binopT(sema, f64, "*", identT(sema, f64, "a"), floatLit(2.0))),
                 ret(binopT(sema, f64, "+", identT(sema, f64, "t"), floatLit(1.0)))});

        // fcmp(a: f64, b: f64): i64 { return a < b; }  (ucomisd + setb)
        makeFnF("fcmp", i64s, "i64", {{"a", "f64", f64}, {"b", "f64", f64}},
                {ret(binopT(sema, i64s, "<", identT(sema, f64, "a"), identT(sema, f64, "b")))});

        // i2f(n: i64): f64 { return (f64) n; }   (cvtsi2sd)
        makeFnF("i2f", f64, "f64", {{"n", "i64", i64s}},
                {ret(castT(sema, f64, "f64", identT(sema, i64s, "n")))});

        // f2i(a: f64): i64 { return (i64) a; }   (cvttsd2si, truncating)
        makeFnF("f2i", i64s, "i64", {{"a", "f64", f64}},
                {ret(castT(sema, i64s, "i64", identT(sema, f64, "a")))});

        // fderef(p: f64*): f64 { return *p; }   (movsd load through pointer)
        makeFnF("fderef", f64, "f64", {{"p", "f64*", pF64}},
                {ret(deref(sema, f64, identT(sema, pF64, "p")))});

        // fstore(p: f64*, v: f64): i64 { *p = v; return 0; }
        {
            auto starTarget = deref(sema, f64, identT(sema, pF64, "p"));
            makeFnF("fstore", i64s, "i64", {{"p", "f64*", pF64}, {"v", "f64", f64}},
                    {assignDeref(std::move(starTarget), identT(sema, f64, "v")),
                     ret(intLit(0))});
        }

        // fsum6(a..f: f64): f64 { return a+b+c+d+e+f; }
        // Six float args: Win64 passes the first four in XMM0-3 and the last two
        // on the stack (FStoreOutgoing / FLoad of incoming stack slot).
        {
            auto s = binopT(sema, f64, "+", identT(sema, f64, "a"), identT(sema, f64, "b"));
            s = binopT(sema, f64, "+", std::move(s), identT(sema, f64, "c"));
            s = binopT(sema, f64, "+", std::move(s), identT(sema, f64, "d"));
            s = binopT(sema, f64, "+", std::move(s), identT(sema, f64, "e"));
            s = binopT(sema, f64, "+", std::move(s), identT(sema, f64, "f"));
            makeFnF("fsum6", f64, "f64",
                    {{"a", "f64", f64}, {"b", "f64", f64}, {"c", "f64", f64},
                     {"d", "f64", f64}, {"e", "f64", f64}, {"f", "f64", f64}},
                    {ret(std::move(s))});
        }

        // fcall(a: f64, b: f64): f64 { return fadd(a, b) * 2.0; }
        // Exercises float arg passing + float return at a call site.
        {
            auto callExpr = call("fadd", {identT(sema, f64, "a"), identT(sema, f64, "b")});
            sema.exprTypes[callExpr.get()] = f64;
            makeFnF("fcall", f64, "f64", {{"a", "f64", f64}, {"b", "f64", f64}},
                    {ret(binopT(sema, f64, "*", std::move(callExpr), floatLit(2.0)))});
        }

        // fsaved(a..f: f64): f64 {
        //   return (a*b) + (c*d) + (e*f) + fadd(a, b);
        // }
        // Each product is computed into an XMM vreg, then the trailing call
        // (which clobbers caller-saved XMM) executes while those products are
        // still live -- forcing the allocator onto callee-saved XMM registers and
        // exercising the XMM prologue save / epilogue restore.
        {
            auto ab = binopT(sema, f64, "*", identT(sema, f64, "a"), identT(sema, f64, "b"));
            auto cd = binopT(sema, f64, "*", identT(sema, f64, "c"), identT(sema, f64, "d"));
            auto ef = binopT(sema, f64, "*", identT(sema, f64, "e"), identT(sema, f64, "f"));
            auto callExpr = call("fadd", {identT(sema, f64, "a"), identT(sema, f64, "b")});
            sema.exprTypes[callExpr.get()] = f64;
            // Build ((ab + cd) + ef) + call  so the products are evaluated (and
            // live) before the call sub-expression is selected.
            auto s = binopT(sema, f64, "+", std::move(ab), std::move(cd));
            s = binopT(sema, f64, "+", std::move(s), std::move(ef));
            s = binopT(sema, f64, "+", std::move(s), std::move(callExpr));
            makeFnF("fsaved", f64, "f64",
                    {{"a", "f64", f64}, {"b", "f64", f64}, {"c", "f64", f64},
                     {"d", "f64", f64}, {"e", "f64", f64}, {"f", "f64", f64}},
                    {ret(std::move(s))});
        }

        for (auto& d : keep) (void)d;
    }

    // ---- Single-precision floating-point (float / f32) ------------------
    {
        Sema::SemaResult sema;
        Types::TypeRef f32 = types.floatType(32);
        Types::TypeRef f64 = types.floatType(64);
        Types::TypeRef i64s = types.intType(64, true);
        Types::TypeRef pF32 = types.pointerType(f32);

        std::vector<std::shared_ptr<AST::FunctionDeclaration>> keep;

        auto makeFnF = [&](const char* fname, Types::TypeRef retTy, const char* retSpell,
                           std::vector<std::tuple<std::string, std::string, Types::TypeRef>> params,
                           AST::NodeList body) {
            auto decl = std::make_shared<AST::FunctionDeclaration>();
            decl->name = fname;
            decl->returnType = retSpell;
            Sema::FunctionInfo info;
            info.name = fname;
            info.returnType = retTy;
            for (auto& [pn, ps, pt] : params) {
                decl->parameters.push_back({pn, ps, false});
                info.paramTypes.push_back(pt);
                info.paramNames.push_back(pn);
            }
            decl->body = std::move(body);
            info.decl = decl.get();
            std::string e;
            MachineCode code;
            bool ok = buildAndCheck(sema, info, code, Abi::Win64, fname);
            check(ok, fname);
            if (ok) CoffWriter::write(code, (std::string("isel_") + fname + ".obj").c_str(), e);
            keep.push_back(decl);
        };

        // sret32(): f32 { return 2.5f; }
        makeFnF("sret32", f32, "f32", {}, {ret(floatLitT(sema, f32, 2.5))});

        // sadd(a: f32, b: f32): f32 { return a + b; }
        makeFnF("sadd", f32, "f32", {{"a", "f32", f32}, {"b", "f32", f32}},
                {ret(binopT(sema, f32, "+", identT(sema, f32, "a"), identT(sema, f32, "b")))});

        // smul(a: f32, b: f32): f32 { return a * b; }
        makeFnF("smul", f32, "f32", {{"a", "f32", f32}, {"b", "f32", f32}},
                {ret(binopT(sema, f32, "*", identT(sema, f32, "a"), identT(sema, f32, "b")))});

        // sdiv(a: f32, b: f32): f32 { return a / b; }
        makeFnF("sdiv", f32, "f32", {{"a", "f32", f32}, {"b", "f32", f32}},
                {ret(binopT(sema, f32, "/", identT(sema, f32, "a"), identT(sema, f32, "b")))});

        // sneg(a: f32): f32 { return -a; }
        makeFnF("sneg", f32, "f32", {{"a", "f32", f32}},
                {ret(unaryT(sema, f32, "-", identT(sema, f32, "a")))});

        // slocal(a: f32): f32 { var t: f32 = a * 2.0f; return t + 1.0f; }
        makeFnF("slocal", f32, "f32", {{"a", "f32", f32}},
                {varDeclT(sema, f32, "t",
                          binopT(sema, f32, "*", identT(sema, f32, "a"), floatLitT(sema, f32, 2.0))),
                 ret(binopT(sema, f32, "+", identT(sema, f32, "t"), floatLitT(sema, f32, 1.0)))});

        // scmp(a: f32, b: f32): i64 { return a < b; }  (ucomiss + setb)
        makeFnF("scmp", i64s, "i64", {{"a", "f32", f32}, {"b", "f32", f32}},
                {ret(binopT(sema, i64s, "<", identT(sema, f32, "a"), identT(sema, f32, "b")))});

        // i2s(n: i64): f32 { return (f32) n; }   (cvtsi2ss)
        makeFnF("i2s", f32, "f32", {{"n", "i64", i64s}},
                {ret(castT(sema, f32, "f32", identT(sema, i64s, "n")))});

        // s2i(a: f32): i64 { return (i64) a; }   (cvttss2si, truncating)
        makeFnF("s2i", i64s, "i64", {{"a", "f32", f32}},
                {ret(castT(sema, i64s, "i64", identT(sema, f32, "a")))});

        // s2d(a: f32): f64 { return (f64) a; }   (cvtss2sd, widening)
        makeFnF("s2d", f64, "f64", {{"a", "f32", f32}},
                {ret(castT(sema, f64, "f64", identT(sema, f32, "a")))});

        // d2s(a: f64): f32 { return (f32) a; }   (cvtsd2ss, narrowing)
        makeFnF("d2s", f32, "f32", {{"a", "f64", f64}},
                {ret(castT(sema, f32, "f32", identT(sema, f64, "a")))});

        // sderef(p: f32*): f32 { return *p; }   (movss load)
        makeFnF("sderef", f32, "f32", {{"p", "f32*", pF32}},
                {ret(deref(sema, f32, identT(sema, pF32, "p")))});

        // sstore(p: f32*, v: f32): i64 { *p = v; return 0; }
        {
            auto starTarget = deref(sema, f32, identT(sema, pF32, "p"));
            makeFnF("sstore", i64s, "i64", {{"p", "f32*", pF32}, {"v", "f32", f32}},
                    {assignDeref(std::move(starTarget), identT(sema, f32, "v")),
                     ret(intLit(0))});
        }

        // ssum6(a..f: f32): f32 { return a+b+c+d+e+f; }  (5th/6th args on stack)
        {
            auto s = binopT(sema, f32, "+", identT(sema, f32, "a"), identT(sema, f32, "b"));
            s = binopT(sema, f32, "+", std::move(s), identT(sema, f32, "c"));
            s = binopT(sema, f32, "+", std::move(s), identT(sema, f32, "d"));
            s = binopT(sema, f32, "+", std::move(s), identT(sema, f32, "e"));
            s = binopT(sema, f32, "+", std::move(s), identT(sema, f32, "f"));
            makeFnF("ssum6", f32, "f32",
                    {{"a", "f32", f32}, {"b", "f32", f32}, {"c", "f32", f32},
                     {"d", "f32", f32}, {"e", "f32", f32}, {"f", "f32", f32}},
                    {ret(std::move(s))});
        }

        // scall(a: f32, b: f32): f32 { return sadd(a, b) * 2.0f; }
        {
            auto callExpr = call("sadd", {identT(sema, f32, "a"), identT(sema, f32, "b")});
            sema.exprTypes[callExpr.get()] = f32;
            makeFnF("scall", f32, "f32", {{"a", "f32", f32}, {"b", "f32", f32}},
                    {ret(binopT(sema, f32, "*", std::move(callExpr), floatLitT(sema, f32, 2.0)))});
        }

        for (auto& d : keep) (void)d;
    }

    std::printf("\n%s (%d failure(s))\n", g_failures == 0 ? "PASSED" : "FAILED",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
