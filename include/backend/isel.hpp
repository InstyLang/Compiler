#pragma once

// Instruction selector: lowers the Insty AST + semantic-analysis result into
// the backend's Machine IR (one MFunction per defined function). It is the
// front of the custom backend pipeline:
//
//   AST + SemaResult  --[InstructionSelector]-->  MFunction
//                      --[LinearScanAllocator]-->  (regs assigned)
//                      --[Lowering]------------->  MachineCode (object bytes)
//
// Scope of this first pass (integer-only, by design):
//   * functions, parameters (register + stack-passed args)
//   * locals as stack slots (var decl, assignment, identifier reads)
//   * return (value + implicit)
//   * integer arithmetic +,-,* and comparisons <,>,<=,>=,==,!=
//   * function calls (System V / Win64 argument placement, RAX result)
//   * control flow: if/else and while
//
// Floats, structs/aggregates, pointers/arrays beyond raw width, and generics
// are out of scope here and reported as unsupported.

#include <map>
#include <set>
#include <string>
#include <vector>

#include <extra/ast.hpp>
#include <extra/type_system.hpp>
#include <sema/sema.hpp>

#include <backend/abi.hpp>
#include <backend/machine_ir.hpp>

namespace Backend {

class InstructionSelector {
public:
    // `wasmTarget` switches the lowerings that would otherwise reach for a
    // platform syscall or DLL import -- the allocator and @panic -- onto the
    // wasm runtime's own entry points. wasm has no syscalls, and the ELF/PE
    // import machinery does not apply.
    InstructionSelector(const Sema::SemaResult& sema, Abi abi,
                        bool instantOsSyscalls = false, bool boundsCheck = false,
                        bool aesHash = false, bool wasmTarget = false)
        : sema_(sema), abi_(makeAbi(abi)), instantOsSyscalls_(instantOsSyscalls),
          boundsCheck_(boundsCheck), aesHash_(aesHash), wasmTarget_(wasmTarget) {}

    // Symbols of the wasm runtime helpers the allocator lowering calls. The wasm
    // backend synthesizes both directly as wasm, because a bump allocator over
    // linear memory needs memory.grow and a mutable global, neither of which the
    // Machine IR can express.
    static constexpr const char* kWasmAllocSymbol = "__wasm_alloc";
    static constexpr const char* kWasmFreeSymbol = "__wasm_free";
    // The high half of an unsigned 64x64 product. x86 gets this from `mul`
    // (RDX:RAX); wasm has no widening multiply, so it is synthesized too.
    static constexpr const char* kWasmUMulHiSymbol = "__wasm_umulhi";

    // Selects machine IR for one analyzed function. Returns nullptr (and sets
    // errorOut) if the body uses an unsupported construct. `info.decl` must be
    // non-null and have a body.
    std::unique_ptr<MFunction> select(const Sema::FunctionInfo& info,
                                      std::string& errorOut);

    // Selects machine IR for one class method/constructor/operator body. Class
    // methods are not free functions (their FunctionInfo.decl is null and the
    // body lives on the AST::Method, not a FunctionDeclaration), so the module
    // emitter pairs each AST::Method with its resolved FunctionInfo (by mangled
    // name) and drives selection here. The method's first parameter is the
    // implicit `this` pointer (already present as paramTypes[0]/paramNames[0]).
    std::unique_ptr<MFunction> selectMethod(const AST::Method& method,
                                            const Sema::FunctionInfo& info,
                                            std::string& errorOut);

    // Names of internal runtime helper functions (e.g. __ins_fmt_int) that the
    // selected functions call and that must therefore also be selected+lowered
    // into the object. Populated during selection (string interpolation requests
    // the __ins_fmt_* formatters). The module emitter drains this set.
    const std::set<std::string>& requestedRuntime() const { return requestedRuntime_; }

    // Shared libraries that referenced `extern` functions declare via the
    // repeatable `[lib("name")]` directive (e.g. lib("X11")). Populated during
    // selection whenever a call to such an extern is lowered, so only libraries
    // for functions actually used end up here. The module emitter unions these
    // and the driver adds them to the ELF link line as `-l<name>`.
    const std::set<std::string>& requiredLibs() const { return requiredLibs_; }

    // ----- Aggregate calling-convention classification -----------------------
    // Exposed because a second backend has to reproduce this classification
    // exactly. The wasm emitter derives each function's wasm signature from it:
    // an aggregate classified into registers contributes one wasm parameter per
    // eightbyte, and one classified in memory contributes a hidden pointer. If
    // the two disagreed, a call would read its arguments from the wrong places.
    struct AbiEightbyte {
        bool isSSE = false;          // true => XMM (SSE), false => GP (INTEGER)
        unsigned bytes = 8;          // valid bytes in this eightbyte (1..8)
    };
    struct AggregateAbi {
        bool inMemory = true;        // true => hidden-pointer path
        std::vector<AbiEightbyte> eightbytes;  // 1 or 2 when !inMemory
        unsigned size = 0;           // sizeOf(aggregate)
    };

    // Structs, classes and slices. Scalars, pointers and `text` are not
    // aggregates: they occupy a single register.
    bool isAggregateType(Types::TypeRef t) const;

    // System V: aggregates <= 16 bytes are split into eightbytes passed in
    // registers; larger ones, and classes with a non-trivial destructor, go
    // through memory. Win64: registers only at size 1/2/4/8.
    AggregateAbi classifyAggregate(Types::TypeRef t) const;

    // Byte size / alignment of any type, following the selector's own rules
    // (pointers and text 8, slices 16).
    unsigned sizeOf(Types::TypeRef t) const;

private:
    const Sema::SemaResult& sema_;
    AbiInfo abi_;
    // When true, the syscalls emitted for @print/@println use the InstantOS
    // (rax=num, rbx, r10, rdx, r8, r9) instead of the Linux convention.
    bool instantOsSyscalls_ = false;
    // When true, slice/array indexing and sub-slicing emit runtime range checks
    // that trap (ud2) on an out-of-range access.
    bool boundsCheck_ = false;
    // When true, @hash / str.hash use the AES fast path (hardware AESENC) on this
    // (hosted x86-64) target: string literals fold with the software-AES hasher
    // and runtime strings call __ins_hash_bytes_aes. When false (e.g. freestanding
    // kernels), the portable word-wise hash is used for both. The compile-time and
    // runtime algorithms must agree per build, which this single flag guarantees.
    bool aesHash_ = false;
    // Targeting WebAssembly: no syscalls, no DLL imports (see the constructor).
    bool wasmTarget_ = false;
    std::set<std::string> requestedRuntime_;
    // Mutable: recorded from importDllFor(), which is const but is the single
    // choke point every call site uses to inspect an extern's link directives.
    mutable std::set<std::string> requiredLibs_;

    // Monotonic counter for naming string-literal symbols (.Lstr.<n>). Persists
    // across functions (the selector instance is reused for the whole module) so
    // names stay unique within the shared object.
    std::uint32_t stringCounter_ = 0;

    // --- per-function state --------------------------------------------------
    MFunction* fn_ = nullptr;
    std::uint32_t curBlock_ = 0;
    Types::TypeRef returnType_ = nullptr;
    bool failed_ = false;
    std::string error_;

    // Struct return-by-value (sret): when the current function returns an
    // aggregate, the caller passes a hidden pointer to result storage in the
    // first int arg register. We stash it in this slot; `return s` copies the
    // struct through it and returns the pointer in RAX. Explicit params then
    // start at the second arg register.

    bool sretActive_ = false;
    std::uint32_t sretSlot_ = 0;
    // When the current function returns an aggregate classified into registers
    // (no hidden sret pointer), this holds the return classification and
    // sretActive_/sretSlot_ stay unused; `return s` packs the aggregate into the
    // ABI return registers instead of copying through a pointer.
    bool sretRegReturn_ = false;
    AggregateAbi sretRetCls_;

    const Sema::ClassInfo* currentDestructorClass_ = nullptr;
    Types::TypeRef currentDestructorClassType_ = nullptr;

    // Loop target stack for `break` / `skip` (continue). Each enclosing loop
    // pushes its exit block (break target) and continue block (the header for
    // `while`, or the body top for `loop`). The innermost loop is the back.
    struct LoopTargets {
        std::uint32_t breakBlock = 0;
        std::uint32_t continueBlock = 0;
        std::size_t cleanupDepth = 0;
    };
    std::vector<LoopTargets> loopStack_;

    struct Cleanup {
        Types::TypeRef type = nullptr;
        std::uint32_t slot = 0;
    };
    std::vector<Cleanup> cleanups_;
    std::vector<std::size_t> cleanupScopeMarks_;
    std::vector<Cleanup> temporaryCleanups_;

    // Optional destination for the NEXT aggregate-returning call: when set, that
    // call constructs its sret result directly into this address (copy-elision)
    // instead of allocating a temporary. selCall/selMethodCall capture and clear
    // it before evaluating arguments; when unset they allocate an sret temp and
    // register it for destruction (so a discarded class result is not leaked).
    VReg pendingAggResultDest_ = kInvalidVReg;

    // Local variable -> its frame slot index + width/signedness. Locals live on
    // the stack (slot model); reads emit a width-aware Load (sext/zext to 64),
    // writes emit a width-aware Store (low `width` bytes).
    //
    // Aggregates (structs) are addressed, not value-loaded. A struct VALUE local
    // occupies sizeOf(struct) bytes inline in its slot (its lvalue address is the
    // slot address). A struct param is passed BY HIDDEN POINTER: its 8-byte slot
    // holds a pointer to caller-owned storage (its lvalue address is the loaded
    // pointer value).
    enum class LocalKind { Scalar, AggregateValue, AggregatePtr };
    struct LocalInfo {
        std::uint32_t slot = 0;
        std::uint8_t width = 8;
        bool isSigned = true;
        LocalKind kind = LocalKind::Scalar;
        bool isFloat = false;  // a double scalar (stored/loaded via movsd in XMM)
        Types::TypeRef type = nullptr;
    };
    std::vector<std::map<std::string, LocalInfo>> scopes_;

    void pushScope();
    void popScope();
    void maybeRegisterCleanup(Types::TypeRef type, std::uint32_t slot);
    void registerTemporaryCleanup(Types::TypeRef type, std::uint32_t slot);
    void emitCleanup(const Cleanup& cleanup);
    // `skipSlot`, when not kNoCleanupSlot, names one frame slot whose cleanup is
    // omitted from this emission. Used for NRVO: a local returned by value has
    // been moved into the caller's storage, so its destructor must not run on the
    // returning path (other paths, which don't pop the stack, still run it).
    static constexpr std::uint32_t kNoCleanupSlot = 0xFFFFFFFFu;
    void emitCleanupsTo(std::size_t depth, bool popEntries,
                        std::uint32_t skipSlot = kNoCleanupSlot);
    void emitTemporaryCleanups();
    void emitActiveCleanups(std::uint32_t skipSlot = kNoCleanupSlot);
    void emitDestructorFieldCleanups();
    void emitFunctionExitCleanups(std::uint32_t skipSlot = kNoCleanupSlot);
    // If `node` returns a named local aggregate with a non-trivial destructor by
    // value, returns that local's slot (for NRVO move-out); otherwise
    // kNoCleanupSlot.
    std::uint32_t movableReturnLocalSlot(const AST::ExprAST* node);
    void declareLocal(const std::string& name, std::uint32_t slot, std::uint8_t width,
                      bool isSigned, LocalKind kind = LocalKind::Scalar,
                      bool isFloat = false, Types::TypeRef type = nullptr);
    bool lookupLocal(const std::string& name, LocalInfo& infoOut) const;
    Types::TypeRef concreteTypeOf(const AST::ExprAST* node) const;

    // Module-level global lookup. Globals are not in any function scope; they are
    // addressed by symbol name (RIP-relative). Returns the matching GlobalInfo, or
    // nullptr if `name` is not a module global.
    const Sema::GlobalInfo* lookupGlobal(const std::string& name) const;
    // Emits a RIP-relative `Lea` of the global's symbol into a fresh GP vreg
    // holding its address (the global's lvalue).
    VReg globalAddr(const Sema::GlobalInfo& g);

    void fail(const std::string& msg);

    MBasicBlock& cur() { return fn_->block(curBlock_); }
    void emit(MInst inst) { cur().insts.push_back(std::move(inst)); }

    // statements
    void selStatement(const AST::NodePtr& stmt);
    void selBlock(const AST::NodeList& body);
    void selReturn(const AST::ReturnStatement& ret);
    void selVarDecl(const AST::VariableDeclarationExpr& decl);
    void selAssign(const AST::AssignmentExpr& a);
    void selIf(const AST::IfStatement& s);
    void selWhile(const AST::WhileLoop& s);
    // `for x in <iterable>` / `for i in a..b`: counted iteration over a slice /
    // fixed array / text, or a half-open integer range.
    void selForLoop(const AST::ForLoop& s);
    // `loop { ... }`: an unconditional infinite loop (exited via break/return).
    void selInfiniteLoop(const AST::InfiniteLoop& s);
    // `when (cond) { ... }`: a single-armed conditional (an `if` with no else).
    void selWhen(const AST::WhenStatement& s);
    // `switch subject { Variant(binds) => body, ... }`: tag dispatch + payload
    // binding for tagged-union (sum-type) values.
    void selSwitch(const AST::SwitchStatement& s);
    // `break` / `skip`: branch to the innermost loop's exit / continue block.
    void selBreak();
    void selSkip();

    // expressions: evaluate into a fresh virtual register, return its id
    VReg selExpr(const AST::NodePtr& expr);
    VReg selBinary(const std::string& op, const AST::NodePtr& l, const AST::NodePtr& r,
                   Types::TypeRef resultTy);
    VReg selShift(const AST::ShiftOperationExpr& sh);
    VReg selUnary(const AST::UnaryExpr& un);
    // Short-circuit logical && / || via a result stack slot + control flow.
    VReg selLogical(const AST::LogicalOperationExpr& lo);
    // Normalizes any integer value to a canonical 0/1 (v != 0) via Cmp + SetCC.
    VReg boolify(VReg v);
    // Evaluates a comparison into a fresh 0/1 vreg (comparison-as-value).
    VReg selComparisonValue(const AST::NodePtr& l, const AST::NodePtr& r,
                            const std::string& op);

    // Emits a Cmp for `l op r` and returns the matching (signed/unsigned) Cond.
    bool emitCompare(const AST::NodePtr& l, const AST::NodePtr& r,
                     const std::string& op, Cond& condOut);
    // If `e` is a comparison node, extracts its operands and operator.
    bool asComparison(const AST::NodePtr& e, AST::NodePtr& lOut, AST::NodePtr& rOut,
                      std::string& opOut) const;
    VReg selCall(const AST::FunctionCallExpr& call);

    // Shared body-selection core for both free functions and class methods.
    // `attributes` may be null (methods carry none). `body` is the statement
    // list to lower. `paramNames`/`paramTypes` already include `this` as element
    // 0 for methods. Returns the built MFunction or nullptr (errorOut set).
    std::unique_ptr<MFunction> selectBody(
        const std::string& symbol,
        const std::vector<AST::Attribute>* attributes,
        const std::vector<std::string>& paramNames,
        const std::vector<Types::TypeRef>& paramTypes,
        Types::TypeRef returnType, bool exported,
        const AST::NodeList& body, std::string& errorOut);

    // Emits the ABI argument marshalling, the call, and result handling for a
    // direct call to `symbol`. `argVRegs` are the already-evaluated argument
    // values in order (including any leading `this` pointer / sret pointer);
    // the parallel `argIsFloat`/`argFloatW` record float args. `resultTy` is the
    // call's sema result type. For an aggregate result the caller must have made
    // `argVRegs[0]` the sret pointer and pass `sretResult` as that same vreg
    // (returned as the call's value); pass kInvalidVReg otherwise.
    //
    // For a REGISTER-classified aggregate result, instead pass `retCls` (its
    // classification) and `retReassembleAddr` (the address of caller-owned result
    // storage). After the call, the ABI return registers are written back into
    // that storage and its address is returned as the call's value.
    VReg emitDirectCall(const std::string& symbol,
                        const std::vector<VReg>& argVRegs,
                        const std::vector<bool>& argIsFloat,
                        const std::vector<std::uint8_t>& argFloatW,
                        Types::TypeRef resultTy, VReg sretResult,
                        const AggregateAbi* retCls = nullptr,
                        VReg retReassembleAddr = kInvalidVReg);

    // If `call`'s callee is `obj.method()`, resolves the class method via sema
    // (object type -> ClassInfo -> methodMangled), computes the `this` pointer,
    // and emits the call. Returns true (result in `out`) when handled.
    bool selMethodCall(const AST::FunctionCallExpr& call, VReg& out);
    // If `call`'s callee is an identifier naming a class (constructor call form
    // `T(...)`), allocates a temp, runs the constructor (if any) with `this` =
    // temp address, and yields the temp address. Returns true when handled.
    bool selConstructorCall(const AST::FunctionCallExpr& call, VReg& out);
    // If `call` is a constructor call (`T(...)` naming a class), returns that
    // class's info; otherwise nullptr. Shared by selConstructorCall and the
    // copy-elision path so both agree on what counts as a constructor call.
    const Sema::ClassInfo* resolveConstructorCall(const AST::FunctionCallExpr& call);
    // Copy-elision: if `init` is a constructor call, run the constructor directly
    // into `destAddr` (the destination's storage) instead of a temporary, so the
    // object is constructed in place and destructed exactly once. Returns true
    // when it handled `init`; the caller then owns `destAddr`'s cleanup.
    bool tryEmitConstructorInPlace(const AST::ExprAST* init, VReg destAddr);
    // Elides an aggregate prvalue initializer directly into `destAddr`: a
    // constructor call (via tryEmitConstructorInPlace) or a call/method call that
    // returns an in-memory aggregate (by threading `destAddr` as the call's sret
    // pointer through pendingAggResultDest_). Returns true when it elided (the
    // caller owns destAddr's cleanup and must NOT also byte-copy); false leaves
    // `init` for the caller's byte-copy fallback (e.g. an lvalue source).
    bool emitAggregatePrvalueInPlace(const AST::NodePtr& init, VReg destAddr);
    // If `op` applies to a class-typed lhs with an overloaded operator method,
    // emits the operator call (this = lhs address, arg1 = rhs). Returns true
    // (result in `out`) when an operator method handled it.
    bool selClassOperator(const std::string& op, const AST::NodePtr& l,
                          const AST::NodePtr& r, VReg& out);
    // Resolves the ClassInfo for a (possibly pointer-to) class type. Returns
    // nullptr if `t` is not a class (or pointer-to-class).
    const Sema::ClassInfo* classInfoFor(Types::TypeRef t) const;

    // Identifier-call intrinsics (no `@` prefix): volatileLoad/volatileStore,
    // atomicLoad/atomicStore/atomicFence/atomicFetchAdd/atomicCompareExchange and
    // inline `asm`. Returns true if `name` was one of these (handled, result in
    // `out`); false if it is an ordinary function call. `callNode` provides the
    // sema result type for the call expression.
    bool selIntrinsicCall(const std::string& name, const AST::NodeList& arguments,
                          const AST::ExprAST& callNode, VReg& out);
    // If `name` resolves to an extern function carrying a [dll("x.dll")]
    // attribute, returns that DLL name (so the call is emitted as an indirect
    // PE-import call). Returns "" when the callee is not a DLL import.
    std::string importDllFor(const std::string& name) const;

    // Casts (int<->int width changes, int<->pointer / pointer reinterpretation).
    VReg selCast(const AST::CastExpr& cast);
    // &x : address of an addressable local (lea reg, [rbp+off]).
    VReg selAddressOf(const AST::AddressOfExpr& addr);
    // *p : load the pointee through a pointer value in a register.
    VReg selDeref(const AST::DereferenceExpr& deref);
    // Pointer arithmetic: `ptr + int` / `ptr - int` (index scaled by elem size).
    VReg selPointerArith(const std::string& op, const AST::NodePtr& l,
                         const AST::NodePtr& r, Types::TypeRef ptrTy);
    // The address of an indexed element, expressed as `base + disp` so the
    // displacement can fold into a memory operand (LoadInd/StoreInd) or a SIB
    // `lea`. For a constant index the whole `index*elemSize` collapses into disp.
    struct ElemAddr {
        VReg base = kInvalidVReg;
        std::int64_t disp = 0;
    };
    // Computes the address of base[index]; sets elemTyOut. Folds a constant index
    // into the displacement (no index register / multiply in that case).
    ElemAddr computeElementAddr(const AST::MemberAccessExpr& m, Types::TypeRef& elemTyOut);
    // General lvalue-address resolver: handles a[i], a.field, *p, pointer/array
    // identifiers, composing nested accesses (a[i].field) into one base+disp.
    ElemAddr computeLValueAddr(const AST::ExprAST* node);
    // Materializes a struct literal (Point{ x: .., y: .. }) into a fresh stack
    // temp by storing each field at its byte offset, returning that temp's
    // address. Used wherever an aggregate rvalue needs a backing address
    // (struct return, copy-init, by-value arg, field assignment source).
    ElemAddr materializeStructInstantiation(const AST::StructInstantiation& lit);
    // An array literal `[a, b, c]` materialized into a fresh stack temp: each
    // element is stored at `base + i*stride`. Returns the temp's address. Used as
    // a copy-init source and as an addressable rvalue.
    ElemAddr materializeArrayLiteral(const AST::ArrayLiteral& lit);
    // Builds a concrete slice value at `destAddr` as `{ ptr, len }`. Accepts an
    // existing slice (copy), a fixed array/array literal (ptr + static length), or
    // `new T[n]` (ptr + allocation count). Returns false on unsupported input.
    bool emitSliceInitInto(VReg destAddr, Types::TypeRef sliceTy,
                           const AST::NodePtr& init);
    // Materializes a slice expression into a fresh 16-byte temporary and returns
    // that temp's address. Used for contextual slice arguments and returns.
    ElemAddr materializeSliceValue(Types::TypeRef sliceTy, const AST::NodePtr& init);
    // Builds a sub-slice `object[start..end]` into a fresh 16-byte header and
    // returns its address. Handles slice / fixed-array / text / (unsafe) pointer
    // sources and open bounds (start defaults to 0, end to the source length).
    ElemAddr materializeSliceExpr(const AST::SliceExpr& node);
    // `new T(...)` / `new T[n](...)`: heap-allocates sizeOf(T) (times n for the
    // array form) bytes via emitMalloc, runs the class constructor (if declared)
    // with `this` bound to each element's address, and returns the pointer.
    VReg selNew(const AST::NewExpression& ne, VReg* arrayCountOut = nullptr);
    // Runs class `ci`'s constructor (or zero-initializes, if none) on the object
    // whose address is held in `thisAddr`. `arguments` are the constructor call
    // arguments (excluding `this`). `objSize` is sizeOf(T) for the zero-init path.
    // Returns false (after reporting) on error.
    bool emitConstructorInto(VReg thisAddr, const Sema::ClassInfo* ci,
                             const AST::NodeList& arguments, unsigned objSize);
    // `delete p`: runs a class destructor when `p` points at a class with one,
    // then releases the scalar allocation through the platform allocator.
    void selDelete(const AST::DeleteExpression& de);
    // a[i] / a.field / a[i].field as an rvalue: load through the lvalue address.
    VReg selMemberLoad(const AST::MemberAccessExpr& m);
    // Collapses a base+disp lvalue address into a single register (lea on disp!=0).
    VReg materializeAddr(const ElemAddr& a);
    // Copies `size` bytes from [src.base + src.disp] to [dst.base + dst.disp] as
    // unrolled width-greedy LoadInd/StoreInd chunks (8/4/2/1). Used for struct
    // value copies (sret return, by-value materialization).
    void emitStructCopy(const ElemAddr& dst, const ElemAddr& src, unsigned size);
    // Evaluates a call argument to its ABI-slot vreg; struct args pass by hidden
    // pointer (the address of their storage).
    VReg selArg(const AST::NodePtr& arg);
    // Contextual call-argument lowering for params declared as `T[]`: wrap arrays
    // or `new T[n]` into a temporary slice header before ABI expansion.
    void appendSliceArgValue(Types::TypeRef sliceTy, const AST::NodePtr& arg,
                             std::vector<VReg>& argVRegs,
                             std::vector<bool>& argIsFloat,
                             std::vector<std::uint8_t>& argFloatW,
                             std::vector<bool>& argIsAggMem);
    // Lowers a by-value aggregate argument (storage at `addr`) into the flat
    // arg-value lists used by the call placement logic, expanding it into 1-2
    // eightbyte register values (System V) / one GP value (Win64) or, when
    // classified in-memory, a single hidden-pointer value. `argIsAggMem` records,
    // per appended value, whether it is such a hidden-pointer aggregate (so the
    // placement keeps treating it as a pointer rather than a struct copy).
    //
    // For the in-memory (hidden-pointer) path, a persistent lvalue argument is
    // first copied into a fresh temporary so the callee cannot alias/mutate the
    // caller's object; `argNode` is the argument expression, used to skip that
    // copy for prvalue temporaries (constructor calls, literals, call results)
    // that are already fresh, unaliasable storage.
    void appendAggregateArgValues(Types::TypeRef ty, const ElemAddr& addr,
                                  std::vector<VReg>& argVRegs,
                                  std::vector<bool>& argIsFloat,
                                  std::vector<std::uint8_t>& argFloatW,
                                  std::vector<bool>& argIsAggMem,
                                  const AST::ExprAST* argNode = nullptr);
    // True if `node` denotes a freshly-materialized aggregate prvalue temporary
    // (a constructor/struct-returning call, struct literal, or array literal),
    // as opposed to a persistent lvalue the caller can still observe.
    static bool isAggregatePrvalueTemp(const AST::ExprAST* node);
    // Computes base + idx*elemSize + disp, folding into a SIB `lea` when elemSize
    // is 1/2/4/8 (with the constant displacement), else IMul + Add (+Add disp).
    // Returns a fresh vreg holding the address.
    VReg emitScaledAddr(VReg base, VReg idx, unsigned elemSize, std::int64_t disp = 0);
    // Bounds-check helper: traps (ud2) when `a <cond> b` holds. Indexing uses
    // UGE (idx >= len; unsigned, so a negative signed index also fails); slice
    // range checks use UGT. Only emitted under `--bounds-check`.
    void emitBoundsTrap(VReg a, VReg b, Cond trapCond);
    // Byte size of the element a pointer/array/text refers to (1 for text).
    unsigned elementSizeOf(Types::TypeRef ptrLike) const;

    // `asm [keep(reg)]( ... )`: parses the block, resolves each `$var` to the
    // local's frame slot, applies the clobber contract, and records it on the
    // MFunction for the lowering pass to assemble.
    void selAsmBlock(const AST::InlineAsmExpr& node);

    bool isSliceType(Types::TypeRef t) const;


    // --- tagged-union (sum-type) enums --------------------------------------
    // A sum type is represented as an aggregate `{ i64 tag; <payload> }`. Its
    // Insty type is a Struct whose name is registered in sema_.sumTypes.
    const Sema::SumTypeInfo* sumTypeByName(const std::string& name) const;
    bool isSumType(Types::TypeRef t) const;
    // Total aggregate size (tag + largest variant payload), rounded to 8.
    unsigned sumSizeOf(const Sema::SumTypeInfo& st) const;
    // Byte offsets (from the aggregate base, i.e. including the 8-byte tag) of
    // each payload field of `v`.
    void sumVariantFieldOffsets(const Sema::SumVariant& v,
                                std::vector<std::int64_t>& out) const;
    // Resolves `E.Variant` to its sum type + variant; false if `m` is not one.
    bool resolveSumVariant(const AST::MemberAccessExpr& m,
                           const Sema::SumTypeInfo*& stOut,
                           const Sema::SumVariant*& vOut) const;
    // Builds a variant value (tag + payload) and returns its storage address.
    // When `destBase` is valid, constructs in place there (copy elision).
    ElemAddr materializeSumConstruct(const Sema::SumTypeInfo& st,
                                     const Sema::SumVariant& v,
                                     const AST::NodeList& args,
                                     VReg destBase = kInvalidVReg);

    // Lowers a condition expression into a Cmp + the Cond for a conditional
    // branch that is TAKEN when the condition is TRUE.
    bool selCondition(const AST::NodePtr& cond, Cond& condOut);

    // maps an Insty type to a machine width in bytes (1/2/4/8).
    unsigned widthOf(Types::TypeRef t) const;
    // whether a value of this type is signed (chooses movsx vs movzx).
    bool isSignedOf(Types::TypeRef t) const;
    bool isIntegerLike(Types::TypeRef t) const;
    // whether the type is a 128-bit integer (i128/u128). These are modeled as
    // 16-byte memory values (a stack slot addressed by lo[+0] / hi[+8] halves),
    // never as a single 64-bit register.
    bool isInt128(Types::TypeRef t) const;
    // Evaluates a 128-bit integer expression, writing its 16-byte little-endian
    // result (low word at [destAddr+0], high word at [destAddr+8]) to memory.
    void selI128(const AST::NodePtr& expr, VReg destAddr);
    // Evaluates a binary128/f128 expression as a 16-byte memory value. Arithmetic
    // is delegated to the standard soft-float __*tf3 helpers.
    void selF128(const AST::NodePtr& expr, VReg destAddr);
    // Loads/stores one 64-bit half of a 128-bit value at `addr` (off 0=low,8=high).
    VReg loadHalf(VReg addr, std::int64_t off);
    void storeHalf(VReg addr, std::int64_t off, VReg value);
    // Helpers for 128-bit arithmetic, writing the 16-byte result to destAddr.
    void selI128Binary(const std::string& op, const AST::NodePtr& l,
                       const AST::NodePtr& r, VReg destAddr, bool isSigned);
    void selI128Shift(const std::string& op, const AST::NodePtr& valueExpr,
                      const AST::NodePtr& amountExpr, VReg destAddr, bool isSigned);
    // carry/borrow flag (0/1) of a 64-bit add: returns (sum <u operandBefore).
    VReg i128AddCarry(VReg sum, VReg operandBefore);
    // Evaluates a 128-bit comparison (l <op> r) into a fresh 0/1 vreg.
    VReg selI128Compare(const AST::NodePtr& l, const AST::NodePtr& r,
                        const std::string& op, bool unsignedCmp);
    // Emits a call to a by-pointer 128-bit div/mod runtime helper (result,a,b).
    void selI128DivMod(const std::string& op, VReg aAddr, VReg bAddr,
                       VReg destAddr, bool isSigned);
    // Emits a call to __addtf3/__subtf3/__multf3/__divtf3, passing f128 operands
    // in XMM0/XMM1 and storing the XMM0 result to destAddr.
    void selF128Binary(const std::string& op, const AST::NodePtr& l,
                       const AST::NodePtr& r, VReg destAddr);
    // Evaluates a call expression whose result type is i128; returns the address
    // of a 16-byte temp holding the returned value.
    VReg selCallReturningI128(const AST::NodePtr& expr);
    // Emits the four 128-bit div/mod runtime helpers into the module (once).
    void emitI128Helpers();
    // whether a value of this type is a floating-point value.
    bool isFloatType(Types::TypeRef t) const;
    // f16 (half) and f128 (quad) predicates.
    bool isFloat16(Types::TypeRef t) const;
    bool isFloat128(Types::TypeRef t) const;
    // Scalar float STORAGE/ABI width in bytes (f16 -> 2, f32 -> 4, f64 -> 8,
    // f128 -> 16).
    std::uint8_t floatWidthOf(Types::TypeRef t) const;
    // The SSE register COMPUTE width in bytes. f16 has no scalar SSE arithmetic,
    // so half values live in XMM registers as f32 (width 4) and are only packed
    // to 16 bits at memory/ABI boundaries. f32/f64 compute at their storage width.
    // (f128 is never in a scalar XMM; it is handled as a 16-byte aggregate.)
    std::uint8_t floatComputeWidthOf(Types::TypeRef t) const;
    // If `to` is f16, round an f32-register value to half precision (kept as
    // f32). Otherwise returns the value unchanged.
    VReg narrowToHalfIfNeeded(VReg v, Types::TypeRef to);
    // Emit a float MInst tagging its precision width (4=f32, 8=f64) so lowering
    // selects the single- vs double-precision SSE encoding.
    void emitFW(MOpcode op, std::vector<MOperand> operands, std::uint8_t fw);

    // String literal (text) -> a GP vreg holding the RIP-relative address of the
    // literal's NUL-terminated bytes (interned into .rodata by the lowering).
    VReg selStringLiteral(const AST::StringLiteral& lit);
    // String interpolation ("a $x ${e}"): builds a NUL-terminated text buffer on
    // the stack and fills it via the __ins_fmt_* runtime formatters. Returns a GP
    // vreg holding the buffer address (a `text`).
    VReg selInterpolation(const AST::StringLiteral& lit);
    // Formats a struct/class aggregate (whose storage is at `structAddr`) into the
    // interpolation buffer described by `bufSlot`/`offSlot`/`cap`, mirroring the
    // LLVM path's emitFormatAggregate: prefers a `toString` method if the class
    // declares one, else emits `Name { field: value, ... }` recursively. `depth`
    // guards against unbounded recursion on self-referential layouts.
    void emitFormatAggregate(std::uint32_t bufSlot, std::uint32_t offSlot,
                             unsigned cap, VReg structAddr, Types::TypeRef type,
                             unsigned depth);
    // Formats a single scalar field value `v` (of type `t`) into the interpolation
    // buffer. Shared between the top-level interpolation loop and the recursive
    // aggregate formatter. Aggregate fields are routed to emitFormatAggregate.
    void emitFormatScalar(std::uint32_t bufSlot, std::uint32_t offSlot,
                          unsigned cap, VReg v, Types::TypeRef t);
    // Appends a fixed literal string into the interpolation buffer.
    void emitFormatRawLit(std::uint32_t bufSlot, std::uint32_t offSlot,
                          unsigned cap, const std::string& s);
    // Computes the C-string length of `ptr` (bytes before the NUL) into a fresh
    // GP vreg via an inline scan loop. Shared by @strlen and interpolation.
    VReg emitStrlenOf(VReg ptr);
    // Calls an internal runtime helper by symbol name with the given integer and
    // (optionally one) float argument; records the symbol in requestedRuntime_.
    // `args` are GP/integer arg vregs in order; `floatArg` (if not kInvalidVReg)
    // is appended as an f64 XMM argument after the integer args. `returnsFloat`
    // selects the XMM vs RAX result. Returns the result vreg (kInvalidVReg for
    // void helpers).
    VReg emitRuntimeCall(const std::string& symbol, const std::vector<VReg>& args,
                         VReg floatArg, bool returnsFloat);
    // Compiler builtin (@strlen, @alignof, @bitcast, @hash, ...). Returns the
    // result vreg (kInvalidVReg for void builtins like a fire-and-forget syscall
    // used as a statement).
    VReg selBuiltinCall(const AST::BuiltinCallExpr& call);

    // Emits an inline byte-copy/fill loop (the body of @memcpy / @memset):
    //   i = 0; while (i < count) { dst[i] = (src ? src[i] : valByte); i++ }
    // When `srcPtr` is valid it copies bytes from srcPtr (memcpy); otherwise it
    // stores the low byte of `valByte` (memset). Counter lives in a frame slot so
    // it survives across the loop's basic blocks.
    void emitByteLoop(VReg destPtr, VReg srcPtr, VReg valByte, VReg count);

    // Emits a DLL-import call `fn` (from `dll`) with up to 4 integer/pointer
    // arguments placed in the ABI integer arg registers, reserving shadow space
    // (Win64). Returns a fresh vreg holding the RAX result. Used to implement
    // heap builtins on Windows (GetProcessHeap / HeapAlloc). Win64 only.
    VReg emitSimpleImportCall(const std::string& fn, const std::string& dll,
                              const std::vector<VReg>& args);

    // Allocates `size` bytes, returning a pointer vreg (0 on failure). Win64:
    // HeapAlloc(GetProcessHeap(), 0, size). SysV/Linux: anonymous mmap (syscall 9).
    VReg emitMalloc(VReg size);
    // Allocates `size` bytes with at least the requested runtime alignment.
    // align <= 16 uses emitMalloc directly. Larger alignments overallocate and
    // store the raw pointer + raw allocation size immediately before the aligned
    // user pointer so emitMaybeAlignedFree can release the original allocation.
    VReg emitAlignedMalloc(VReg size, VReg align);
    // Releases an allocation created by emitMalloc. Win64: HeapFree. SysV/Linux:
    // munmap(ptr, size) (size must match the scalar allocation size).
    void emitFree(VReg ptr, VReg size);
    // Releases either a direct emitMalloc allocation (align <= 16) or an
    // emitAlignedMalloc allocation (align > 16). Null is a no-op.
    void emitMaybeAlignedFree(VReg ptr, VReg size, VReg align = kInvalidVReg);

    // --- floating point (double / f64) --------------------------------------
    // Float literal -> an XMM-class vreg holding the constant.
    VReg selFloatLiteral(const AST::FloatLiteral& lit);
    // Float arithmetic (+ - * /) on two float operands -> XMM vreg. `fw` is the
    // precision width (4=f32, 8=f64).
    VReg selFloatBinary(const std::string& op, const AST::NodePtr& l,
                        const AST::NodePtr& r, std::uint8_t fw);

    // --- aggregate (struct/class) layout ------------------------------------
    // Resolves a struct/class type to its sema-recorded field list (by name).
    const Sema::StructInfo* structInfoFor(Types::TypeRef t) const;
    // Natural alignment / byte size (mirrors LLVM's StructType layout rule,
    // honoring the `packed` attribute). Scalars use widthOf.
    unsigned alignOf(Types::TypeRef t) const;

    // Byte offset + type of a named field within a struct/class. Returns false if
    // the type isn't an aggregate or the field is absent.
    bool fieldOffsetOf(Types::TypeRef structTy, const std::string& field,
                       std::int64_t& offsetOut, Types::TypeRef& fieldTyOut) const;

    // ----- Aggregate (struct/class) calling-convention classification -----
    //
    // Decides how a by-value aggregate is passed/returned. `Memory` means the
    // SysV/Win64 "hidden pointer" path (a caller-allocated copy addressed by
    // pointer); `Registers` means it is packed into 1-2 ABI registers.
    //
    // System V: aggregates <= 16 bytes are split into "eightbytes", each
    //   classified INTEGER (-> GP register) or SSE (-> XMM register) based on its
    //   constituent fields; > 16 bytes go in memory.
    // Win64: an aggregate is passed in ONE integer register iff its size is
    //   exactly 1/2/4/8 bytes; otherwise in memory. (No XMM/SSE classification,
    //   no two-register split.)
    // Classifies an aggregate type for the active ABI. Non-aggregate types are
    // reported as a single INTEGER eightbyte sized to the type (a convenience for
    // callers that only special-case aggregates).

    // True for a class with a non-trivial destructor (one whose destructor runs
    // via the cleanup machinery). Such types are passed/returned in memory so the
    // object keeps a single identity and is destroyed exactly once.
    bool hasNonTrivialDestructor(Types::TypeRef t) const;
    // Recursively assigns each scalar field of an aggregate to eightbyte 0/1,
    // merging SSE/INTEGER classes (used by classifyAggregate for System V).
    void classifyFields(Types::TypeRef t, unsigned baseOff,
                        bool& eb0Sse, bool& eb0HasInt,
                        bool& eb1Sse, bool& eb1HasInt) const;
};

}  // namespace Backend



