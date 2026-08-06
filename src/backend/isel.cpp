#include <backend/isel.hpp>

#include <utilities/string_hash.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <utility>

namespace Backend {

namespace {
// Caller-saved registers clobbered by a call (used to inform the allocator).
std::vector<PhysReg> callClobbers(const AbiInfo& abi) { return abi.callerSaved; }
// Rounds `v` up to the next multiple of `a` (a power of two >= 1).
unsigned alignUp(unsigned v, unsigned a) { return a <= 1 ? v : (v + a - 1) / a * a; }

// Round an f32 value to IEEE-754 half (binary16) precision and back to f32.
// Implements round-to-nearest-even, matching VCVTPS2PH with imm=0, so that a
// half literal kept in an f32 register holds exactly what f16 storage would.
float halfRoundF32(float in) {
    std::uint32_t x;
    std::memcpy(&x, &in, sizeof(x));
    std::uint32_t sign = (x >> 16) & 0x8000u;
    std::int32_t exp = static_cast<std::int32_t>((x >> 23) & 0xFF) - 127 + 15;
    std::uint32_t mant = x & 0x7FFFFFu;
    std::uint16_t half;
    if (((x >> 23) & 0xFF) == 0xFF) {
        // Inf / NaN: preserve, collapse mantissa to a quiet-NaN bit if nonzero.
        half = static_cast<std::uint16_t>(sign | 0x7C00u | (mant ? 0x200u : 0u));
    } else if (exp >= 0x1F) {
        // Overflow -> Inf.
        half = static_cast<std::uint16_t>(sign | 0x7C00u);
    } else if (exp <= 0) {
        // Subnormal or zero.
        if (exp < -10) {
            half = static_cast<std::uint16_t>(sign);
        } else {
            mant |= 0x800000u;  // restore implicit leading 1
            std::uint32_t shift = static_cast<std::uint32_t>(14 - exp);
            std::uint32_t m = mant >> shift;
            std::uint32_t rem = mant & ((1u << shift) - 1);
            std::uint32_t halfway = 1u << (shift - 1);
            if (rem > halfway || (rem == halfway && (m & 1))) ++m;  // round-to-even
            half = static_cast<std::uint16_t>(sign | m);
        }
    } else {
        std::uint32_t m = mant >> 13;
        std::uint32_t rem = mant & 0x1FFFu;
        if (rem > 0x1000u || (rem == 0x1000u && (m & 1))) {
            ++m;
            if (m == 0x400u) { m = 0; ++exp; }  // mantissa overflow carries to exp
        }
        if (exp >= 0x1F) half = static_cast<std::uint16_t>(sign | 0x7C00u);
        else half = static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(exp) << 10) | m);
    }
    // Expand the half back to f32.
    std::uint32_t hsign = (static_cast<std::uint32_t>(half) & 0x8000u) << 16;
    std::uint32_t hexp = (half >> 10) & 0x1F;
    std::uint32_t hmant = half & 0x3FFu;
    std::uint32_t out;
    if (hexp == 0) {
        if (hmant == 0) {
            out = hsign;
        } else {
            // Normalize subnormal.
            std::int32_t e = -1;
            do { hmant <<= 1; ++e; } while (!(hmant & 0x400u));
            hmant &= 0x3FFu;
            out = hsign | (static_cast<std::uint32_t>(127 - 15 - e) << 23) | (hmant << 13);
        }
    } else if (hexp == 0x1F) {
        out = hsign | 0x7F800000u | (hmant << 13);
    } else {
        out = hsign | ((hexp - 15 + 127) << 23) | (hmant << 13);
    }
    float r;
    std::memcpy(&r, &out, sizeof(r));
    return r;
}

std::pair<std::uint64_t, std::uint64_t> doubleToF128Bits(double in) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &in, sizeof(bits));
    const std::uint64_t sign = bits >> 63;
    const std::uint64_t exp = (bits >> 52) & 0x7FFu;
    const std::uint64_t frac = bits & ((1ULL << 52) - 1);

    std::uint64_t qExp = 0;
    unsigned __int128 qFrac = 0;
    if (exp == 0x7FFu) {
        qExp = 0x7FFFu;
        qFrac = static_cast<unsigned __int128>(frac) << (112 - 52);
        if (frac != 0 && qFrac == 0) qFrac = 1;
    } else if (exp == 0) {
        if (frac != 0) {
            int msb = 51;
            while (msb >= 0 && ((frac >> msb) & 1ULL) == 0) --msb;
            const int unbiased = msb - 1074;
            qExp = static_cast<std::uint64_t>(unbiased + 16383);
            const std::uint64_t tail = frac ^ (1ULL << msb);
            qFrac = static_cast<unsigned __int128>(tail) << (112 - msb);
        }
    } else {
        const int unbiased = static_cast<int>(exp) - 1023;
        qExp = static_cast<std::uint64_t>(unbiased + 16383);
        qFrac = static_cast<unsigned __int128>(frac) << (112 - 52);
    }

    const std::uint64_t lo = static_cast<std::uint64_t>(qFrac);
    const std::uint64_t hiFrac = static_cast<std::uint64_t>(qFrac >> 64) & 0x0000FFFFFFFFFFFFULL;
    const std::uint64_t hi = (sign << 63) | (qExp << 48) | hiFrac;
    return {lo, hi};
}
}  // namespace

void InstructionSelector::pushScope() {
    scopes_.emplace_back();
    cleanupScopeMarks_.push_back(cleanups_.size());
}
void InstructionSelector::popScope() {
    if (!cleanupScopeMarks_.empty()) cleanupScopeMarks_.pop_back();
    scopes_.pop_back();
}
void InstructionSelector::maybeRegisterCleanup(Types::TypeRef type, std::uint32_t slot) {
    if (!type || type->kind != Types::Kind::Class) return;
    const Sema::ClassInfo* ci = classInfoFor(type);
    if (!ci || ci->destructorMangled.empty()) return;
    cleanups_.push_back(Cleanup{type, slot});
}
void InstructionSelector::registerTemporaryCleanup(Types::TypeRef type, std::uint32_t slot) {
    if (!type || type->kind != Types::Kind::Class) return;
    const Sema::ClassInfo* ci = classInfoFor(type);
    if (!ci || ci->destructorMangled.empty()) return;
    temporaryCleanups_.push_back(Cleanup{type, slot});
}
void InstructionSelector::emitCleanup(const Cleanup& cleanup) {
    const Sema::ClassInfo* ci = classInfoFor(cleanup.type);
    if (!ci || ci->destructorMangled.empty()) return;
    VReg addr = fn_->newVReg();
    emit({MOpcode::LeaSlot, {MOperand::defVReg(addr), MOperand::slot(cleanup.slot)}});
    emitDirectCall(ci->destructorMangled, {addr}, {false}, {8},
                   /*resultTy=*/nullptr, /*sretResult=*/kInvalidVReg);
}
void InstructionSelector::emitCleanupsTo(std::size_t depth, bool popEntries,
                                         std::uint32_t skipSlot) {
    if (depth > cleanups_.size()) depth = cleanups_.size();
    for (std::size_t i = cleanups_.size(); i > depth; --i) {
        if (skipSlot != kNoCleanupSlot && cleanups_[i - 1].slot == skipSlot) continue;
        emitCleanup(cleanups_[i - 1]);
        if (failed_) return;
    }
    if (popEntries) cleanups_.resize(depth);
}
void InstructionSelector::emitTemporaryCleanups() {
    for (std::size_t i = temporaryCleanups_.size(); i > 0; --i) {
        emitCleanup(temporaryCleanups_[i - 1]);
        if (failed_) return;
    }
    temporaryCleanups_.clear();
}
void InstructionSelector::emitActiveCleanups(std::uint32_t skipSlot) {
    emitCleanupsTo(0, /*popEntries=*/false, skipSlot);
}
void InstructionSelector::emitDestructorFieldCleanups() {
    if (!currentDestructorClass_ || !currentDestructorClassType_) return;

    LocalInfo thisInfo;
    if (!lookupLocal("this", thisInfo)) {
        fail("selector: destructor missing implicit this");
        return;
    }
    VReg thisAddr = fn_->newVReg();
    MInst ldThis{MOpcode::Load,
                 {MOperand::defVReg(thisAddr), MOperand::slot(thisInfo.slot)}};
    ldThis.width = 8; ldThis.isSigned = false;
    emit(ldThis);

    for (auto it = currentDestructorClass_->fields.rbegin();
         it != currentDestructorClass_->fields.rend(); ++it) {
        const std::string& fieldName = it->first;
        Types::TypeRef fieldTy = it->second;
        if (!fieldTy || fieldTy->kind != Types::Kind::Class) continue;
        const Sema::ClassInfo* fieldClass = classInfoFor(fieldTy);
        if (!fieldClass || fieldClass->destructorMangled.empty()) continue;

        std::int64_t off = 0;
        Types::TypeRef resolvedFieldTy = nullptr;
        if (!fieldOffsetOf(currentDestructorClassType_, fieldName, off, resolvedFieldTy)) {
            fail("selector: unknown destructor field '" + fieldName + "'");
            return;
        }
        VReg fieldAddr = thisAddr;
        if (off != 0) {
            fieldAddr = fn_->newVReg();
            emit({MOpcode::LeaDisp,
                  {MOperand::defVReg(fieldAddr), MOperand::useVReg(thisAddr),
                   MOperand::immediate(off)}});
        }
        emitDirectCall(fieldClass->destructorMangled, {fieldAddr}, {false}, {8},
                       /*resultTy=*/nullptr, /*sretResult=*/kInvalidVReg);
        if (failed_) return;
    }
}
void InstructionSelector::emitFunctionExitCleanups(std::uint32_t skipSlot) {
    emitTemporaryCleanups();
    if (failed_) return;
    emitActiveCleanups(skipSlot);
    if (failed_) return;
    emitDestructorFieldCleanups();
}
std::uint32_t
InstructionSelector::movableReturnLocalSlot(const AST::ExprAST* node) {
    if (!node || node->nodeType() != AST::NodeType::IdentifierExpr)
        return kNoCleanupSlot;
    const auto& id = static_cast<const AST::IdentifierExpr&>(*node);
    LocalInfo li;
    if (!lookupLocal(id.name, li)) return kNoCleanupSlot;
    // Only a by-value aggregate local owns its storage; a struct parameter's slot
    // holds a pointer to caller storage, so it is not ours to move out.
    if (li.kind != LocalKind::AggregateValue) return kNoCleanupSlot;
    if (!hasNonTrivialDestructor(li.type)) return kNoCleanupSlot;
    return li.slot;
}
void InstructionSelector::declareLocal(const std::string& name, std::uint32_t slot,
                                       std::uint8_t width, bool isSigned, LocalKind kind,
                                       bool isFloat, Types::TypeRef type) {
    scopes_.back()[name] = LocalInfo{slot, width, isSigned, kind, isFloat, type};
}
bool InstructionSelector::lookupLocal(const std::string& name, LocalInfo& infoOut) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto f = it->find(name);
        if (f != it->end()) { infoOut = f->second; return true; }
    }
    return false;
}

Types::TypeRef InstructionSelector::concreteTypeOf(const AST::ExprAST* node) const {
    if (!node) return nullptr;
    if (node->nodeType() == AST::NodeType::IdentifierExpr) {
        const auto& id = static_cast<const AST::IdentifierExpr&>(*node);
        LocalInfo li;
        if (lookupLocal(id.name, li) && li.type) {
            return li.type;
        }
    }
    if (node->nodeType() == AST::NodeType::MemberAccess) {
        const auto& m = static_cast<const AST::MemberAccessExpr&>(*node);
        if (m.computed) {
            Types::TypeRef baseTy = concreteTypeOf(m.object.get());
            if (baseTy && (baseTy->kind == Types::Kind::Pointer ||
                           baseTy->kind == Types::Kind::Array ||
                           baseTy->kind == Types::Kind::Slice)) {
                return baseTy->element;
            }
            if (baseTy && baseTy->kind == Types::Kind::Text) {
                return sema_.typeOf(node);
            }
        }
        if (!m.computed && m.property &&
            m.property->nodeType() == AST::NodeType::IdentifierExpr) {
            const std::string& field =
                static_cast<const AST::IdentifierExpr&>(*m.property).name;
            Types::TypeRef objTy = concreteTypeOf(m.object.get());
            if (objTy && objTy->kind == Types::Kind::Pointer) {
                objTy = objTy->element;
            }
            // A slice's `.ptr`/`.len` have no struct field type; their type was
            // recorded by sema (T* / i64), so fall through to it.
            if (objTy && objTy->kind == Types::Kind::Slice) {
                return sema_.typeOf(node);
            }
            std::int64_t off = 0;
            Types::TypeRef fieldTy = nullptr;
            if (fieldOffsetOf(objTy, field, off, fieldTy)) {
                return fieldTy;
            }
        }
    }
    if (node->nodeType() == AST::NodeType::DereferenceExpr) {
        const auto& d = static_cast<const AST::DereferenceExpr&>(*node);
        Types::TypeRef ptrTy = concreteTypeOf(d.operand.get());
        if (ptrTy && ptrTy->kind == Types::Kind::Pointer) {
            return ptrTy->element;
        }
    }
    return sema_.typeOf(node);
}

const Sema::GlobalInfo* InstructionSelector::lookupGlobal(const std::string& name) const {
    for (const auto& g : sema_.globals) {
        if (g.name == name) return &g;
    }
    return nullptr;
}

VReg InstructionSelector::globalAddr(const Sema::GlobalInfo& g) {
    // The data symbol bears the global's source name (see module_emit emitGlobals).
    // `Lea` loads its RIP-relative address into a fresh GP vreg.
    VReg addr = fn_->newVReg();
    emit({MOpcode::Lea, {MOperand::defVReg(addr), MOperand::sym(g.name)}});
    return addr;
}

void InstructionSelector::fail(const std::string& msg) {
    if (!failed_) { failed_ = true; error_ = msg; }
}

bool InstructionSelector::isSliceType(Types::TypeRef t) const {
    return t && t->kind == Types::Kind::Slice;
}

bool InstructionSelector::isAggregateType(Types::TypeRef t) const {
    return t && (t->kind == Types::Kind::Struct ||
                 t->kind == Types::Kind::Class ||
                 t->kind == Types::Kind::Slice);
}

const Sema::SumTypeInfo* InstructionSelector::sumTypeByName(const std::string& name) const {
    for (const auto& st : sema_.sumTypes) {
        if (st.name == name) return &st;
    }
    return nullptr;
}

bool InstructionSelector::isSumType(Types::TypeRef t) const {
    return t && t->kind == Types::Kind::Struct && sumTypeByName(t->name) != nullptr;
}

unsigned InstructionSelector::sumSizeOf(const Sema::SumTypeInfo& st) const {
    unsigned maxPayload = 0;
    for (const auto& v : st.variants) {
        unsigned cursor = 0;
        for (Types::TypeRef ft : v.payload) {
            unsigned fa = alignOf(ft);
            if (fa == 0) fa = 1;
            cursor = alignUp(cursor, fa);
            cursor += sizeOf(ft);
        }
        maxPayload = std::max(maxPayload, cursor);
    }
    return alignUp(8u + maxPayload, 8u);
}

void InstructionSelector::sumVariantFieldOffsets(const Sema::SumVariant& v,
                                                 std::vector<std::int64_t>& out) const {
    // Tag occupies [0, 8); payload fields follow, each aligned to its own type.
    unsigned cursor = 0;
    for (Types::TypeRef ft : v.payload) {
        unsigned fa = alignOf(ft);
        if (fa == 0) fa = 1;
        cursor = alignUp(cursor, fa);
        out.push_back(static_cast<std::int64_t>(8u + cursor));
        cursor += sizeOf(ft);
    }
}

bool InstructionSelector::resolveSumVariant(const AST::MemberAccessExpr& m,
                                            const Sema::SumTypeInfo*& stOut,
                                            const Sema::SumVariant*& vOut) const {
    stOut = nullptr;
    vOut = nullptr;
    if (m.computed || !m.object) return false;
    std::string typeName;
    if (m.object->nodeType() == AST::NodeType::IdentifierExpr) {
        typeName = static_cast<const AST::IdentifierExpr&>(*m.object).name;
    } else if (m.object->nodeType() == AST::NodeType::MemberAccess) {
        auto& inner = static_cast<const AST::MemberAccessExpr&>(*m.object);
        if (inner.isScope && inner.property &&
            inner.property->nodeType() == AST::NodeType::IdentifierExpr) {
            typeName = static_cast<const AST::IdentifierExpr&>(*inner.property).name;
        }
    }
    if (typeName.empty()) return false;
    const Sema::SumTypeInfo* st = sumTypeByName(typeName);
    if (!st) return false;
    stOut = st;
    if (m.property && m.property->nodeType() == AST::NodeType::IdentifierExpr) {
        const std::string& variantName =
            static_cast<const AST::IdentifierExpr&>(*m.property).name;
        for (const auto& v : st->variants) {
            if (v.name == variantName) { vOut = &v; break; }
        }
    }
    return true;
}

InstructionSelector::ElemAddr
InstructionSelector::materializeSumConstruct(const Sema::SumTypeInfo& st,
                                             const Sema::SumVariant& v,
                                             const AST::NodeList& args,
                                             VReg destBase) {
    unsigned size = sumSizeOf(st);
    VReg base = destBase;
    if (base == kInvalidVReg) {
        std::uint32_t slot = fn_->addFrameSlot(size ? size : 8, 8, /*isSpill=*/false);
        base = fn_->newVReg();
        emit({MOpcode::LeaSlot, {MOperand::defVReg(base), MOperand::slot(slot)}});
    }
    // Store the discriminant tag at offset 0.
    VReg tag = fn_->newVReg();
    emit({MOpcode::MovRI, {MOperand::defVReg(tag), MOperand::immediate(v.tag)}});
    { MInst st0{MOpcode::StoreInd,
                {MOperand::useVReg(base), MOperand::immediate(0), MOperand::useVReg(tag)}};
      st0.width = 8; st0.isSigned = false; emit(st0); }

    std::vector<std::int64_t> offsets;
    sumVariantFieldOffsets(v, offsets);
    for (std::size_t i = 0; i < args.size() && i < offsets.size(); ++i) {
        Types::TypeRef ft = v.payload[i];
        std::int64_t off = offsets[i];
        if (isFloatType(ft)) {
            VReg fv = selExpr(args[i]);
            if (failed_) return {};
            emitFW(MOpcode::FStoreInd,
                   {MOperand::useVReg(base), MOperand::immediate(off), MOperand::useVReg(fv)},
                   floatWidthOf(ft));
        } else {
            VReg val = selExpr(args[i]);
            if (failed_) return {};
            MInst stf{MOpcode::StoreInd,
                      {MOperand::useVReg(base), MOperand::immediate(off), MOperand::useVReg(val)}};
            stf.width = static_cast<std::uint8_t>(widthOf(ft));
            stf.isSigned = isSignedOf(ft);
            emit(stf);
        }
    }
    return ElemAddr{base, 0};
}

unsigned InstructionSelector::widthOf(Types::TypeRef t) const {
    // Map an Insty integer-like type to its storage width in bytes. Pointers and
    // text are 64-bit; bool is 1 byte; enums and ints use their declared bit
    // width (rounded to a power-of-two byte width: 1/2/4/8). Unknown -> 8.
    if (!t) return 8;
    switch (t->kind) {
        case Types::Kind::Bool:
            return 1;
        case Types::Kind::Pointer:
        case Types::Kind::Text:
            return 8;
        case Types::Kind::Slice:
            return 8;
        case Types::Kind::Int:
        case Types::Kind::Enum: {
            int bits = t->bitWidth > 0 ? t->bitWidth : 64;
            if (bits <= 8) return 1;
            if (bits <= 16) return 2;
            if (bits <= 32) return 4;
            if (bits <= 64) return 8;
            return 16;  // i128/u128 occupy two 64-bit words
        }
        default:
            return 8;
    }
}

bool InstructionSelector::isInt128(Types::TypeRef t) const {
    return t && t->kind == Types::Kind::Int && t->bitWidth > 64;
}

bool InstructionSelector::isSignedOf(Types::TypeRef t) const {
    if (!t) return true;
    if (t->kind == Types::Kind::Bool) return false;  // bool is unsigned 0/1
    if (t->kind == Types::Kind::Pointer || t->kind == Types::Kind::Text) return false;
    return t->isSigned;  // Int/Enum follow their declared signedness
}

bool InstructionSelector::isIntegerLike(Types::TypeRef t) const {
    if (!t) return true;  // untyped literal context -> treat as integer
    return t->kind == Types::Kind::Int || t->kind == Types::Kind::Bool ||
           t->kind == Types::Kind::Enum;
}

bool InstructionSelector::isFloatType(Types::TypeRef t) const {
    return t && t->kind == Types::Kind::Float;
}

bool InstructionSelector::isFloat16(Types::TypeRef t) const {
    return t && t->kind == Types::Kind::Float && t->bitWidth == 16;
}

bool InstructionSelector::isFloat128(Types::TypeRef t) const {
    return t && t->kind == Types::Kind::Float && t->bitWidth == 128;
}

std::uint8_t InstructionSelector::floatWidthOf(Types::TypeRef t) const {
    // Scalar float storage/ABI width in bytes: f16 -> 2, f32 -> 4, f128 -> 16,
    // everything else (f64) -> 8.
    if (t && t->kind == Types::Kind::Float) {
        if (t->bitWidth == 16) return 2;
        if (t->bitWidth == 32) return 4;
        if (t->bitWidth == 128) return 16;
    }
    return 8;
}

std::uint8_t InstructionSelector::floatComputeWidthOf(Types::TypeRef t) const {
    // f16 is computed in f32 registers (no scalar half arithmetic on x86-64).
    if (isFloat16(t)) return 4;
    return floatWidthOf(t);
}

void InstructionSelector::emitFW(MOpcode op, std::vector<MOperand> operands,
                                 std::uint8_t fw) {
    MInst inst{op, std::move(operands)};
    inst.width = fw;
    emit(inst);
}

const Sema::StructInfo* InstructionSelector::structInfoFor(Types::TypeRef t) const {
    // A struct/class type identifies its layout by name; sema records the field
    // list (in declaration order) in SemaResult.structs. Linear search by name.
    if (!t) return nullptr;
    if (t->kind != Types::Kind::Struct && t->kind != Types::Kind::Class) return nullptr;
    for (const auto& s : sema_.structs) {
        if (s.name == t->name) return &s;
    }
    return nullptr;
}

unsigned InstructionSelector::alignOf(Types::TypeRef t) const {
    // Natural alignment. Aggregates align to their largest field; scalars align
    // to their storage width.
    if (t && t->kind == Types::Kind::Array) {
        // Fixed array aligns to its element's natural alignment.
        return t->element ? alignOf(t->element) : 1;
    }
    if (isSliceType(t)) return 8;
    if (isSumType(t)) return 8;
    if (const Sema::StructInfo* si = structInfoFor(t)) {
        if (si->packed) return 1;
        unsigned a = 1;
        for (const auto& f : si->fields) a = std::max(a, alignOf(f.second));
        return a;
    }
    // A float aligns to its own precision, matching its size above.
    if (isFloatType(t)) return floatWidthOf(t);
    if (t->kind == Types::Kind::Any) return 8;
    if (t->kind == Types::Kind::Object) return 8;
    if (t->kind == Types::Kind::Closure) return 8;
    return widthOf(t);
}

unsigned InstructionSelector::sizeOf(Types::TypeRef t) const {
    // Byte size. Mirrors LLVM's StructType layout: fields in declaration order,
    // each placed at the next offset aligned to its natural alignment (unless
    // `packed`), and the total rounded up to the struct's alignment.
    if (!t) return 8;
    if (t->kind == Types::Kind::Array) {
        // Fixed-size array: element stride * count.
        unsigned elem = t->element ? sizeOf(t->element) : 1;
        std::int64_t n = t->arrayLength > 0 ? t->arrayLength : 0;
        return static_cast<unsigned>(elem * static_cast<unsigned>(n));
    }
    if (isSliceType(t)) return 16;
    if (isSumType(t)) {
        if (const Sema::SumTypeInfo* st = sumTypeByName(t->name)) return sumSizeOf(*st);
    }
    if (const Sema::StructInfo* si = structInfoFor(t)) {
        unsigned off = 0, maxAlign = 1;
        for (const auto& f : si->fields) {
            unsigned fa = si->packed ? 1 : alignOf(f.second);
            off = alignUp(off, fa);
            off += sizeOf(f.second);
            maxAlign = std::max(maxAlign, fa);
        }
        return alignUp(off, maxAlign);
    }
    // A float occupies its precision, not a register: f16 -> 2, f32 -> 4,
    // f64 -> 8, f128 -> 16. widthOf() below is the integer-like width and would
    // answer 8 for every float, which would pad an f32 field to 8 bytes (and, worse,
    // under-allocate an f128 to 8). Backend::scalarSizeAlign already sizes float
    // globals by precision, so this is also what keeps aggregate fields and globals
    // agreeing on one layout.
    if (isFloatType(t)) return floatWidthOf(t);
    if (t->kind == Types::Kind::Any) return 24;
    if (t->kind == Types::Kind::Object) return 8;
    if (t->kind == Types::Kind::Closure) return 8;
    return widthOf(t);
}

bool InstructionSelector::fieldOffsetOf(Types::TypeRef structTy, const std::string& field,
                                        std::int64_t& offsetOut,
                                        Types::TypeRef& fieldTyOut) const {
    if (isSliceType(structTy)) {
        if (field == "ptr") {
            offsetOut = 0;
            fieldTyOut = nullptr;
            return true;
        }
        if (field == "len") {
            offsetOut = 8;
            fieldTyOut = nullptr;
            return true;
        }
        return false;
    }
    const Sema::StructInfo* si = structInfoFor(structTy);
    if (!si) return false;
    unsigned off = 0;
    for (const auto& f : si->fields) {
        unsigned fa = si->packed ? 1 : alignOf(f.second);
        off = alignUp(off, fa);
        if (f.first == field) {
            offsetOut = static_cast<std::int64_t>(off);
            fieldTyOut = f.second;
            return true;
        }
        off += sizeOf(f.second);
    }
    return false;
}

void InstructionSelector::classifyFields(Types::TypeRef t, unsigned baseOff,
                                         bool& eb0Sse, bool& eb0HasInt,
                                         bool& eb1Sse, bool& eb1HasInt) const {
    // Walk the scalar leaves of `t` (a struct/class/array/scalar) at byte offset
    // `baseOff`, marking, for each of the two eightbytes (bytes 0-7 and 8-15),
    // whether it contains a float (SSE contributor) and/or a non-float (INTEGER).
    auto markScalar = [&](unsigned off, bool isFloat) {
        if (off < 8) {
            if (isFloat) eb0Sse = true; else eb0HasInt = true;
        } else {
            if (isFloat) eb1Sse = true; else eb1HasInt = true;
        }
    };
    if (isSliceType(t)) {
        // A slice is `{ T* ptr, i64 len }`: two INTEGER eightbytes.
        for (unsigned b = 0; b < 8; ++b) markScalar(baseOff + b, false);
        for (unsigned b = 0; b < 8; ++b) markScalar(baseOff + 8 + b, false);
        return;
    }
    if (t && (t->kind == Types::Kind::Struct || t->kind == Types::Kind::Class)) {
        if (const Sema::StructInfo* si = structInfoFor(t)) {
            unsigned off = 0;
            for (const auto& f : si->fields) {
                unsigned fa = si->packed ? 1 : alignOf(f.second);
                off = alignUp(off, fa);
                classifyFields(f.second, baseOff + off, eb0Sse, eb0HasInt,
                               eb1Sse, eb1HasInt);
                off += sizeOf(f.second);
            }
        }
        return;
    }
    if (t && t->kind == Types::Kind::Array && t->element) {
        unsigned stride = sizeOf(t->element);
        std::int64_t n = t->arrayLength > 0 ? t->arrayLength : 0;
        for (std::int64_t k = 0; k < n; ++k) {
            classifyFields(t->element, baseOff + static_cast<unsigned>(k) * stride,
                           eb0Sse, eb0HasInt, eb1Sse, eb1HasInt);
        }
        return;
    }
    // Scalar leaf: a wide field (e.g. an 8-byte double straddling exactly one
    // eightbyte) contributes to whichever eightbyte(s) its bytes fall in.
    unsigned sz = sizeOf(t);
    bool isFloat = isFloatType(t);
    for (unsigned b = 0; b < sz; ++b) markScalar(baseOff + b, isFloat);
}

bool InstructionSelector::hasNonTrivialDestructor(Types::TypeRef t) const {
    if (!t || t->kind != Types::Kind::Class) return false;
    const Sema::ClassInfo* ci = classInfoFor(t);
    return ci && !ci->destructorMangled.empty();
}

InstructionSelector::AggregateAbi
InstructionSelector::classifyAggregate(Types::TypeRef t) const {
    AggregateAbi r;
    r.size = sizeOf(t);
    const bool isAggregate = isAggregateType(t);
    if (t && t->kind == Types::Kind::Any) {
        r.inMemory = true;
        return r;
    }
    if (!isAggregate) {
        // Convenience: treat a scalar as a single INTEGER eightbyte.
        r.inMemory = false;
        AbiEightbyte eb;
        eb.isSSE = isFloatType(t);
        eb.bytes = r.size ? r.size : 8;
        r.eightbytes.push_back(eb);
        return r;
    }

    // A class with a non-trivial destructor is passed and returned in memory (by
    // hidden pointer), regardless of size. A register-class return would bitwise
    // copy the object into and back out of registers, producing a second object
    // that the destructor machinery would run on -- a double destruction. The
    // memory path keeps one object identity in caller-owned storage.
    if (hasNonTrivialDestructor(t)) {
        r.inMemory = true;
        return r;
    }

    if (abi_.abi == Abi::Win64) {
        // Win64: in a single GP register only when the size is exactly 1/2/4/8.
        if (r.size == 1 || r.size == 2 || r.size == 4 || r.size == 8) {
            r.inMemory = false;
            AbiEightbyte eb;
            eb.isSSE = false;  // Win64 never classes a struct as SSE
            eb.bytes = r.size;
            r.eightbytes.push_back(eb);
        } else {
            r.inMemory = true;
        }
        return r;
    }

    // System V: > 16 bytes => memory. (A zero-size struct also goes "in memory"
    // trivially, but we never construct those.)
    if (r.size == 0 || r.size > 16) {
        r.inMemory = true;
        return r;
    }
    bool eb0Sse = false, eb0HasInt = false, eb1Sse = false, eb1HasInt = false;
    classifyFields(t, 0, eb0Sse, eb0HasInt, eb1Sse, eb1HasInt);
    r.inMemory = false;
    const unsigned nEb = (r.size > 8) ? 2 : 1;
    {
        AbiEightbyte eb0;
        // INTEGER wins over SSE when both appear in the same eightbyte.
        eb0.isSSE = eb0Sse && !eb0HasInt;
        eb0.bytes = (r.size >= 8) ? 8 : r.size;
        r.eightbytes.push_back(eb0);
    }
    if (nEb == 2) {
        AbiEightbyte eb1;
        eb1.isSSE = eb1Sse && !eb1HasInt;
        eb1.bytes = r.size - 8;  // 1..8
        r.eightbytes.push_back(eb1);
    }
    return r;
}

std::unique_ptr<MFunction> InstructionSelector::select(const Sema::FunctionInfo& info,
                                                       std::string& errorOut) {
    if (!info.decl || !info.decl->hasBody || info.isExternal) {
        errorOut = "selector: function has no body to select";
        return nullptr;
    }
    const std::string symbol = info.mangledName.empty() ? info.name : info.mangledName;
    return selectBody(symbol, &info.decl->attributes, info.paramNames, info.paramTypes,
                      info.returnType, info.isExported, info.decl->body, errorOut);
}

std::unique_ptr<MFunction> InstructionSelector::selectMethod(const AST::Method& method,
                                                             const Sema::FunctionInfo& info,
                                                             std::string& errorOut) {
    // Methods carry no function-level attributes and their body lives on the
    // AST::Method. paramNames/paramTypes from the FunctionInfo already include
    // `this` as element 0 (injected by sema), matching the call-site contract.
    const std::string symbol = info.mangledName.empty() ? info.name : info.mangledName;
    const Sema::ClassInfo* savedDtorClass = currentDestructorClass_;
    Types::TypeRef savedDtorClassType = currentDestructorClassType_;
    currentDestructorClass_ = nullptr;
    currentDestructorClassType_ = nullptr;
    if (method.isDestructor && !info.paramTypes.empty()) {
        Types::TypeRef thisTy = info.paramTypes.front();
        Types::TypeRef classTy =
            (thisTy && thisTy->kind == Types::Kind::Pointer) ? thisTy->element : nullptr;
        currentDestructorClassType_ = classTy;
        currentDestructorClass_ = classInfoFor(classTy);
    }
    auto out = selectBody(symbol, &method.attributes, info.paramNames, info.paramTypes,
                          info.returnType, info.isExported, method.body, errorOut);
    currentDestructorClass_ = savedDtorClass;
    currentDestructorClassType_ = savedDtorClassType;
    return out;
}

std::unique_ptr<MFunction> InstructionSelector::selectBody(
    const std::string& symbol,
    const std::vector<AST::Attribute>* attributes,
    const std::vector<std::string>& paramNames,
    const std::vector<Types::TypeRef>& paramTypes,
    Types::TypeRef returnType, bool exported,
    const AST::NodeList& body, std::string& errorOut) {

    auto fn = std::make_unique<MFunction>(symbol, abi_.abi);
    fn_ = fn.get();

    // Decode function-level attributes that influence symbol emission / framing.
    //   [naked]              -> no prologue/epilogue/arg spills
    //   [section("name")]    -> place code in a named section
    //   [linkage(internal|external|weak)]
    //   [conv(...)]          -> calling convention: accepted/validated only
    //                           (the backend uses a fixed ABI per target)
    fn_->exported = exported;
    if (attributes) for (const auto& attr : *attributes) {
        if (attr.name == "naked") {
            fn_->naked = true;
        } else if (attr.name == "section") {
            if (attr.value.empty()) {
                errorOut = "selector: [section] requires a name, e.g. [section(\".boot\")]";
                return nullptr;
            }
            fn_->customSection = attr.value;
        } else if (attr.name == "linkage") {
            if (attr.value == "internal" || attr.value == "private") {
                fn_->linkage = MFunction::Linkage::Internal;
            } else if (attr.value == "external") {
                fn_->linkage = MFunction::Linkage::External;
            } else if (attr.value == "weak") {
                // Source-level weak: overridable, so a strong definition
                // elsewhere wins. Not the link-once kind used for generic
                // instantiations, where all copies are identical and fold.
                fn_->linkage = MFunction::Linkage::Weak;
            } else {
                errorOut = "selector: unknown [linkage] value '" + attr.value +
                           "' (expected internal, external, or weak)";
                return nullptr;
            }
        } else if (attr.name == "conv") {
            // The custom backend emits a fixed ABI per target (SysV on ELF,
            // Win64 on COFF/PE). Accept the conventions that match that ABI and
            // the platform-default spelling; reject anything we cannot honor so
            // mismatches fail loudly rather than miscompiling.
            const std::string& c = attr.value;
            bool ok = c.empty() || c == "default" || c == "c" || c == "cdecl" ||
                      (abi_.abi == Abi::Win64 ? (c == "win64" || c == "ms")
                                              : (c == "sysv" || c == "systemv"));
            if (!ok) {
                errorOut = "selector: calling convention '" + c +
                           "' is not supported by the custom backend for this target";
                return nullptr;
            }
        }
    }

    returnType_ = returnType;
    failed_ = false;
    error_.clear();
    scopes_.clear();
    cleanups_.clear();
    cleanupScopeMarks_.clear();
    temporaryCleanups_.clear();
    loopStack_.clear();

    curBlock_ = fn_->addBlock("entry");
    pushScope();

    // Materialize parameters into stack slots. Register args are stored from
    // their ABI register; stack-passed args are copied from the caller frame.
    //
    // System V/Win64 incoming layout at function entry, before our prologue:
    //   [rsp]      return address
    //   [rsp+8..]  stack-passed args (arg N..), each 8 bytes
    // After `push rbp; mov rbp,rsp`, stack args sit at [rbp + 16 + 8*k].
    const auto& argRegs = abi_.intArgRegs;

    // A naked function gets no compiler-generated incoming-argument handling: the
    // body (typically all inline-asm) sees registers/stack exactly as the ABI
    // delivered them and manages its own frame and return.
    if (!fn_->naked) {
    // Struct return-by-value: the caller passes a hidden pointer to result storage
    // in the first int arg register. Stash it; explicit params shift down by one
    // register (regBase = 1).
    sretActive_ = false;
    sretSlot_ = 0;
    sretRegReturn_ = false;
    unsigned regBase = 0;
    if (isAggregateType(returnType_)) {
        sretRetCls_ = classifyAggregate(returnType_);
        if (!sretRetCls_.inMemory) {
            // Register return: no hidden pointer is passed; `return s` will pack
            // the aggregate into the ABI return registers. Explicit params keep
            // the full argument-register file.
            sretRegReturn_ = true;
        } else if (argRegs.empty()) {
            fail("selector: no register available for struct return pointer");
        } else {
            sretActive_ = true;
            sretSlot_ = fn_->addFrameSlot(8, 8, /*isSpill=*/false);
            MInst st{MOpcode::Store,
                     {MOperand::slot(sretSlot_), MOperand::usePhys(argRegs[0])}};
            st.width = 8; st.isSigned = false;
            emit(st);
            regBase = 1;
        }
    } else if (returnType_ && returnType_->kind == Types::Kind::Any) {
        if (argRegs.empty()) {
            fail("selector: no register available for any return pointer");
        } else {
            sretActive_ = true;
            sretSlot_ = fn_->addFrameSlot(8, 8, false);
            MInst st{MOpcode::Store,
                     {MOperand::slot(sretSlot_), MOperand::usePhys(argRegs[0])}};
            st.width = 8; st.isSigned = false;
            emit(st);
            regBase = 1;
        }
    }

    int stackArgIndex = 0;
    // ABI argument-register cursors. On Win64 the int and float register files
    // share a single positional index (the 3rd arg uses arg-reg slot 3 whether it
    // is GP or XMM); on System V they advance independently.
    unsigned gpCursor = regBase;
    unsigned xmmCursor = 0;
    const auto& xmmArgRegs = abi_.xmmArgRegs;

    // Pre-pass: when any 128-bit parameter is present, binding it dereferences a
    // pointer / reassembles halves using scratch registers that the allocator may
    // place in a not-yet-read argument register belonging to a LATER parameter.
    // To make binding order-independent, spill EVERY incoming GP argument register
    // (for any register-passed param: i128 / scalar / hidden-pointer aggregate)
    // into its own frame slot up front -- a plain reg->memory store needs no
    // colliding scratch -- then bind each parameter from the spilled slot(s).
    // Maps param index -> slot(s) holding its incoming GP register value(s).
    // Absent entries (float/SSE/stack params) are bound from their usual source.
    std::unordered_map<std::size_t, std::vector<std::uint32_t>> i128IncomingSlots;
    bool anyI128Param = false;
    for (std::size_t i = 0; i < paramTypes.size(); ++i)
        if (isInt128(paramTypes[i])) { anyI128Param = true; break; }
    if (anyI128Param) {
        unsigned gp = gpCursor, xmm = xmmCursor;
        auto spillGP = [&](std::size_t pi, unsigned count) {
            std::vector<std::uint32_t> slots;
            for (unsigned k = 0; k < count; ++k) {
                if (gp + k >= argRegs.size()) break;  // overflow -> bound from stack
                std::uint32_t s = fn_->addFrameSlot(8, 8, /*isSpill=*/false);
                MInst st{MOpcode::Store,
                         {MOperand::slot(s), MOperand::usePhys(argRegs[gp + k])}};
                st.width = 8; st.isSigned = false;
                emit(st);
                slots.push_back(s);
            }
            if (!slots.empty()) i128IncomingSlots[pi] = slots;
        };
        for (std::size_t i = 0; i < paramNames.size(); ++i) {
            Types::TypeRef pty = i < paramTypes.size() ? paramTypes[i] : nullptr;
            if (isInt128(pty)) {
                unsigned need = (abi_.abi == Abi::Win64) ? 1u : 2u;
                bool inRegs = (abi_.abi == Abi::Win64) ? (gp < argRegs.size())
                                                       : (gp + 1 < argRegs.size());
                if (inRegs) { spillGP(i, need); gp += need; if (abi_.sharedArgRegIndex) xmm += need; }
                continue;
            }
            if (isFloatType(pty)) {
                unsigned xidx = abi_.sharedArgRegIndex ? gp : xmm;
                if (xidx < xmmArgRegs.size()) { if (abi_.sharedArgRegIndex) ++gp; else ++xmm; }
                else { if (abi_.sharedArgRegIndex) ++gp; }
                continue;
            }
            const bool isAgg = isAggregateType(pty);
            if (isAgg) {
                AggregateAbi cls = classifyAggregate(pty);
                if (!cls.inMemory) {
                    // Register aggregate: spill its GP eightbytes; SSE ones stay in
                    // XMM (untouched by i128 binding's GP scratch).
                    std::vector<std::uint32_t> slots;
                    for (const auto& eb : cls.eightbytes) {
                        if (eb.isSSE) { if (abi_.sharedArgRegIndex) ++gp; else ++xmm; }
                        else {
                            if (gp < argRegs.size()) {
                                std::uint32_t s = fn_->addFrameSlot(8, 8, false);
                                MInst st{MOpcode::Store,
                                         {MOperand::slot(s), MOperand::usePhys(argRegs[gp])}};
                                st.width = 8; st.isSigned = false; emit(st);
                                slots.push_back(s);
                            }
                            ++gp; if (abi_.sharedArgRegIndex) ++xmm;
                        }
                    }
                    if (!slots.empty()) i128IncomingSlots[i] = slots;
                    continue;
                }
                // memory aggregate -> hidden pointer in one GP reg (handled below).
            }
            // Scalar / pointer / memory-aggregate-pointer: one GP register.
            if (gp < argRegs.size()) { spillGP(i, 1); ++gp; if (abi_.sharedArgRegIndex) ++xmm; }
        }
    }

    for (std::size_t i = 0; i < paramNames.size() && !failed_; ++i) {
        Types::TypeRef pty = i < paramTypes.size() ? paramTypes[i] : nullptr;

        if (isFloat128(pty)) {
            fail("selector: f128 parameter ABI is not supported yet");
            break;
        }

        // 128-bit integer parameter. ABI:
        //   SysV : two INTEGER eightbytes in two consecutive GP arg registers
        //          (low word first); if fewer than two remain, the whole value is
        //          passed on the stack (16-byte slot).
        //   Win64: passed BY REFERENCE -- one GP register holds a pointer to a
        //          caller-owned 16-byte copy; load both halves through it.
        // Either way we end up with a 16-byte inline VALUE slot (width=16 marks it
        // as i128 for selAssign), addressed via its slot.
        if (isInt128(pty)) {
            std::uint32_t slot = fn_->addFrameSlot(16, 16, /*isSpill=*/false);
            declareLocal(paramNames[i], slot, 16, pty->isSigned,
                         LocalKind::AggregateValue, false, pty);
            VReg baseAddr = fn_->newVReg();
            emit({MOpcode::LeaSlot, {MOperand::defVReg(baseAddr), MOperand::slot(slot)}});
            auto preSpilled = i128IncomingSlots.find(i);
            const bool inRegs = preSpilled != i128IncomingSlots.end();
            if (abi_.abi == Abi::Win64) {
                // By-reference: the incoming GP reg (or stack word) is a pointer to
                // a caller-owned 16-byte copy; dereference it into the value slot.
                VReg ptr = fn_->newVReg();
                if (inRegs) {
                    MInst ldp{MOpcode::Load,
                              {MOperand::defVReg(ptr), MOperand::slot(preSpilled->second[0])}};
                    ldp.width = 8; ldp.isSigned = false; emit(ldp);
                    ++gpCursor;
                    if (abi_.sharedArgRegIndex) ++xmmCursor;
                } else {
                    std::int64_t inOff = 16 + static_cast<std::int64_t>(abi_.shadowSpace) +
                                         8 * static_cast<std::int64_t>(stackArgIndex);
                    std::uint32_t inSlot = fn_->addFrameSlot(8, 8, false);
                    fn_->frameSlots()[inSlot].isIncoming = true;
                    fn_->frameSlots()[inSlot].rbpOffset = inOff;
                    MInst ld{MOpcode::Load, {MOperand::defVReg(ptr), MOperand::slot(inSlot)}};
                    ld.width = 8; ld.isSigned = false; emit(ld);
                    ++stackArgIndex;
                    ++gpCursor;
                    if (abi_.sharedArgRegIndex) ++xmmCursor;
                }
                VReg lo = loadHalf(ptr, 0);
                VReg hi = loadHalf(ptr, 8);
                storeHalf(baseAddr, 0, lo);
                storeHalf(baseAddr, 8, hi);
            } else {
                // SysV: two GP registers (low, high), pre-spilled to slots above;
                // else the whole value sits on the stack as two 8-byte words.
                if (inRegs) {
                    VReg lo = fn_->newVReg();
                    MInst ldL{MOpcode::Load,
                              {MOperand::defVReg(lo), MOperand::slot(preSpilled->second[0])}};
                    ldL.width = 8; ldL.isSigned = false; emit(ldL);
                    VReg hi = fn_->newVReg();
                    MInst ldH{MOpcode::Load,
                              {MOperand::defVReg(hi), MOperand::slot(preSpilled->second[1])}};
                    ldH.width = 8; ldH.isSigned = false; emit(ldH);
                    storeHalf(baseAddr, 0, lo);
                    storeHalf(baseAddr, 8, hi);
                    gpCursor += 2;
                } else {
                    for (int half = 0; half < 2; ++half) {
                        std::int64_t inOff = 16 +
                                             static_cast<std::int64_t>(abi_.shadowSpace) +
                                             8 * static_cast<std::int64_t>(stackArgIndex);
                        std::uint32_t inSlot = fn_->addFrameSlot(8, 8, false);
                        fn_->frameSlots()[inSlot].isIncoming = true;
                        fn_->frameSlots()[inSlot].rbpOffset = inOff;
                        VReg tmp = fn_->newVReg();
                        MInst ld{MOpcode::Load,
                                 {MOperand::defVReg(tmp), MOperand::slot(inSlot)}};
                        ld.width = 8; ld.isSigned = false; emit(ld);
                        storeHalf(baseAddr, 8 * half, tmp);
                        ++stackArgIndex;
                    }
                }
            }
            continue;
        }

        // Float scalar parameter: arrives in an XMM arg register (or on the stack
        // once they are exhausted), stored to a local via movss/movsd. The local
        // slot is 8-byte-aligned regardless of precision; LocalInfo.width records
        // the precision (4=f32, 8=f64).
        if (isFloatType(pty)) {
            std::uint8_t fw = floatWidthOf(pty);
            const bool isF16 = isFloat16(pty);
            std::uint32_t slot = fn_->addFrameSlot(8, 8, /*isSpill=*/false);
            declareLocal(paramNames[i], slot, fw, false, LocalKind::Scalar,
                         /*isFloat=*/true, pty);
            unsigned xidx = abi_.sharedArgRegIndex ? (gpCursor) : xmmCursor;
            if (xidx < xmmArgRegs.size()) {
                if (isF16) {
                    // The incoming XMM holds the packed half in its low 16 bits.
                    // Spill those 16 bits raw into the (packed) local slot,
                    // consuming the argument register immediately.
                    emitFW(MOpcode::StoreXmmLo16,
                           {MOperand::slot(slot),
                            MOperand::usePhysXmm(xmmArgRegs[xidx])}, 2);
                } else {
                    emitFW(MOpcode::FStore,
                           {MOperand::slot(slot), MOperand::usePhysXmm(xmmArgRegs[xidx])}, fw);
                }
            } else {
                // Stack-passed float: copy the argument word from the caller frame
                // into the local slot.
                std::int64_t inOff = 16 + static_cast<std::int64_t>(abi_.shadowSpace) +
                                     8 * static_cast<std::int64_t>(stackArgIndex);
                std::uint32_t inSlot = fn_->addFrameSlot(8, 8, false);
                fn_->frameSlots()[inSlot].isIncoming = true;
                fn_->frameSlots()[inSlot].rbpOffset = inOff;
                if (isF16) {
                    // Packed half occupies the low 16 bits of the incoming word.
                    // Move it through XMM into the packed local slot raw.
                    VReg tmp = fn_->newVReg(RegClass::XMM);
                    emitFW(MOpcode::FLoad, {MOperand::defVReg(tmp), MOperand::slot(inSlot)}, 4);
                    emitFW(MOpcode::StoreXmmLo16,
                           {MOperand::slot(slot), MOperand::useVReg(tmp)}, 2);
                } else {
                    VReg tmp = fn_->newVReg(RegClass::XMM);
                    emitFW(MOpcode::FLoad, {MOperand::defVReg(tmp), MOperand::slot(inSlot)}, fw);
                    emitFW(MOpcode::FStore, {MOperand::slot(slot), MOperand::useVReg(tmp)}, fw);
                }
                ++stackArgIndex;
            }
            if (abi_.sharedArgRegIndex) ++gpCursor; else ++xmmCursor;
            continue;
        }

        // A struct/class parameter is passed BY HIDDEN POINTER: the incoming arg
        // register/stack word holds a pointer to caller-owned storage. We keep
        // that pointer in an 8-byte slot and address fields through it.
        const bool isAggregate = isAggregateType(pty);

        // Register-classified aggregate: the eightbytes arrive in consecutive GP
        // (RDI..) / XMM (XMM0..) argument registers. Materialize a full-size
        // inline VALUE slot and store each eightbyte into it; fields are then
        // addressed directly off the slot (LocalKind::AggregateValue).
        if (isAggregate) {
            AggregateAbi cls = classifyAggregate(pty);
            if (!cls.inMemory) {
                unsigned sz = sizeOf(pty), al = alignOf(pty);
                std::uint32_t slot = fn_->addFrameSlot(sz ? sz : 8, al ? al : 8, false);
                declareLocal(paramNames[i], slot, 8, false,
                             LocalKind::AggregateValue, false, pty);
                // Base address of the slot to store each eightbyte into.
                VReg baseAddr = fn_->newVReg();
                emit({MOpcode::LeaSlot, {MOperand::defVReg(baseAddr), MOperand::slot(slot)}});
                auto preSpAgg = i128IncomingSlots.find(i);
                std::size_t gpSpillIdx = 0;  // cursor into preSpAgg GP slots
                bool ranOutOfRegs = false;
                for (std::size_t e = 0; e < cls.eightbytes.size() && !ranOutOfRegs; ++e) {
                    const AbiEightbyte& eb = cls.eightbytes[e];
                    std::int64_t off = static_cast<std::int64_t>(e) * 8;
                    if (eb.isSSE) {
                        unsigned xidx = abi_.sharedArgRegIndex ? gpCursor : xmmCursor;
                        if (xidx >= xmmArgRegs.size()) { ranOutOfRegs = true; break; }
                        std::uint8_t fw = (eb.bytes <= 4) ? 4 : 8;
                        // Store straight out of the incoming register. Copying it
                        // into a vreg first would be wrong for a multi-eightbyte
                        // aggregate: the allocator does not know the *later* SSE
                        // argument registers are still live, so it can pick one of
                        // them as the scratch for this copy and destroy an argument
                        // that has not been read yet. (The GP path below sidesteps
                        // the same hazard by pre-spilling.)
                        emitFW(MOpcode::FStoreInd,
                               {MOperand::useVReg(baseAddr), MOperand::immediate(off),
                                MOperand::usePhysXmm(xmmArgRegs[xidx])}, fw);
                        if (abi_.sharedArgRegIndex) ++gpCursor; else ++xmmCursor;
                    } else {
                        if (gpCursor >= argRegs.size()) { ranOutOfRegs = true; break; }
                        std::uint8_t bw = static_cast<std::uint8_t>(eb.bytes);
                        if (bw > 4) bw = 8; else if (bw > 2) bw = 4;
                        else if (bw > 1) bw = 2; else bw = 1;
                        if (preSpAgg != i128IncomingSlots.end() &&
                            gpSpillIdx < preSpAgg->second.size()) {
                            // GP eightbyte was pre-spilled; reload then store.
                            VReg tmp = fn_->newVReg();
                            MInst ld{MOpcode::Load,
                                     {MOperand::defVReg(tmp),
                                      MOperand::slot(preSpAgg->second[gpSpillIdx++])}};
                            ld.width = 8; ld.isSigned = false; emit(ld);
                            MInst st{MOpcode::StoreInd,
                                     {MOperand::useVReg(baseAddr), MOperand::immediate(off),
                                      MOperand::useVReg(tmp)}};
                            st.width = bw; st.isSigned = false; emit(st);
                        } else {
                            MInst st{MOpcode::StoreInd,
                                     {MOperand::useVReg(baseAddr), MOperand::immediate(off),
                                      MOperand::usePhys(argRegs[gpCursor])}};
                            st.width = bw; st.isSigned = false;
                            emit(st);
                        }
                        ++gpCursor;
                        if (abi_.sharedArgRegIndex) ++xmmCursor;
                    }
                }
                if (ranOutOfRegs) {
                    fail("selector: aggregate parameter '" + paramNames[i] +
                         "' would split across registers and the stack (unsupported)");
                    break;
                }
                continue;
            }
            // else: fall through to the hidden-pointer (memory) handling below.
        }

        if (pty && pty->kind == Types::Kind::Any) {
            unsigned sz = sizeOf(pty), al = alignOf(pty);
            std::uint32_t slot = fn_->addFrameSlot(sz, al, false);
            declareLocal(paramNames[i], slot, 8, false,
                         LocalKind::AggregateValue, false, pty);
            VReg baseAddr = fn_->newVReg();
            emit({MOpcode::LeaSlot, {MOperand::defVReg(baseAddr), MOperand::slot(slot)}});
            VReg ptr = fn_->newVReg();
            auto preSp = i128IncomingSlots.find(i);
            if (preSp != i128IncomingSlots.end()) {
                MInst ld{MOpcode::Load, {MOperand::defVReg(ptr), MOperand::slot(preSp->second[0])}};
                ld.width = 8; ld.isSigned = false; emit(ld);
                ++gpCursor;
                if (abi_.sharedArgRegIndex) ++xmmCursor;
            } else if (gpCursor < argRegs.size()) {
                emit({MOpcode::MovRR, {MOperand::defVReg(ptr), MOperand::usePhys(argRegs[gpCursor])}});
                ++gpCursor;
                if (abi_.sharedArgRegIndex) ++xmmCursor;
            } else {
                std::int64_t inOff = 16 + static_cast<std::int64_t>(abi_.shadowSpace) +
                                     8 * static_cast<std::int64_t>(stackArgIndex);
                std::uint32_t inSlot = fn_->addFrameSlot(8, 8, false);
                fn_->frameSlots()[inSlot].isIncoming = true;
                fn_->frameSlots()[inSlot].rbpOffset = inOff;
                MInst ld{MOpcode::Load, {MOperand::defVReg(ptr), MOperand::slot(inSlot)}};
                ld.width = 8; ld.isSigned = false; emit(ld);
                ++stackArgIndex;
                ++gpCursor;
                if (abi_.sharedArgRegIndex) ++xmmCursor;
            }
            emitStructCopy(ElemAddr{baseAddr, 0}, ElemAddr{ptr, 0}, sz);
            continue;
        }

        if (!isAggregate && !isIntegerLike(pty) && pty &&
            pty->kind != Types::Kind::Pointer && pty->kind != Types::Kind::Text &&
            pty->kind != Types::Kind::Any && pty->kind != Types::Kind::Object) {
            fail("selector: unsupported parameter type for '" + paramNames[i] + "'");
            break;
        }
        std::uint8_t w = isAggregate ? 8 : static_cast<std::uint8_t>(widthOf(pty));
        bool sgn = isAggregate ? false : isSignedOf(pty);
        std::uint32_t slot = fn_->addFrameSlot(8, 8, /*isSpill=*/false);
        declareLocal(paramNames[i], slot, w, sgn,
                     isAggregate ? LocalKind::AggregatePtr : LocalKind::Scalar,
                     false, pty);

        const std::size_t regIdx = gpCursor;
        if (regIdx < argRegs.size()) {
            auto preSp = i128IncomingSlots.find(i);
            if (preSp != i128IncomingSlots.end()) {
                // The incoming arg register was pre-spilled (an i128 param elsewhere
                // forced all GP arg regs to be captured up front); reload from its
                // slot and re-store at the declared width.
                VReg tmp = fn_->newVReg();
                MInst ld{MOpcode::Load,
                         {MOperand::defVReg(tmp), MOperand::slot(preSp->second[0])}};
                ld.width = 8; ld.isSigned = false; emit(ld);
                MInst st{MOpcode::Store, {MOperand::slot(slot), MOperand::useVReg(tmp)}};
                st.width = w; st.isSigned = sgn; emit(st);
            } else {
                // store argReg -> [slot] (low `width` bytes)
                MInst st{MOpcode::Store, {MOperand::slot(slot), MOperand::usePhys(argRegs[regIdx])}};
                st.width = w; st.isSigned = sgn;
                emit(st);
            }
            ++gpCursor;
            if (abi_.sharedArgRegIndex) ++xmmCursor;
        } else {
            // Stack-passed arg. The caller places the k-th stack arg above its
            // shadow space, so at the callee (after `push rbp`) it sits at
            // [rbp + 16 + shadow + 8*k]: +8 return addr, +8 saved rbp, then the
            // Win64 shadow region (0 on System V), then the stack args.
            // Modeled as a Load from a synthetic "incoming" slot (positive off).
            std::int64_t inOff = 16 + static_cast<std::int64_t>(abi_.shadowSpace) +
                                 8 * static_cast<std::int64_t>(stackArgIndex);
            std::uint32_t inSlot = fn_->addFrameSlot(8, 8, false);
            fn_->frameSlots()[inSlot].isIncoming = true;
            fn_->frameSlots()[inSlot].rbpOffset = inOff;
            VReg tmp = fn_->newVReg();
            // Incoming args occupy a full 8-byte stack slot; reload the value at
            // its declared width (sext/zext), then store the low bytes locally.
            MInst ld{MOpcode::Load, {MOperand::defVReg(tmp), MOperand::slot(inSlot)}};
            ld.width = w; ld.isSigned = sgn;
            emit(ld);
            MInst st{MOpcode::Store, {MOperand::slot(slot), MOperand::useVReg(tmp)}};
            st.width = w; st.isSigned = sgn;
            emit(st);
            ++stackArgIndex;
            // The GP register file is exhausted; on Win64 the shared cursor still
            // advances so a later float would also land on the stack.
            ++gpCursor;
            if (abi_.sharedArgRegIndex) ++xmmCursor;
        }
    }
    }  // end !naked incoming-argument handling

    if (!failed_) selBlock(body);
    if (!failed_) emitDestructorFieldCleanups();

    // Guarantee every path is terminated: append a trailing `Ret` so a function
    // that falls off the end of its body (e.g. a void function with no explicit
    // `return`, or one ending in an if/loop) still returns instead of running into
    // the next function's code. A redundant `Ret` after an existing terminator is
    // harmless dead code (the same property the control-flow lowering relies on).
    // Naked functions are exempt: they emit their own return and we must not
    // inject any compiler-generated epilogue/return.
    if (!failed_ && !fn_->naked) emit({MOpcode::Ret, {}});

    popScope();
    fn_ = nullptr;

    if (failed_) { errorOut = error_; return nullptr; }
    return fn;
}

void InstructionSelector::selBlock(const AST::NodeList& body) {
    pushScope();
    std::size_t cleanupMark = cleanupScopeMarks_.empty() ? cleanups_.size()
                                                         : cleanupScopeMarks_.back();
    for (const auto& stmt : body) {
        if (failed_) break;
        selStatement(stmt);
    }
    if (!failed_) emitCleanupsTo(cleanupMark, /*popEntries=*/true);
    popScope();
}

void InstructionSelector::selStatement(const AST::NodePtr& stmt) {
    if (!stmt) return;
    auto finishStatement = [&]() {
        if (!failed_) emitTemporaryCleanups();
    };
    switch (stmt->nodeType()) {
        case AST::NodeType::ReturnStatement:
            selReturn(static_cast<const AST::ReturnStatement&>(*stmt));
            return;
        case AST::NodeType::VariableDeclaration:
            selVarDecl(static_cast<const AST::VariableDeclarationExpr&>(*stmt));
            finishStatement();
            return;
        case AST::NodeType::AssignmentExpr:
            selAssign(static_cast<const AST::AssignmentExpr&>(*stmt));
            finishStatement();
            return;
        case AST::NodeType::IfStatement:
            selIf(static_cast<const AST::IfStatement&>(*stmt));
            finishStatement();
            return;
        case AST::NodeType::WhileLoop:
            selWhile(static_cast<const AST::WhileLoop&>(*stmt));
            finishStatement();
            return;
        case AST::NodeType::InfiniteLoop:
            selInfiniteLoop(static_cast<const AST::InfiniteLoop&>(*stmt));
            finishStatement();
            return;
        case AST::NodeType::ForLoop:
            selForLoop(static_cast<const AST::ForLoop&>(*stmt));
            finishStatement();
            return;
        case AST::NodeType::WhenStatement:
            selWhen(static_cast<const AST::WhenStatement&>(*stmt));
            finishStatement();
            return;
        case AST::NodeType::SwitchStatement:
            selSwitch(static_cast<const AST::SwitchStatement&>(*stmt));
            finishStatement();
            return;
        case AST::NodeType::BreakStatement:
            selBreak();
            return;
        case AST::NodeType::SkipStatement:
            selSkip();
            return;
        case AST::NodeType::UnsafeBlock:
            // `unsafe { ... }` is a transparent statement block; lower its body in
            // a nested scope (the unsafety is a sema-level concept, not codegen).
            selBlock(static_cast<const AST::UnsafeBlock&>(*stmt).body);
            finishStatement();
            return;
        default:
            // expression-as-statement (e.g. a bare call)
            selExpr(stmt);
            finishStatement();
            return;
    }
}

void InstructionSelector::selReturn(const AST::ReturnStatement& ret) {
    // Slice return: resolve the return value to a 16-byte { ptr, len } header,
    // then hand it back per the aggregate return ABI (SysV: RAX:RDX; Win64: via
    // the hidden sret pointer). A slice lvalue is addressed in place; a contextual
    // array / `new T[n]` return is built into a fresh header first.
    if (ret.returnValue && isSliceType(returnType_)) {
        Types::TypeRef vty = concreteTypeOf(ret.returnValue.get());
        ElemAddr src = isSliceType(vty)
                           ? computeLValueAddr(ret.returnValue.get())
                           : materializeSliceValue(returnType_, ret.returnValue);
        if (failed_) return;
        VReg base = materializeAddr(src);
        if (failed_) return;
        if (sretActive_) {
            VReg dstPtr = fn_->newVReg();
            MInst ld{MOpcode::Load, {MOperand::defVReg(dstPtr), MOperand::slot(sretSlot_)}};
            ld.width = 8; ld.isSigned = false;
            emit(ld);
            emitStructCopy(ElemAddr{dstPtr, 0}, ElemAddr{base, 0}, 16);
            VReg ret2 = fn_->newVReg();
            MInst ld2{MOpcode::Load, {MOperand::defVReg(ret2), MOperand::slot(sretSlot_)}};
            ld2.width = 8; ld2.isSigned = false;
            emit(ld2);
            emitFunctionExitCleanups();
            if (failed_) return;
            emit({MOpcode::MovRR,
                  {MOperand::defPhys(abi_.intReturnReg), MOperand::useVReg(ret2)}});
            emit({MOpcode::Ret, {}});
            return;
        }
        VReg lo = loadHalf(base, 0);
        VReg hi = loadHalf(base, 8);
        emitFunctionExitCleanups();
        if (failed_) return;
        emit({MOpcode::MovRR, {MOperand::defPhys(PhysReg::RAX), MOperand::useVReg(lo)}});
        emit({MOpcode::MovRR, {MOperand::defPhys(PhysReg::RDX), MOperand::useVReg(hi)}});
        emit({MOpcode::Ret, {}});
        return;
    }
    if (ret.returnValue) {
        // Struct return-by-value in registers: load each eightbyte of the
        // returned aggregate from its storage into the ABI return registers
        // (INTEGER -> RAX/RDX, SSE -> XMM0/XMM1).
        if (sretRegReturn_) {
            ElemAddr src = computeLValueAddr(ret.returnValue.get());
            if (failed_) return;
            VReg base = materializeAddr(src);
            struct RetPiece {
                bool isSSE = false;
                std::uint8_t width = 8;
                VReg value = kInvalidVReg;
            };
            std::vector<RetPiece> retPieces;
            for (std::size_t e = 0; e < sretRetCls_.eightbytes.size(); ++e) {
                const AbiEightbyte& eb = sretRetCls_.eightbytes[e];
                std::int64_t off = static_cast<std::int64_t>(e) * 8;
                if (eb.isSSE) {
                    std::uint8_t fw = (eb.bytes <= 4) ? 4 : 8;
                    VReg v = fn_->newVReg(RegClass::XMM);
                    emitFW(MOpcode::FLoadInd,
                           {MOperand::defVReg(v), MOperand::useVReg(base),
                            MOperand::immediate(off)}, fw);
                    retPieces.push_back(RetPiece{true, fw, v});
                } else {
                    std::uint8_t w = static_cast<std::uint8_t>(eb.bytes);
                    if (w > 4) w = 8; else if (w > 2) w = 4; else if (w > 1) w = 2; else w = 1;
                    VReg v = fn_->newVReg();
                    MInst ld{MOpcode::LoadInd,
                             {MOperand::defVReg(v), MOperand::useVReg(base),
                              MOperand::immediate(off)}};
                    ld.width = w; ld.isSigned = false;
                    emit(ld);
                    retPieces.push_back(RetPiece{false, w, v});
                }
            }
            if (failed_) return;
            emitFunctionExitCleanups();
            if (failed_) return;
            static const PhysReg kIntRet[2] = {PhysReg::RAX, PhysReg::RDX};
            static const XmmReg kSseRet[2] = {XmmReg::XMM0, XmmReg::XMM1};
            unsigned intIdx = 0, sseIdx = 0;
            for (const RetPiece& piece : retPieces) {
                if (piece.isSSE) {
                    emitFW(MOpcode::FMovRR,
                           {MOperand::defPhysXmm(kSseRet[sseIdx++]),
                            MOperand::useVReg(piece.value)}, piece.width);
                } else {
                    emit({MOpcode::MovRR,
                          {MOperand::defPhys(kIntRet[intIdx++]),
                           MOperand::useVReg(piece.value)}});
                }
            }
            emit({MOpcode::Ret, {}});
            return;
        }
        // Struct return-by-value: copy the returned aggregate into the caller's
        // result storage (via the hidden sret pointer) and return that pointer.
        if (sretActive_) {
            VReg dstPtr = fn_->newVReg();
            MInst ld{MOpcode::Load, {MOperand::defVReg(dstPtr), MOperand::slot(sretSlot_)}};
            ld.width = 8; ld.isSigned = false;
            emit(ld);
            // `return T(...)` / `return f()`: construct/return the result directly
            // into the caller's storage (return-value optimization) so no temporary
            // is created and a class with a destructor is destroyed exactly once.
            // Otherwise copy the returned aggregate from its source lvalue.
            std::uint32_t movedSlot = kNoCleanupSlot;
            if (!emitAggregatePrvalueInPlace(ret.returnValue, dstPtr)) {
                ElemAddr src = computeLValueAddr(ret.returnValue.get());
                if (failed_) return;
                emitStructCopy(ElemAddr{dstPtr, 0}, src, sizeOf(returnType_));
                // NRVO: returning a named local moves it into the caller's
                // storage, so suppress its destructor on this path.
                movedSlot = movableReturnLocalSlot(ret.returnValue.get());
            }
            if (failed_) return;
            // Per the ABI, return the result pointer in RAX.
            VReg ret2 = fn_->newVReg();
            MInst ld2{MOpcode::Load, {MOperand::defVReg(ret2), MOperand::slot(sretSlot_)}};
            ld2.width = 8; ld2.isSigned = false;
            emit(ld2);
            emitFunctionExitCleanups(movedSlot);
            if (failed_) return;
            emit({MOpcode::MovRR,
                  {MOperand::defPhys(abi_.intReturnReg), MOperand::useVReg(ret2)}});
            emit({MOpcode::Ret, {}});
            return;
        }
        VReg v = selExpr(ret.returnValue);
        if (failed_) return;
        // Float results return in the ABI XMM return register; integers/pointers
        // in the GP return register.
        if (isFloat128(returnType_)) {
            fail("selector: f128 return ABI is not supported yet");
            return;
        }
        if (isFloatType(returnType_)) {
            if (isFloat16(returnType_)) {
                // Pack the f32 compute value to a half in the XMM return reg's
                // low 16 bits; the caller expands it.
                VReg h = fn_->newVReg(RegClass::XMM);
                emitFW(MOpcode::CvtF32ToF16,
                       {MOperand::defVReg(h), MOperand::useVReg(v)}, 4);
                emitFunctionExitCleanups();
                if (failed_) return;
                emitFW(MOpcode::FMovRR,
                       {MOperand::defPhysXmm(abi_.xmmReturnReg), MOperand::useVReg(h)}, 4);
            } else {
                emitFunctionExitCleanups();
                if (failed_) return;
                emitFW(MOpcode::FMovRR,
                       {MOperand::defPhysXmm(abi_.xmmReturnReg), MOperand::useVReg(v)},
                       floatWidthOf(returnType_));
            }
        } else if (isInt128(returnType_)) {
            // A 16-byte integer returns in RAX:RDX. `v` is the address of the
            // value's 16-byte slot; load its low/high words into the pair.
            VReg lo = loadHalf(v, 0);
            VReg hi = loadHalf(v, 8);
            emitFunctionExitCleanups();
            if (failed_) return;
            emit({MOpcode::MovRR, {MOperand::defPhys(PhysReg::RAX), MOperand::useVReg(lo)}});
            emit({MOpcode::MovRR, {MOperand::defPhys(PhysReg::RDX), MOperand::useVReg(hi)}});
        } else {
            emitFunctionExitCleanups();
            if (failed_) return;
            emit({MOpcode::MovRR,
                  {MOperand::defPhys(abi_.intReturnReg), MOperand::useVReg(v)}});
        }
    } else {
        emitFunctionExitCleanups();
        if (failed_) return;
    }
    emit({MOpcode::Ret, {}});
}

void InstructionSelector::selVarDecl(const AST::VariableDeclarationExpr& decl) {
    Types::TypeRef ty = concreteTypeOf(&decl);
    // f128 local: modeled as a 16-byte memory value, mirroring i128. Arithmetic
    // writes directly into the destination slot through soft-float helpers.
    if (isFloat128(ty)) {
        std::uint32_t slot = fn_->addFrameSlot(16, 16, /*isSpill=*/false);
        declareLocal(decl.identifier, slot, 16, false, LocalKind::AggregateValue,
                     /*isFloat=*/true, ty);
        if (decl.initialValue) {
            VReg dstAddr = fn_->newVReg();
            emit({MOpcode::LeaSlot, {MOperand::defVReg(dstAddr), MOperand::slot(slot)}});
            selF128(decl.initialValue, dstAddr);
        }
        return;
    }
    // 128-bit integer local: a 16-byte (16-aligned) slot addressed like an
    // aggregate. Its lvalue is the slot address; arithmetic operates on the
    // lo[+0]/hi[+8] halves. Initialize by evaluating the rhs straight into it.
    if (isInt128(ty)) {
        std::uint32_t slot = fn_->addFrameSlot(16, 16, /*isSpill=*/false);
        declareLocal(decl.identifier, slot, 16, ty->isSigned, LocalKind::AggregateValue,
                     false, ty);
        if (decl.initialValue) {
            VReg dstAddr = fn_->newVReg();
            emit({MOpcode::LeaSlot, {MOperand::defVReg(dstAddr), MOperand::slot(slot)}});
            selI128(decl.initialValue, dstAddr);
        }
        return;
    }
    // Slice value local: a 16-byte `{ T* ptr, i64 len }` header inline on the
    // stack (addressed like an aggregate). An initializer builds the header from
    // a fixed array, another slice, or `new T[n]`.
    if (isSliceType(ty)) {
        std::uint32_t slot = fn_->addFrameSlot(16, 8, /*isSpill=*/false);
        declareLocal(decl.identifier, slot, 8, false, LocalKind::AggregateValue,
                     false, ty);
        if (decl.initialValue) {
            VReg dstAddr = fn_->newVReg();
            emit({MOpcode::LeaSlot, {MOperand::defVReg(dstAddr), MOperand::slot(slot)}});
            if (!emitSliceInitInto(dstAddr, ty, decl.initialValue)) return;
        }
        return;
    }
    // Fixed-array value local: occupies sizeOf(ty) = stride*count bytes inline on
    // the stack, addressed like an aggregate (its lvalue is the slot address).
    // Element access (a[i]) folds through computeElementAddr. An optional
    // initializer copy-initializes from another array lvalue.
    if (ty && ty->kind == Types::Kind::Array) {
        unsigned sz = sizeOf(ty);
        unsigned al = alignOf(ty);
        std::uint32_t slot = fn_->addFrameSlot(sz ? sz : 8, al ? al : 8, /*isSpill=*/false);
        declareLocal(decl.identifier, slot, 8, false, LocalKind::AggregateValue,
                     false, ty);
        if (decl.initialValue) {
            ElemAddr src = computeLValueAddr(decl.initialValue.get());
            if (failed_) return;
            VReg dstAddr = fn_->newVReg();
            emit({MOpcode::LeaSlot, {MOperand::defVReg(dstAddr), MOperand::slot(slot)}});
            emitStructCopy(ElemAddr{dstAddr, 0}, src, sz);
        }
        return;
    }
    // Struct/class value local: occupies sizeOf(ty) bytes inline on the stack and
    // is addressed (never value-loaded). Field assignments write through its slot
    // address; an initializer copy-initializes the slot (construct-in-place for a
    // constructor call, otherwise a byte copy from the source aggregate lvalue).
    if (ty && (ty->kind == Types::Kind::Struct || ty->kind == Types::Kind::Class)) {
        const Sema::StructInfo* si = structInfoFor(ty);
        if (!si) {
            fail("selector: unknown struct type for '" + decl.identifier + "'");
            return;
        }
        unsigned sz = sizeOf(ty);
        unsigned al = alignOf(ty);
        std::uint32_t slot = fn_->addFrameSlot(sz ? sz : 8, al ? al : 8, /*isSpill=*/false);
        declareLocal(decl.identifier, slot, 8, false, LocalKind::AggregateValue,
                     false, ty);
        if (decl.initialValue) {
            VReg dstAddr = fn_->newVReg();
            emit({MOpcode::LeaSlot, {MOperand::defVReg(dstAddr), MOperand::slot(slot)}});
            // `var x = T(...)` / `var x = f()`: construct/return-into-place
            // (copy-elision) so a class with a destructor is built directly in x
            // and destroyed exactly once. Otherwise copy-init from an aggregate
            // lvalue (var s = other).
            if (!emitAggregatePrvalueInPlace(decl.initialValue, dstAddr)) {
                ElemAddr src = computeLValueAddr(decl.initialValue.get());
                if (failed_) return;
                emitStructCopy(ElemAddr{dstAddr, 0}, src, sz);
            }
            if (failed_) return;
        }
        maybeRegisterCleanup(ty, slot);
        return;
    }
    if (ty && ty->kind == Types::Kind::Any) {
        unsigned sz = sizeOf(ty), al = alignOf(ty);
        std::uint32_t slot = fn_->addFrameSlot(sz, al, false);
        declareLocal(decl.identifier, slot, 8, false, LocalKind::AggregateValue,
                     false, ty);
        if (decl.initialValue) {
            VReg dstAddr = fn_->newVReg();
            emit({MOpcode::LeaSlot, {MOperand::defVReg(dstAddr), MOperand::slot(slot)}});
            ElemAddr src = computeLValueAddr(decl.initialValue.get());
            if (failed_) return;
            emitStructCopy(ElemAddr{dstAddr, 0}, src, sz);
        }
        return;
    }
    if (ty && !isFloatType(ty) && !isIntegerLike(ty) && ty->kind != Types::Kind::Pointer &&
        ty->kind != Types::Kind::Text && ty->kind != Types::Kind::Function &&
        ty->kind != Types::Kind::Any && ty->kind != Types::Kind::Object &&
        ty->kind != Types::Kind::Closure) {
        fail("selector: unsupported variable type for '" + decl.identifier + "'");
        return;
    }
    // Float scalar local: 8-byte-aligned slot; precision (4/8) stored in
    // LocalInfo.width, stored/loaded via movss/movsd in XMM.
    if (isFloatType(ty)) {
        std::uint8_t fw = floatWidthOf(ty);
        std::uint32_t slot = fn_->addFrameSlot(8, 8, /*isSpill=*/false);
        declareLocal(decl.identifier, slot, fw, false, LocalKind::Scalar,
                     /*isFloat=*/true, ty);
        if (decl.initialValue) {
            VReg v = selExpr(decl.initialValue);
            if (failed_) return;
            emitFW(MOpcode::FStore, {MOperand::slot(slot), MOperand::useVReg(v)}, fw);
        }
        return;
    }
    std::uint8_t w = static_cast<std::uint8_t>(widthOf(ty));
    bool sgn = isSignedOf(ty);
    std::uint32_t slot = fn_->addFrameSlot(8, 8, /*isSpill=*/false);
    declareLocal(decl.identifier, slot, w, sgn, LocalKind::Scalar, false, ty);
    if (decl.initialValue) {
        VReg v = selExpr(decl.initialValue);
        if (failed_) return;
        MInst st{MOpcode::Store, {MOperand::slot(slot), MOperand::useVReg(v)}};
        st.width = w; st.isSigned = sgn;
        emit(st);
    }
}

void InstructionSelector::selAssign(const AST::AssignmentExpr& a) {
    if (!a.target) {
        fail("selector: assignment without a target");
        return;
    }
    // Whole-struct copy assignment: `dst = src` where dst is a struct/class
    // lvalue. Both sides resolve to addresses; copy sizeOf(struct) bytes. Handles
    // identifiers, fields (s.inner = other), and elements (a[i] = other).
    Types::TypeRef tgtTy = concreteTypeOf(a.target.get());
    // Whole-slice assignment: rebuild the destination header from the rhs (another
    // slice copies its {ptr,len}; a fixed array / `new T[n]` forms a fresh header).
    if (isSliceType(tgtTy)) {
        ElemAddr dst = computeLValueAddr(a.target.get());
        if (failed_) return;
        VReg dstAddr = materializeAddr(dst);
        if (failed_) return;
        emitSliceInitInto(dstAddr, tgtTy, a.value);
        return;
    }
    if (tgtTy && (tgtTy->kind == Types::Kind::Struct || tgtTy->kind == Types::Kind::Class)) {
        ElemAddr dst = computeLValueAddr(a.target.get());
        if (failed_) return;
        VReg dstAddr = materializeAddr(dst);
        if (failed_) return;
        // `dst = T(...)` / `dst = f()`: construct/return the rhs directly into dst
        // (elision) so a class result is not created as a separate temporary that
        // would be destroyed in addition to dst. Otherwise byte-copy from the rhs
        // lvalue. (Assignment does not run dst's prior destructor -- a separate,
        // pre-existing gap -- but this never double-destroys the new value.)
        if (!emitAggregatePrvalueInPlace(a.value, dstAddr)) {
            ElemAddr src = computeLValueAddr(a.value.get());
            if (failed_) return;
            emitStructCopy(ElemAddr{dstAddr, 0}, src, sizeOf(tgtTy));
        }
        return;
    }
    // 128-bit integer assignment: resolve the destination to an address and
    // evaluate the rhs straight into it (lo[+0]/hi[+8]). Covers x = ..., a[i] =,
    // s.f =, *p = for i128 lvalues. The target's sema type may be unannotated for
    // a bare identifier, so also consult the declared local width (16 bytes).
    bool f128Target = isFloat128(tgtTy);
    if (!f128Target && a.target->nodeType() == AST::NodeType::IdentifierExpr) {
        LocalInfo li;
        if (lookupLocal(static_cast<const AST::IdentifierExpr&>(*a.target).name, li) &&
            li.kind == LocalKind::AggregateValue && li.width == 16 &&
            li.isFloat) {
            f128Target = true;
        }
    }
    if (f128Target) {
        ElemAddr dst = computeLValueAddr(a.target.get());
        if (failed_) return;
        VReg dstAddr = materializeAddr(dst);
        selF128(a.value, dstAddr);
        return;
    }

    bool i128Target = isInt128(tgtTy);
    if (!i128Target && a.target->nodeType() == AST::NodeType::IdentifierExpr) {
        LocalInfo li;
        if (lookupLocal(static_cast<const AST::IdentifierExpr&>(*a.target).name, li) &&
            li.kind == LocalKind::AggregateValue && li.width == 16) {
            i128Target = true;
        }
    }
    if (i128Target) {
        ElemAddr dst = computeLValueAddr(a.target.get());
        if (failed_) return;
        VReg dstAddr = dst.base;
        if (dst.disp != 0) {
            dstAddr = fn_->newVReg();
            emit({MOpcode::LeaDisp,
                  {MOperand::defVReg(dstAddr), MOperand::useVReg(dst.base),
                   MOperand::immediate(dst.disp)}});
        }
        selI128(a.value, dstAddr);
        return;
    }
    // `*p = v` : store through the pointer in a register.
    if (a.target->nodeType() == AST::NodeType::DereferenceExpr) {
        const auto& deref = static_cast<const AST::DereferenceExpr&>(*a.target);
        VReg base = selExpr(deref.operand);
        if (failed_) return;
        VReg v = selExpr(a.value);
        if (failed_) return;
        Types::TypeRef pointee = concreteTypeOf(&deref);
        if (isFloatType(pointee)) {
            emitFW(MOpcode::FStoreInd,
                   {MOperand::useVReg(base), MOperand::immediate(0), MOperand::useVReg(v)},
                   floatWidthOf(pointee));
            return;
        }
        MInst st{MOpcode::StoreInd,
                 {MOperand::useVReg(base), MOperand::immediate(0), MOperand::useVReg(v)}};
        st.width = static_cast<std::uint8_t>(widthOf(pointee));
        st.isSigned = isSignedOf(pointee);
        emit(st);
        return;
    }
    // `a[i] = v` / `a.field = v` / `a[i].field = v` : store through the computed
    // element/field address (constant index scaling + field offset fold into the
    // store's displacement).
    if (a.target->nodeType() == AST::NodeType::MemberAccess) {
        const auto& m = static_cast<const AST::MemberAccessExpr&>(*a.target);
        Types::TypeRef elemTy = concreteTypeOf(&m);
        ElemAddr ea = computeLValueAddr(&m);
        if (failed_) return;
        VReg v = selExpr(a.value);
        if (failed_) return;
        if (isFloatType(elemTy)) {
            emitFW(MOpcode::FStoreInd,
                   {MOperand::useVReg(ea.base), MOperand::immediate(ea.disp),
                    MOperand::useVReg(v)},
                   floatWidthOf(elemTy));
            return;
        }
        MInst st{MOpcode::StoreInd,
                 {MOperand::useVReg(ea.base), MOperand::immediate(ea.disp), MOperand::useVReg(v)}};
        st.width = static_cast<std::uint8_t>(widthOf(elemTy));
        st.isSigned = isSignedOf(elemTy);
        emit(st);
        return;
    }
    // Only simple identifier targets are otherwise supported in this pass.
    if (a.target->nodeType() != AST::NodeType::IdentifierExpr) {
        fail("selector: only simple variable or *pointer assignment is supported");
        return;
    }
    const auto& id = static_cast<const AST::IdentifierExpr&>(*a.target);
    LocalInfo li;
    if (!lookupLocal(id.name, li)) {
        // Assignment to a module-level global scalar: store through its
        // RIP-relative address.
        if (const Sema::GlobalInfo* g = lookupGlobal(id.name)) {
            Types::TypeRef gt = g->type;
            if (gt && (gt->kind == Types::Kind::Struct ||
                       gt->kind == Types::Kind::Class ||
                       gt->kind == Types::Kind::Array)) {
                fail("selector: whole-aggregate assignment to global '" + id.name +
                     "' is not supported");
                return;
            }
            VReg addr = globalAddr(*g);
            VReg v = selExpr(a.value);
            if (failed_) return;
            if (isFloatType(gt)) {
                emitFW(MOpcode::FStoreInd,
                       {MOperand::useVReg(addr), MOperand::immediate(0),
                        MOperand::useVReg(v)},
                       floatWidthOf(gt));
                return;
            }
            MInst st{MOpcode::StoreInd,
                     {MOperand::useVReg(addr), MOperand::immediate(0),
                      MOperand::useVReg(v)}};
            st.width = static_cast<std::uint8_t>(widthOf(gt));
            st.isSigned = isSignedOf(gt);
            emit(st);
            return;
        }
        fail("selector: assignment to unknown local '" + id.name + "'");
        return;
    }
    if (li.kind != LocalKind::Scalar) {
        fail("selector: whole-aggregate assignment to '" + id.name +
             "' is not supported");
        return;
    }
    VReg v = selExpr(a.value);
    if (failed_) return;
    if (li.isFloat) {
        emitFW(MOpcode::FStore, {MOperand::slot(li.slot), MOperand::useVReg(v)}, li.width);
        return;
    }
    MInst st{MOpcode::Store, {MOperand::slot(li.slot), MOperand::useVReg(v)}};
    st.width = li.width; st.isSigned = li.isSigned;
    emit(st);
}

void InstructionSelector::selIf(const AST::IfStatement& s) {
    // entry: cond ; branch-if-FALSE -> elseBlock ; then... ; jmp join
    Cond trueCond;
    if (!selCondition(s.condition, trueCond)) return;
    // We branch to the else/join block when the condition is FALSE, so invert.
    Cond falseCond = invertCond(trueCond);

    const bool hasElse = !s.alternate.empty();
    std::uint32_t thenB = fn_->addBlock();
    std::uint32_t elseB = hasElse ? fn_->addBlock() : 0;
    std::uint32_t joinB = fn_->addBlock();

    // conditional branch to else (or join) when condition is false. We then
    // explicitly jump to the then-block rather than relying on physical
    // fall-through: nested ifs/loops append their own blocks to the function in
    // creation order, so `thenB` is frequently NOT the next block in layout.
    MInst jcc{MOpcode::Jcc, {MOperand::lbl(hasElse ? elseB : joinB)}};
    jcc.cond = falseCond;
    emit(jcc);
    emit({MOpcode::Jmp, {MOperand::lbl(thenB)}});

    // then block
    curBlock_ = thenB;
    selBlock(s.consequent);
    if (!failed_) emit({MOpcode::Jmp, {MOperand::lbl(joinB)}});

    if (hasElse) {
        curBlock_ = elseB;
        selBlock(s.alternate);
        if (!failed_) emit({MOpcode::Jmp, {MOperand::lbl(joinB)}});
    }

    curBlock_ = joinB;
}

void InstructionSelector::selWhile(const AST::WhileLoop& s) {
    // header: cond ; branch-if-FALSE -> exit ; body... ; jmp header ; exit:
    //
    // The body/exit blocks are created AFTER the condition is selected, because a
    // short-circuit condition (`a && b`, `a || b`) appends its own basic blocks
    // during evaluation. Creating body/exit first would interleave them between
    // the condition's sub-blocks in layout order, so post-loop code could fall
    // through into a loop-internal block. (selIf orders things the same way.)
    std::uint32_t headerB = fn_->addBlock();
    emit({MOpcode::Jmp, {MOperand::lbl(headerB)}});

    curBlock_ = headerB;
    Cond trueCond;
    if (!selCondition(s.condition, trueCond)) return;
    Cond falseCond = invertCond(trueCond);

    std::uint32_t bodyB = fn_->addBlock();
    std::uint32_t exitB = fn_->addBlock();

    MInst jcc{MOpcode::Jcc, {MOperand::lbl(exitB)}};
    jcc.cond = falseCond;
    emit(jcc);
    // Explicit jump into the body (do not rely on physical fall-through: a
    // short-circuit condition or nested construct may have appended blocks).
    emit({MOpcode::Jmp, {MOperand::lbl(bodyB)}});

    curBlock_ = bodyB;
    // `break` exits to exitB; `skip` (continue) re-tests the condition at headerB.
    loopStack_.push_back({exitB, headerB, cleanups_.size()});
    selBlock(s.body);
    loopStack_.pop_back();
    if (!failed_) emit({MOpcode::Jmp, {MOperand::lbl(headerB)}});

    curBlock_ = exitB;
}

void InstructionSelector::selForLoop(const AST::ForLoop& s) {
    // Loop-variable type as computed by sema (recorded on the node).
    Types::TypeRef vt = sema_.typeOf(&s);

    pushScope();
    std::uint32_t headerB = fn_->addBlock();
    std::uint32_t bodyB = fn_->addBlock();
    std::uint32_t contB = fn_->addBlock();  // `skip` target: advance, then re-test
    std::uint32_t exitB = fn_->addBlock();

    auto store8 = [&](std::uint32_t slot, VReg v) {
        MInst st{MOpcode::Store, {MOperand::slot(slot), MOperand::useVReg(v)}};
        st.width = 8; st.isSigned = false;
        emit(st);
    };
    auto load8 = [&](std::uint32_t slot) {
        VReg r = fn_->newVReg();
        MInst ld{MOpcode::Load, {MOperand::defVReg(r), MOperand::slot(slot)}};
        ld.width = 8; ld.isSigned = false;
        emit(ld);
        return r;
    };

    if (s.isRange) {
        std::uint8_t w = static_cast<std::uint8_t>(widthOf(vt));
        bool sgn = isSignedOf(vt);
        std::uint32_t iSlot = fn_->addFrameSlot(8, 8, /*isSpill=*/false);
        std::uint32_t endSlot = fn_->addFrameSlot(8, 8, /*isSpill=*/false);
        declareLocal(s.varName, iSlot, w, sgn, LocalKind::Scalar, false, vt);

        VReg sv = selExpr(s.rangeStart);
        if (failed_) { popScope(); return; }
        { MInst st{MOpcode::Store, {MOperand::slot(iSlot), MOperand::useVReg(sv)}};
          st.width = w; st.isSigned = sgn; emit(st); }
        VReg ev = selExpr(s.rangeEnd);
        if (failed_) { popScope(); return; }
        store8(endSlot, ev);

        emit({MOpcode::Jmp, {MOperand::lbl(headerB)}});

        // header: exit when i >= end (signed/unsigned per the counter type).
        curBlock_ = headerB;
        VReg iCur = fn_->newVReg();
        { MInst ld{MOpcode::Load, {MOperand::defVReg(iCur), MOperand::slot(iSlot)}};
          ld.width = w; ld.isSigned = sgn; emit(ld); }
        VReg eCur = load8(endSlot);
        emit({MOpcode::Cmp, {MOperand::useVReg(iCur), MOperand::useVReg(eCur)}});
        { MInst jcc{MOpcode::Jcc, {MOperand::lbl(exitB)}};
          jcc.cond = sgn ? Cond::GE : Cond::UGE; emit(jcc); }
        emit({MOpcode::Jmp, {MOperand::lbl(bodyB)}});

        curBlock_ = bodyB;
        loopStack_.push_back({exitB, contB, cleanups_.size()});
        selBlock(s.body);
        loopStack_.pop_back();
        if (!failed_) emit({MOpcode::Jmp, {MOperand::lbl(contB)}});

        // cont: ++i, then re-test.
        curBlock_ = contB;
        VReg iN = fn_->newVReg();
        { MInst ld{MOpcode::Load, {MOperand::defVReg(iN), MOperand::slot(iSlot)}};
          ld.width = w; ld.isSigned = sgn; emit(ld); }
        VReg one = fn_->newVReg();
        emit({MOpcode::MovRI, {MOperand::defVReg(one), MOperand::immediate(1)}});
        emit({MOpcode::Add, {MOperand::useDefVReg(iN), MOperand::useVReg(one)}});
        { MInst st{MOpcode::Store, {MOperand::slot(iSlot), MOperand::useVReg(iN)}};
          st.width = w; st.isSigned = sgn; emit(st); }
        emit({MOpcode::Jmp, {MOperand::lbl(headerB)}});

        curBlock_ = exitB;
        popScope();
        return;
    }

    // Iterable form: resolve the source's base pointer + element count, then walk
    // indices 0..len, binding the loop variable to a copy of each element.
    Types::TypeRef srcTy = concreteTypeOf(s.iterable.get());
    Types::TypeRef elemTy = vt;
    unsigned elemSize = elemTy ? sizeOf(elemTy) : 1;
    if (elemSize == 0) elemSize = 1;

    VReg basePtr = kInvalidVReg;
    VReg lenV = kInvalidVReg;
    if (isSliceType(srcTy)) {
        ElemAddr hdr = computeLValueAddr(s.iterable.get());
        if (failed_) { popScope(); return; }
        VReg ha = materializeAddr(hdr);
        basePtr = fn_->newVReg();
        { MInst ld{MOpcode::LoadInd,
                   {MOperand::defVReg(basePtr), MOperand::useVReg(ha),
                    MOperand::immediate(0)}};
          ld.width = 8; ld.isSigned = false; emit(ld); }
        lenV = loadHalf(ha, 8);
    } else if (srcTy && srcTy->kind == Types::Kind::Array) {
        ElemAddr arr = computeLValueAddr(s.iterable.get());
        if (failed_) { popScope(); return; }
        basePtr = materializeAddr(arr);
        lenV = fn_->newVReg();
        emit({MOpcode::MovRI,
              {MOperand::defVReg(lenV),
               MOperand::immediate(srcTy->arrayLength > 0 ? srcTy->arrayLength : 0)}});
    } else if (srcTy && srcTy->kind == Types::Kind::Text) {
        basePtr = selExpr(s.iterable);
        if (failed_) { popScope(); return; }
        lenV = emitStrlenOf(basePtr);
    } else {
        fail("selector: cannot iterate this value in a for-in loop");
        popScope();
        return;
    }

    // Spill loop-invariant base/len and the index across the body (calls in the
    // body clobber caller-saved registers).
    std::uint32_t baseSlot = fn_->addFrameSlot(8, 8, /*isSpill=*/false);
    std::uint32_t lenSlot = fn_->addFrameSlot(8, 8, /*isSpill=*/false);
    std::uint32_t idxSlot = fn_->addFrameSlot(8, 8, /*isSpill=*/false);
    store8(baseSlot, basePtr);
    store8(lenSlot, lenV);
    { VReg z = fn_->newVReg();
      emit({MOpcode::MovRI, {MOperand::defVReg(z), MOperand::immediate(0)}});
      store8(idxSlot, z); }

    // The loop variable holds a copy of the current element.
    const bool elemAgg = isAggregateType(elemTy);
    const bool elemFloat = isFloatType(elemTy);
    unsigned vslotSize = elemAgg ? (elemSize ? elemSize : 8) : 8;
    unsigned vslotAlign = elemAgg ? (alignOf(elemTy) ? alignOf(elemTy) : 8) : 8;
    std::uint32_t vslot = fn_->addFrameSlot(vslotSize, vslotAlign, /*isSpill=*/false);
    if (elemAgg) {
        declareLocal(s.varName, vslot, 8, false, LocalKind::AggregateValue, false, elemTy);
    } else if (elemFloat) {
        declareLocal(s.varName, vslot, floatWidthOf(elemTy), false, LocalKind::Scalar,
                     true, elemTy);
    } else {
        declareLocal(s.varName, vslot, static_cast<std::uint8_t>(widthOf(elemTy)),
                     isSignedOf(elemTy), LocalKind::Scalar, false, elemTy);
    }

    emit({MOpcode::Jmp, {MOperand::lbl(headerB)}});

    // header: exit when idx >= len (unsigned; both are counts).
    curBlock_ = headerB;
    VReg idxCur = load8(idxSlot);
    VReg lenCur = load8(lenSlot);
    emit({MOpcode::Cmp, {MOperand::useVReg(idxCur), MOperand::useVReg(lenCur)}});
    { MInst jcc{MOpcode::Jcc, {MOperand::lbl(exitB)}}; jcc.cond = Cond::UGE; emit(jcc); }
    emit({MOpcode::Jmp, {MOperand::lbl(bodyB)}});

    // body: elem = base[idx]; copy into the loop variable; run the body.
    curBlock_ = bodyB;
    VReg baseCur = load8(baseSlot);
    VReg idxForAddr = load8(idxSlot);
    VReg elemAddr = emitScaledAddr(baseCur, idxForAddr, elemSize);
    if (elemAgg) {
        VReg vaddr = fn_->newVReg();
        emit({MOpcode::LeaSlot, {MOperand::defVReg(vaddr), MOperand::slot(vslot)}});
        emitStructCopy(ElemAddr{vaddr, 0}, ElemAddr{elemAddr, 0}, elemSize);
    } else if (elemFloat) {
        std::uint8_t fw = floatWidthOf(elemTy);
        VReg v = fn_->newVReg(RegClass::XMM);
        emitFW(MOpcode::FLoadInd,
               {MOperand::defVReg(v), MOperand::useVReg(elemAddr), MOperand::immediate(0)}, fw);
        emitFW(MOpcode::FStore, {MOperand::slot(vslot), MOperand::useVReg(v)}, fw);
    } else {
        std::uint8_t w = static_cast<std::uint8_t>(widthOf(elemTy));
        bool sgn = isSignedOf(elemTy);
        VReg v = fn_->newVReg();
        MInst ld{MOpcode::LoadInd,
                 {MOperand::defVReg(v), MOperand::useVReg(elemAddr), MOperand::immediate(0)}};
        ld.width = w; ld.isSigned = sgn; emit(ld);
        MInst st{MOpcode::Store, {MOperand::slot(vslot), MOperand::useVReg(v)}};
        st.width = w; st.isSigned = sgn; emit(st);
    }
    loopStack_.push_back({exitB, contB, cleanups_.size()});
    selBlock(s.body);
    loopStack_.pop_back();
    if (!failed_) emit({MOpcode::Jmp, {MOperand::lbl(contB)}});

    // cont: ++idx, then re-test.
    curBlock_ = contB;
    VReg idxN = load8(idxSlot);
    VReg one = fn_->newVReg();
    emit({MOpcode::MovRI, {MOperand::defVReg(one), MOperand::immediate(1)}});
    emit({MOpcode::Add, {MOperand::useDefVReg(idxN), MOperand::useVReg(one)}});
    store8(idxSlot, idxN);
    emit({MOpcode::Jmp, {MOperand::lbl(headerB)}});

    curBlock_ = exitB;
    popScope();
}

void InstructionSelector::selInfiniteLoop(const AST::InfiniteLoop& s) {
    // body: ... ; jmp body ; exit:   (exited only via break/return)
    std::uint32_t bodyB = fn_->addBlock();
    std::uint32_t exitB = fn_->addBlock();

    emit({MOpcode::Jmp, {MOperand::lbl(bodyB)}});

    curBlock_ = bodyB;
    // `break` exits to exitB; `skip` (continue) restarts the body at bodyB.
    loopStack_.push_back({exitB, bodyB, cleanups_.size()});
    selBlock(s.body);
    loopStack_.pop_back();
    if (!failed_) emit({MOpcode::Jmp, {MOperand::lbl(bodyB)}});

    curBlock_ = exitB;
}

void InstructionSelector::selWhen(const AST::WhenStatement& s) {
    // A single-armed conditional: `when (c) { ... }` == `if (c) { ... }`.
    // branch-if-FALSE -> join ; then... ; jmp join ; join:
    Cond trueCond;
    if (!selCondition(s.condition, trueCond)) return;
    Cond falseCond = invertCond(trueCond);

    std::uint32_t thenB = fn_->addBlock();
    std::uint32_t joinB = fn_->addBlock();

    MInst jcc{MOpcode::Jcc, {MOperand::lbl(joinB)}};
    jcc.cond = falseCond;
    emit(jcc);
    emit({MOpcode::Jmp, {MOperand::lbl(thenB)}});

    curBlock_ = thenB;
    selBlock(s.consequent);
    if (!failed_) emit({MOpcode::Jmp, {MOperand::lbl(joinB)}});

    curBlock_ = joinB;
}

void InstructionSelector::selSwitch(const AST::SwitchStatement& s) {
    Types::TypeRef subjTy = concreteTypeOf(s.subject.get());
    const Sema::SumTypeInfo* st =
        (subjTy && subjTy->kind == Types::Kind::Struct) ? sumTypeByName(subjTy->name)
                                                        : nullptr;
    if (!st) {
        fail("selector: switch subject is not a tagged-union value");
        return;
    }

    // Resolve the subject to an aggregate address and spill it so it survives the
    // arm bodies (which may contain calls). The tag lives at [base + 0].
    ElemAddr subjAddr = computeLValueAddr(s.subject.get());
    if (failed_) return;
    VReg base = materializeAddr(subjAddr);
    std::uint32_t baseSlot = fn_->addFrameSlot(8, 8, /*isSpill=*/false);
    { MInst stb{MOpcode::Store, {MOperand::slot(baseSlot), MOperand::useVReg(base)}};
      stb.width = 8; stb.isSigned = false; emit(stb); }

    std::uint32_t joinB = fn_->addBlock();

    std::vector<std::uint32_t> bodyBlocks;
    bodyBlocks.reserve(s.arms.size());
    for (size_t i = 0; i < s.arms.size(); ++i) bodyBlocks.push_back(fn_->addBlock());

    // Dispatch chain: load the tag, compare against each named variant.
    int defaultIdx = -1;
    for (size_t ai = 0; ai < s.arms.size(); ++ai) {
        const AST::SwitchArm& arm = s.arms[ai];
        if (arm.isDefault) { defaultIdx = static_cast<int>(ai); continue; }
        // Find this arm's variant tag.
        long long tagVal = 0;
        bool found = false;
        for (const auto& v : st->variants) {
            if (v.name == arm.variant) { tagVal = v.tag; found = true; break; }
        }
        if (!found) { fail("selector: unknown variant '" + arm.variant + "' in match"); return; }

        VReg tag = fn_->newVReg();
        { MInst ld{MOpcode::Load, {MOperand::defVReg(tag), MOperand::slot(baseSlot)}};
          ld.width = 8; ld.isSigned = false; /* load base */ emit(ld); }
        // tag holds base pointer right now; load the actual tag from [base+0].
        VReg tagVReg = fn_->newVReg();
        { MInst ld{MOpcode::LoadInd,
                   {MOperand::defVReg(tagVReg), MOperand::useVReg(tag), MOperand::immediate(0)}};
          ld.width = 8; ld.isSigned = false; emit(ld); }
        VReg want = fn_->newVReg();
        emit({MOpcode::MovRI, {MOperand::defVReg(want), MOperand::immediate(tagVal)}});
        emit({MOpcode::Cmp, {MOperand::useVReg(tagVReg), MOperand::useVReg(want)}});
        std::uint32_t missB = fn_->addBlock();
        { MInst jcc{MOpcode::Jcc, {MOperand::lbl(bodyBlocks[ai])}}; jcc.cond = Cond::EQ; emit(jcc); }
        emit({MOpcode::Jmp, {MOperand::lbl(missB)}});
        curBlock_ = missB;
    }
    if (defaultIdx >= 0) {
        emit({MOpcode::Jmp, {MOperand::lbl(bodyBlocks[defaultIdx])}});
    } else {
        emit({MOpcode::Jmp, {MOperand::lbl(joinB)}});
    }

    // Arm bodies. Each binds its variant's payload fields (loaded from the spilled
    // base) as locals, then runs the body in a fresh scope.
    for (size_t ai = 0; ai < s.arms.size(); ++ai) {
        curBlock_ = bodyBlocks[ai];
        const AST::SwitchArm& arm = s.arms[ai];
        pushScope();
        if (!arm.isDefault && !arm.bindings.empty()) {
            const Sema::SumVariant* v = nullptr;
            for (const auto& cand : st->variants) {
                if (cand.name == arm.variant) { v = &cand; break; }
            }
            if (v) {
                std::vector<std::int64_t> offsets;
                sumVariantFieldOffsets(*v, offsets);
                VReg b = fn_->newVReg();
                { MInst ld{MOpcode::Load, {MOperand::defVReg(b), MOperand::slot(baseSlot)}};
                  ld.width = 8; ld.isSigned = false; emit(ld); }
                for (size_t bi = 0; bi < arm.bindings.size() && bi < offsets.size(); ++bi) {
                    Types::TypeRef ft = v->payload[bi];
                    std::int64_t off = offsets[bi];
                    std::uint32_t bslot = fn_->addFrameSlot(8, 8, /*isSpill=*/false);
                    if (isFloatType(ft)) {
                        std::uint8_t fw = floatWidthOf(ft);
                        VReg fv = fn_->newVReg(RegClass::XMM);
                        emitFW(MOpcode::FLoadInd,
                               {MOperand::defVReg(fv), MOperand::useVReg(b), MOperand::immediate(off)}, fw);
                        emitFW(MOpcode::FStore, {MOperand::slot(bslot), MOperand::useVReg(fv)}, fw);
                        declareLocal(arm.bindings[bi], bslot, fw, false, LocalKind::Scalar, true, ft);
                    } else {
                        std::uint8_t w = static_cast<std::uint8_t>(widthOf(ft));
                        bool sgn = isSignedOf(ft);
                        VReg val = fn_->newVReg();
                        MInst ld{MOpcode::LoadInd,
                                 {MOperand::defVReg(val), MOperand::useVReg(b), MOperand::immediate(off)}};
                        ld.width = w; ld.isSigned = sgn; emit(ld);
                        MInst stv{MOpcode::Store, {MOperand::slot(bslot), MOperand::useVReg(val)}};
                        stv.width = w; stv.isSigned = sgn; emit(stv);
                        declareLocal(arm.bindings[bi], bslot, w, sgn, LocalKind::Scalar, false, ft);
                    }
                }
            }
        }
        selBlock(arm.body);
        popScope();
        if (failed_) return;
        emit({MOpcode::Jmp, {MOperand::lbl(joinB)}});
    }

    curBlock_ = joinB;
}

void InstructionSelector::selBreak() {
    if (loopStack_.empty()) {
        fail("selector: 'break' outside of a loop");
        return;
    }
    emitCleanupsTo(loopStack_.back().cleanupDepth, /*popEntries=*/false);
    if (failed_) return;
    emit({MOpcode::Jmp, {MOperand::lbl(loopStack_.back().breakBlock)}});
}

void InstructionSelector::selSkip() {
    if (loopStack_.empty()) {
        fail("selector: 'skip' outside of a loop");
        return;
    }
    emitCleanupsTo(loopStack_.back().cleanupDepth, /*popEntries=*/false);
    if (failed_) return;
    emit({MOpcode::Jmp, {MOperand::lbl(loopStack_.back().continueBlock)}});
}

bool InstructionSelector::emitCompare(const AST::NodePtr& l, const AST::NodePtr& r,
                                      const std::string& op, Cond& condOut) {
    // Floating-point comparison: ucomisd sets CF/ZF/PF like an unsigned compare,
    // so the ordered double relations map onto the unsigned condition codes.
    Types::TypeRef lt = concreteTypeOf(l.get());
    Types::TypeRef rt = concreteTypeOf(r.get());
    if (isFloat128(lt) || isFloat128(rt)) {
        fail("selector: f128 comparisons are not supported yet");
        return false;
    }
    if (isFloatType(lt) || isFloatType(rt)) {
        VReg lv = selExpr(l);
        VReg rv = selExpr(r);
        if (failed_) return false;
        std::uint8_t fw = floatComputeWidthOf(isFloatType(lt) ? lt : rt);
        emitFW(MOpcode::FCmp, {MOperand::useVReg(lv), MOperand::useVReg(rv)}, fw);
        if (op == "==") condOut = Cond::EQ;
        else if (op == "!=") condOut = Cond::NE;
        else if (op == "<") condOut = Cond::ULT;
        else if (op == "<=") condOut = Cond::ULE;
        else if (op == ">") condOut = Cond::UGT;
        else if (op == ">=") condOut = Cond::UGE;
        else { fail("selector: unsupported float comparison '" + op + "'"); return false; }
        return true;
    }

    // 128-bit comparison: operands evaluate to addresses of 16-byte values.
    // Compute a 0/1 boolean for the relation, then reduce to a flags test so the
    // caller's Jcc/SetCC (using condOut) works uniformly.
    if (isInt128(lt) || isInt128(rt)) {
        VReg res = selI128Compare(l, r, op, !isSignedOf(lt) && !isSignedOf(rt));
        if (failed_) return false;
        VReg zero = fn_->newVReg();
        emit({MOpcode::MovRI, {MOperand::defVReg(zero), MOperand::immediate(0)}});
        emit({MOpcode::Cmp, {MOperand::useVReg(res), MOperand::useVReg(zero)}});
        condOut = Cond::NE;  // res != 0  <=>  relation holds
        return true;
    }

    // Content-based string equality: `text == text` / `text != text` compares
    // BYTES (not pointers) via the null-safe runtime helper __ins_streq. Gated on
    // both operands being `text`; relational ops and text-vs-other-pointer keep
    // the generic (pointer) comparison below.
    if ((op == "==" || op == "!=") && lt && rt &&
        lt->kind == Types::Kind::Text && rt->kind == Types::Kind::Text) {
        VReg lv = selExpr(l);
        VReg rv = selExpr(r);
        if (failed_) return false;
        VReg res = emitRuntimeCall("__ins_streq", {lv, rv}, kInvalidVReg, false);
        VReg zero = fn_->newVReg();
        emit({MOpcode::MovRI, {MOperand::defVReg(zero), MOperand::immediate(0)}});
        emit({MOpcode::Cmp, {MOperand::useVReg(res), MOperand::useVReg(zero)}});
        // streq != 0  <=>  equal, so `==` holds when NE-to-zero; invert for `!=`.
        condOut = (op == "==") ? Cond::NE : Cond::EQ;
        return true;
    }

    VReg lv = selExpr(l);
    VReg rv = selExpr(r);
    if (failed_) return false;
    emit({MOpcode::Cmp, {MOperand::useVReg(lv), MOperand::useVReg(rv)}});

    // Relational ops pick signed vs unsigned condition codes from the operand
    // types (a comparison is unsigned only if both sides are unsigned).
    const bool uns = !isSignedOf(lt) && !isSignedOf(rt);
    if (op == "==") condOut = Cond::EQ;
    else if (op == "!=") condOut = Cond::NE;
    else if (op == "<") condOut = uns ? Cond::ULT : Cond::LT;
    else if (op == "<=") condOut = uns ? Cond::ULE : Cond::LE;
    else if (op == ">") condOut = uns ? Cond::UGT : Cond::GT;
    else if (op == ">=") condOut = uns ? Cond::UGE : Cond::GE;
    else { fail("selector: unsupported comparison '" + op + "'"); return false; }
    return true;
}

VReg InstructionSelector::selI128Compare(const AST::NodePtr& l, const AST::NodePtr& r,
                                         const std::string& op, bool unsignedCmp) {
    VReg aAddr = selExpr(l);
    VReg bAddr = selExpr(r);
    if (failed_) return kInvalidVReg;
    VReg aLo = loadHalf(aAddr, 0), aHi = loadHalf(aAddr, 8);
    VReg bLo = loadHalf(bAddr, 0), bHi = loadHalf(bAddr, 8);

    auto setcc = [&](VReg x, VReg y, Cond c) {
        emit({MOpcode::Cmp, {MOperand::useVReg(x), MOperand::useVReg(y)}});
        VReg b = fn_->newVReg();
        MInst s{MOpcode::SetCC, {MOperand::defVReg(b)}};
        s.cond = c;
        emit(s);
        return b;
    };
    auto andOf = [&](VReg x, VReg y) {
        VReg t = fn_->newVReg();
        emit({MOpcode::MovRR, {MOperand::defVReg(t), MOperand::useVReg(x)}});
        emit({MOpcode::And, {MOperand::useDefVReg(t), MOperand::useVReg(y)}});
        return t;
    };
    auto orOf = [&](VReg x, VReg y) {
        VReg t = fn_->newVReg();
        emit({MOpcode::MovRR, {MOperand::defVReg(t), MOperand::useVReg(x)}});
        emit({MOpcode::Or, {MOperand::useDefVReg(t), MOperand::useVReg(y)}});
        return t;
    };

    if (op == "==") {
        return andOf(setcc(aHi, bHi, Cond::EQ), setcc(aLo, bLo, Cond::EQ));
    }
    if (op == "!=") {
        return orOf(setcc(aHi, bHi, Cond::NE), setcc(aLo, bLo, Cond::NE));
    }

    // Relational: result = (aHi <strict> bHi) | ((aHi==bHi) & (aLo <rel-uns> bLo)).
    // The high word uses signed/unsigned per the operand signedness; the low word
    // is always unsigned. <strict> is the strict form (< or >) of the relation.
    Cond hiStrict, loRel;
    if (op == "<")      { hiStrict = unsignedCmp ? Cond::ULT : Cond::LT; loRel = Cond::ULT; }
    else if (op == "<=") { hiStrict = unsignedCmp ? Cond::ULT : Cond::LT; loRel = Cond::ULE; }
    else if (op == ">")  { hiStrict = unsignedCmp ? Cond::UGT : Cond::GT; loRel = Cond::UGT; }
    else if (op == ">=") { hiStrict = unsignedCmp ? Cond::UGT : Cond::GT; loRel = Cond::UGE; }
    else { fail("selector: unsupported i128 comparison '" + op + "'"); return kInvalidVReg; }

    VReg hiPart = setcc(aHi, bHi, hiStrict);
    VReg hiEq = setcc(aHi, bHi, Cond::EQ);
    VReg loPart = setcc(aLo, bLo, loRel);
    return orOf(hiPart, andOf(hiEq, loPart));
}

// Recognizes a comparison node and, if so, fills l/r/op for it. Returns false
// for non-comparison expressions.
bool InstructionSelector::asComparison(const AST::NodePtr& e, AST::NodePtr& lOut,
                                       AST::NodePtr& rOut, std::string& opOut) const {
    if (!e) return false;
    switch (e->nodeType()) {
        case AST::NodeType::EqualityCheck: {
            const auto& c = static_cast<const AST::EqualityCheckExpr&>(*e);
            lOut = c.left; rOut = c.right; opOut = c.op; return true;
        }
        case AST::NodeType::BinaryOperation: {
            const auto& b = static_cast<const AST::BinaryOperationExpr&>(*e);
            if (b.op == "<" || b.op == "<=" || b.op == ">" || b.op == ">=") {
                lOut = b.lhs; rOut = b.rhs; opOut = b.op; return true;
            }
            return false;
        }
        default: return false;
    }
}

bool InstructionSelector::selCondition(const AST::NodePtr& cond, Cond& condOut) {
    if (!cond) { fail("selector: empty condition"); return false; }

    AST::NodePtr l, r;
    std::string op;
    if (asComparison(cond, l, r, op)) {
        return emitCompare(l, r, op, condOut);
    }

    // Fallback: evaluate the expression and compare against zero (value != 0).
    VReg v = selExpr(cond);
    if (failed_) return false;
    VReg zero = fn_->newVReg();
    emit({MOpcode::MovRI, {MOperand::defVReg(zero), MOperand::immediate(0)}});
    emit({MOpcode::Cmp, {MOperand::useVReg(v), MOperand::useVReg(zero)}});
    condOut = Cond::NE;
    return true;
}

// Evaluates a comparison expression into a fresh 0/1 vreg (comparison as value).
VReg InstructionSelector::selComparisonValue(const AST::NodePtr& l,
                                             const AST::NodePtr& r,
                                             const std::string& op) {
    // Operator overloading for comparison/equality operators on class lhs.
    {
        VReg out = kInvalidVReg;
        if (selClassOperator(op, l, r, out)) return out;
    }
    Cond c;
    if (!emitCompare(l, r, op, c)) return kInvalidVReg;
    VReg res = fn_->newVReg();
    MInst set{MOpcode::SetCC, {MOperand::defVReg(res)}};
    set.cond = c;
    emit(set);
    return res;
}

VReg InstructionSelector::selExpr(const AST::NodePtr& expr) {
    if (!expr) { fail("selector: null expression"); return kInvalidVReg; }
    // f128 values are address-valued 16-byte memory objects. Arithmetic writes
    // into a fresh temp via __*tf3 soft-float helpers and yields the temp address.
    if (isFloat128(concreteTypeOf(expr.get()))) {
        std::uint32_t slot = fn_->addFrameSlot(16, 16, /*isSpill=*/false);
        VReg addr = fn_->newVReg();
        emit({MOpcode::LeaSlot, {MOperand::defVReg(addr), MOperand::slot(slot)}});
        selF128(expr, addr);
        return addr;
    }
    // 128-bit integers are modeled as 16-byte memory values: evaluate into a
    // fresh stack temp and yield its address (mirroring how aggregates are
    // represented). All 128-bit arithmetic happens in selI128.
    if (isInt128(concreteTypeOf(expr.get()))) {
        std::uint32_t slot = fn_->addFrameSlot(16, 16, /*isSpill=*/false);
        VReg addr = fn_->newVReg();
        emit({MOpcode::LeaSlot, {MOperand::defVReg(addr), MOperand::slot(slot)}});
        selI128(expr, addr);
        return addr;
    }
    switch (expr->nodeType()) {
        case AST::NodeType::IntegerLiteral: {
            const auto& lit = static_cast<const AST::IntegerLiteral&>(*expr);
            VReg v = fn_->newVReg();
            emit({MOpcode::MovRI, {MOperand::defVReg(v), MOperand::immediate(lit.value)}});
            return v;
        }
        case AST::NodeType::BoolLiteral: {
            const auto& lit = static_cast<const AST::BoolLiteral&>(*expr);
            VReg v = fn_->newVReg();
            emit({MOpcode::MovRI,
                  {MOperand::defVReg(v), MOperand::immediate(lit.value ? 1 : 0)}});
            return v;
        }
        case AST::NodeType::FloatLiteral: {
            return selFloatLiteral(static_cast<const AST::FloatLiteral&>(*expr));
        }
        case AST::NodeType::StringLiteral: {
            return selStringLiteral(static_cast<const AST::StringLiteral&>(*expr));
        }
        case AST::NodeType::BuiltinCall: {
            return selBuiltinCall(static_cast<const AST::BuiltinCallExpr&>(*expr));
        }
        case AST::NodeType::IdentifierExpr: {
            const auto& id = static_cast<const AST::IdentifierExpr&>(*expr);
            LocalInfo li;
            if (!lookupLocal(id.name, li)) {
                // Module-level global: load through its RIP-relative address.
                if (const Sema::GlobalInfo* g = lookupGlobal(id.name)) {
                    Types::TypeRef gt = g->type;
                    VReg addr = globalAddr(*g);
                    if (isFloatType(gt)) {
                        VReg v = fn_->newVReg(RegClass::XMM);
                        emitFW(MOpcode::FLoadInd,
                               {MOperand::defVReg(v), MOperand::useVReg(addr),
                                MOperand::immediate(0)},
                               floatWidthOf(gt));
                        return v;
                    }
                    if (gt && (gt->kind == Types::Kind::Struct ||
                               gt->kind == Types::Kind::Class ||
                               gt->kind == Types::Kind::Array)) {
                        fail("selector: cannot use aggregate global '" + id.name +
                             "' as a scalar value");
                        return kInvalidVReg;
                    }
                    VReg v = fn_->newVReg();
                    MInst ld{MOpcode::LoadInd,
                             {MOperand::defVReg(v), MOperand::useVReg(addr),
                              MOperand::immediate(0)}};
                    ld.width = static_cast<std::uint8_t>(widthOf(gt));
                    ld.isSigned = isSignedOf(gt);
                    emit(ld);
                    return v;
                }
                for (const auto& e : sema_.enums) {
                    auto it = e.variants.find(id.name);
                    if (it != e.variants.end()) {
                        VReg v = fn_->newVReg();
                        emit({MOpcode::MovRI,
                              {MOperand::defVReg(v),
                               MOperand::immediate(it->second)}});
                        return v;
                    }
                }
                fail("selector: unknown identifier '" + id.name + "'");
                return kInvalidVReg;
            }
            // A whole struct/aggregate has no scalar register value; it is only
            // usable through its address (&s) or a field/element (s.f, s[i]).
            if (li.kind != LocalKind::Scalar) {
                fail("selector: cannot use aggregate '" + id.name +
                     "' as a scalar value");
                return kInvalidVReg;
            }
            // A float scalar lives in XMM: load via movss/movsd into an XMM vreg.
            if (li.isFloat) {
                VReg v = fn_->newVReg(RegClass::XMM);
                emitFW(MOpcode::FLoad, {MOperand::defVReg(v), MOperand::slot(li.slot)},
                       li.width);
                return v;
            }
            VReg v = fn_->newVReg();
            // Load sign/zero-extends the low `width` bytes to a 64-bit value, so
            // downstream 64-bit arithmetic/compares see a correctly extended val.
            MInst ld{MOpcode::Load, {MOperand::defVReg(v), MOperand::slot(li.slot)}};
            ld.width = li.width; ld.isSigned = li.isSigned;
            emit(ld);
            return v;
        }
        case AST::NodeType::BinaryOperation: {
            const auto& b = static_cast<const AST::BinaryOperationExpr&>(*expr);
            // A relational comparison used as a value -> 0/1 via SetCC.
            if (b.op == "<" || b.op == "<=" || b.op == ">" || b.op == ">=") {
                return selComparisonValue(b.lhs, b.rhs, b.op);
            }
            return selBinary(b.op, b.lhs, b.rhs, concreteTypeOf(&b));
        }
        case AST::NodeType::EqualityCheck: {
            const auto& e = static_cast<const AST::EqualityCheckExpr&>(*expr);
            return selComparisonValue(e.left, e.right, e.op);
        }
        case AST::NodeType::ShiftOperation: {
            const auto& sh = static_cast<const AST::ShiftOperationExpr&>(*expr);
            return selShift(sh);
        }
        case AST::NodeType::UnaryExpr: {
            const auto& un = static_cast<const AST::UnaryExpr&>(*expr);
            return selUnary(un);
        }
        case AST::NodeType::LogicalOperation: {
            const auto& lo = static_cast<const AST::LogicalOperationExpr&>(*expr);
            return selLogical(lo);
        }
        case AST::NodeType::FunctionCall:
            return selCall(static_cast<const AST::FunctionCallExpr&>(*expr));
        case AST::NodeType::CastExpr: {
            const auto& c = static_cast<const AST::CastExpr&>(*expr);
            return selCast(c);
        }
        case AST::NodeType::AddressOfExpr: {
            const auto& a = static_cast<const AST::AddressOfExpr&>(*expr);
            return selAddressOf(a);
        }
        case AST::NodeType::Lambda: {
            const auto& lam = static_cast<const AST::LambdaExpr&>(*expr);
            if (!lam.captures.empty()) {
                // Capturing lambda: allocate { fn_ptr, cap1, cap2, ... } on the
                // heap and return a pointer to it. The env struct was registered
                // by sema; compute its total size and the offset of each field.
                const Sema::StructInfo* si = nullptr;
                for (const auto& s : sema_.structs) {
                    if (s.name == lam.envStructName) { si = &s; break; }
                }
                if (!si) {
                    fail("selector: closure env struct '" + lam.envStructName +
                         "' not registered");
                    return kInvalidVReg;
                }
                unsigned envSz = 0;
                std::vector<unsigned> fieldOffs;
                for (unsigned fi = 0; fi < si->fields.size(); ++fi) {
                    fieldOffs.push_back(envSz);
                    unsigned fa = si->packed ? 1 : alignOf(si->fields[fi].second);
                    envSz = alignUp(envSz, fa);
                    envSz += sizeOf(si->fields[fi].second);
                }
                if (envSz == 0) envSz = 8;
                VReg sizeV = fn_->newVReg();
                emit({MOpcode::MovRI,
                      {MOperand::defVReg(sizeV),
                       MOperand::immediate(static_cast<std::int64_t>(envSz))}});
                VReg heap = emitMalloc(sizeV);
                if (failed_) return kInvalidVReg;

                // Store fn_ptr in the __fn field (always field 0, offset 0)
                for (const auto& fi : sema_.functions) {
                    if (fi.name != lam.name && fi.mangledName != lam.name) continue;
                    const std::string sym = fi.mangledName.empty() ? fi.name : fi.mangledName;
                    VReg fnAddr = fn_->newVReg();
                    emit({MOpcode::Lea, {MOperand::defVReg(fnAddr), MOperand::sym(sym)}});
                    MInst st{MOpcode::StoreInd,
                             {MOperand::useVReg(heap), MOperand::immediate(0),
                              MOperand::useVReg(fnAddr)}};
                    st.width = 8; st.isSigned = false; emit(st);
                    break;
                }

                // Store each captured variable at fieldOffs[ci + 1] (skip __fn)
                for (unsigned ci = 0; ci < lam.captures.size(); ++ci) {
                    unsigned fidx = ci + 1;  // +1 skips the __fn field
                    if (fidx >= fieldOffs.size()) break;
                    const std::string& capName = lam.captures[ci];
                    LocalInfo li;
                    if (!lookupLocal(capName, li)) {
                        fail("selector: closure capture '" + capName +
                             "' not a local in enclosing function");
                        return kInvalidVReg;
                    }
                    Types::TypeRef capTy = si->fields[fidx].second;
                    VReg capVal;
                    if (isFloatType(capTy)) {
                        capVal = fn_->newVReg(RegClass::XMM);
                        std::uint8_t fw = floatWidthOf(capTy);
                        emitFW(MOpcode::FLoad,
                               {MOperand::defVReg(capVal), MOperand::slot(li.slot)}, fw);
                    } else if (li.kind == LocalKind::AggregateValue ||
                               li.kind == LocalKind::AggregatePtr) {
                        // Aggregate: copy bytes from local slot to heap
                        VReg srcAddr = fn_->newVReg();
                        emit({MOpcode::LeaSlot,
                              {MOperand::defVReg(srcAddr), MOperand::slot(li.slot)}});
                        unsigned sz = sizeOf(capTy);
                        emitStructCopy(ElemAddr{heap, static_cast<std::int64_t>(fieldOffs[fidx])},
                                       ElemAddr{srcAddr, 0}, sz);
                        continue;
                    } else {
                        capVal = fn_->newVReg();
                        MInst ld{MOpcode::Load,
                                 {MOperand::defVReg(capVal), MOperand::slot(li.slot)}};
                        ld.width = static_cast<std::uint8_t>(widthOf(capTy));
                        ld.isSigned = isSignedOf(capTy);
                        emit(ld);
                    }
                    if (isFloatType(capTy)) {
                        std::uint8_t fw = floatWidthOf(capTy);
                        emitFW(MOpcode::FStoreInd,
                               {MOperand::useVReg(heap),
                                MOperand::immediate(static_cast<std::int64_t>(fieldOffs[fidx])),
                                MOperand::useVReg(capVal)}, fw);
                    } else {
                        MInst st2{MOpcode::StoreInd,
                                  {MOperand::useVReg(heap),
                                   MOperand::immediate(static_cast<std::int64_t>(fieldOffs[fidx])),
                                   MOperand::useVReg(capVal)}};
                        st2.width = static_cast<std::uint8_t>(widthOf(capTy));
                        st2.isSigned = false;
                        emit(st2);
                    }
                }
                return heap;
            }
            // Non-capturing lambda: plain function pointer (unchanged).
            for (const auto& fi : sema_.functions) {
                if (fi.name != lam.name && fi.mangledName != lam.name) continue;
                const std::string sym = fi.mangledName.empty() ? fi.name : fi.mangledName;
                VReg a = fn_->newVReg();
                emit({MOpcode::Lea, {MOperand::defVReg(a), MOperand::sym(sym)}});
                return a;
            }
            fail("selector: lambda '" + lam.name + "' was not registered by sema");
            return kInvalidVReg;
        }
        case AST::NodeType::DereferenceExpr: {
            const auto& d = static_cast<const AST::DereferenceExpr&>(*expr);
            return selDeref(d);
        }
        case AST::NodeType::InlineAsmExpr: {
            // Only the block form reaches here: the legacy constrained form is
            // parsed as a call and handled among the identifier intrinsics.
            selAsmBlock(static_cast<const AST::InlineAsmExpr&>(*expr));
            return kInvalidVReg;
        }
        case AST::NodeType::MemberAccess: {
            const auto& m = static_cast<const AST::MemberAccessExpr&>(*expr);
            // `X.insize`: a compile-time byte size. Sema recorded which type this
            // node measures but not its size, because the layout is decided here:
            // sizeOf() is the same function that places fields, sizes sum-type
            // payloads and honours `packed`/`align`. Sizing the type with it means
            // `.insize` can never disagree with the bytes actually emitted.
            {
                auto it = sema_.insizeTypes.find(&m);
                if (it != sema_.insizeTypes.end()) {
                    VReg v = fn_->newVReg();
                    emit({MOpcode::MovRI,
                          {MOperand::defVReg(v),
                           MOperand::immediate(
                               static_cast<std::int64_t>(sizeOf(it->second)))}});
                    return v;
                }
                // `X.inalign`: the alignment, from the same layout rules.
                auto al = sema_.inalignTypes.find(&m);
                if (al != sema_.inalignTypes.end()) {
                    VReg v = fn_->newVReg();
                    emit({MOpcode::MovRI,
                          {MOperand::defVReg(v),
                           MOperand::immediate(
                               static_cast<std::int64_t>(alignOf(al->second)))}});
                    return v;
                }
            }
            // `E.Variant` unit-variant value: build the tagged aggregate.
            const Sema::SumTypeInfo* st = nullptr;
            const Sema::SumVariant* v = nullptr;
            if (resolveSumVariant(m, st, v) && v) {
                ElemAddr a = materializeSumConstruct(*st, *v, {});
                if (failed_) return kInvalidVReg;
                return materializeAddr(a);
            }
            return selMemberLoad(m);
        }
        case AST::NodeType::NewExpression:
            return selNew(static_cast<const AST::NewExpression&>(*expr));
        case AST::NodeType::DeleteExpression:
            selDelete(static_cast<const AST::DeleteExpression&>(*expr));
            return kInvalidVReg;
        case AST::NodeType::ArrayLiteral: {
            // An array literal used as an rvalue: materialize it on the stack and
            // yield its base address (usable as a pointer to the first element).
            ElemAddr a = materializeArrayLiteral(
                static_cast<const AST::ArrayLiteral&>(*expr));
            if (failed_) return kInvalidVReg;
            return materializeAddr(a);
        }
        case AST::NodeType::SliceExpr: {
            // A sub-slice value: materialize its { ptr, len } header and yield the
            // header address (a slice is an addressed aggregate value).
            ElemAddr a = materializeSliceExpr(static_cast<const AST::SliceExpr&>(*expr));
            if (failed_) return kInvalidVReg;
            return materializeAddr(a);
        }
        default:
            fail("selector: unsupported expression kind");
            return kInvalidVReg;
    }
}

VReg InstructionSelector::selBinary(const std::string& op, const AST::NodePtr& l,
                                    const AST::NodePtr& r, Types::TypeRef resultTy) {
    // Operator overloading: if the lhs is a class with an `operator <op>` method,
    // dispatch to it (this = lhs, arg1 = rhs).
    {
        VReg out = kInvalidVReg;
        if (selClassOperator(op, l, r, out)) return out;
    }
    // Floating-point arithmetic dispatches to the SSE path.
    if (isFloat128(resultTy)) {
        std::uint32_t slot = fn_->addFrameSlot(16, 16, /*isSpill=*/false);
        VReg addr = fn_->newVReg();
        emit({MOpcode::LeaSlot, {MOperand::defVReg(addr), MOperand::slot(slot)}});
        selF128Binary(op, l, r, addr);
        return addr;
    }
    if (isFloatType(resultTy)) {
        VReg fr = selFloatBinary(op, l, r, floatComputeWidthOf(resultTy));
        if (failed_) return fr;
        // Each f16 operation rounds its result to half precision.
        return narrowToHalfIfNeeded(fr, resultTy);
    }
    // Pointer arithmetic: `ptr + int` / `ptr - int`. Sema records the result type
    // as the pointer type, so scale the integer operand by the element size and
    // add/subtract it from the pointer value.
    if ((op == "+" || op == "-") && resultTy && resultTy->isPointerLike()) {
        return selPointerArith(op, l, r, resultTy);
    }

    VReg lv = selExpr(l);
    VReg rv = selExpr(r);
    if (failed_) return kInvalidVReg;

    const bool resSigned = isSignedOf(resultTy);
    std::uint8_t w = static_cast<std::uint8_t>(widthOf(resultTy));

    // Division and modulo use the fixed RDX:RAX convention; model them with a
    // dedicated opcode (operands: def result, use dividend, use divisor) so
    // lowering can emit the cqo/idiv (or xor-edx/div) sequence and the allocator
    // knows RAX/RDX are clobbered.
    if (op == "/" || op == "%") {
        VReg res = fn_->newVReg();
        MInst di{op == "/" ? MOpcode::Div : MOpcode::Mod,
                 {MOperand::defVReg(res), MOperand::useVReg(lv), MOperand::useVReg(rv)}};
        di.isSigned = resSigned;
        di.clobbers = {PhysReg::RAX, PhysReg::RDX};
        emit(di);
        if (w < 8) {
            MInst ext{MOpcode::Ext, {MOperand::useDefVReg(res)}};
            ext.width = w; ext.isSigned = resSigned;
            emit(ext);
        }
        return res;
    }

    MOpcode mop;
    if (op == "+") mop = MOpcode::Add;
    else if (op == "-") mop = MOpcode::Sub;
    else if (op == "*") mop = MOpcode::IMul;
    else if (op == "&") mop = MOpcode::And;
    else if (op == "|") mop = MOpcode::Or;
    else if (op == "^") mop = MOpcode::Xor;
    else { fail("selector: unsupported binary operator '" + op + "'"); return kInvalidVReg; }

    // Two-address form: result vreg = lv, then op= rv. Copy lv into a fresh
    // result vreg first so the source isn't clobbered.
    VReg res = fn_->newVReg();
    emit({MOpcode::MovRR, {MOperand::defVReg(res), MOperand::useVReg(lv)}});
    emit({mop, {MOperand::useDefVReg(res), MOperand::useVReg(rv)}});

    // Re-extend the result to its declared width so narrower arithmetic wraps
    // correctly (e.g. i8 overflow) and stays canonical for later 64-bit use.
    if (w < 8) {
        MInst ext{MOpcode::Ext, {MOperand::useDefVReg(res)}};
        ext.width = w; ext.isSigned = resSigned;
        emit(ext);
    }
    return res;
}

// --- 128-bit integer support ----------------------------------------------
// 128-bit integers are kept in 16-byte memory (a stack temp or an aggregate
// lvalue): the low 64 bits at [addr+0], the high 64 bits at [addr+8]. These
// two helpers move a single half in/out of a GP vreg.
VReg InstructionSelector::loadHalf(VReg addr, std::int64_t off) {
    VReg v = fn_->newVReg();
    MInst ld{MOpcode::LoadInd,
             {MOperand::defVReg(v), MOperand::useVReg(addr), MOperand::immediate(off)}};
    ld.width = 8; ld.isSigned = false;
    emit(ld);
    return v;
}

void InstructionSelector::storeHalf(VReg addr, std::int64_t off, VReg value) {
    MInst st{MOpcode::StoreInd,
             {MOperand::useVReg(addr), MOperand::immediate(off), MOperand::useVReg(value)}};
    st.width = 8; st.isSigned = false;
    emit(st);
}

void InstructionSelector::selI128(const AST::NodePtr& expr, VReg destAddr) {
    if (!expr) { fail("selector: null i128 expression"); return; }
    Types::TypeRef ty = concreteTypeOf(expr.get());
    const bool isSigned = ty && ty->isSigned;

    switch (expr->nodeType()) {
        case AST::NodeType::IntegerLiteral: {
            const auto& lit = static_cast<const AST::IntegerLiteral&>(*expr);
            unsigned __int128 uv = static_cast<unsigned __int128>(lit.value);
            std::int64_t lo = static_cast<std::int64_t>(static_cast<std::uint64_t>(uv));
            std::int64_t hi =
                static_cast<std::int64_t>(static_cast<std::uint64_t>(uv >> 64));
            VReg vlo = fn_->newVReg();
            VReg vhi = fn_->newVReg();
            emit({MOpcode::MovRI, {MOperand::defVReg(vlo), MOperand::immediate(lo)}});
            emit({MOpcode::MovRI, {MOperand::defVReg(vhi), MOperand::immediate(hi)}});
            storeHalf(destAddr, 0, vlo);
            storeHalf(destAddr, 8, vhi);
            return;
        }
        case AST::NodeType::IdentifierExpr:
        case AST::NodeType::MemberAccess:
        case AST::NodeType::DereferenceExpr: {
            // Copy a 16-byte i128 lvalue (local/global/field/element/*ptr) into the
            // destination via its computed address.
            ElemAddr src = computeLValueAddr(static_cast<const AST::ExprAST*>(expr.get()));
            if (failed_) return;
            VReg srcAddr = src.base;
            std::int64_t off = src.disp;
            VReg lo = loadHalf(srcAddr, off + 0);
            VReg hi = loadHalf(srcAddr, off + 8);
            storeHalf(destAddr, 0, lo);
            storeHalf(destAddr, 8, hi);
            return;
        }
        case AST::NodeType::CastExpr: {
            const auto& cast = static_cast<const AST::CastExpr&>(*expr);
            Types::TypeRef from = concreteTypeOf(cast.expression.get());
            if (isInt128(from)) {
                // i128 -> i128 (e.g. u128<->i128): just copy the 16 bytes.
                selI128(cast.expression, destAddr);
                return;
            }
            // Widen a <=64-bit integer/pointer to 128 bits. The source low word is
            // its value; the high word is the sign extension (signed) or zero.
            VReg lo = selExpr(cast.expression);
            if (failed_) return;
            storeHalf(destAddr, 0, lo);
            VReg hi = fn_->newVReg();
            if (isSigned && isSignedOf(from)) {
                // arithmetic: hi = lo >> 63 (replicate sign bit)
                emit({MOpcode::MovRR, {MOperand::defVReg(hi), MOperand::useVReg(lo)}});
                VReg cnt = fn_->newVReg();
                emit({MOpcode::MovRI, {MOperand::defVReg(cnt), MOperand::immediate(63)}});
                MInst sh{MOpcode::Shr, {MOperand::useDefVReg(hi), MOperand::useVReg(cnt)}};
                sh.isSigned = true;  // arithmetic shift -> replicates sign
                sh.clobbers = {PhysReg::RCX};
                emit(sh);
            } else {
                emit({MOpcode::MovRI, {MOperand::defVReg(hi), MOperand::immediate(0)}});
            }
            storeHalf(destAddr, 8, hi);
            return;
        }
        case AST::NodeType::BinaryOperation: {
            const auto& b = static_cast<const AST::BinaryOperationExpr&>(*expr);
            selI128Binary(b.op, b.lhs, b.rhs, destAddr, isSigned);
            return;
        }
        case AST::NodeType::ShiftOperation: {
            const auto& sh = static_cast<const AST::ShiftOperationExpr&>(*expr);
            selI128Shift(sh.op, sh.lhs, sh.rhs, destAddr, isSigned);
            return;
        }
        case AST::NodeType::UnaryExpr: {
            const auto& un = static_cast<const AST::UnaryExpr&>(*expr);
            if (un.op == "+") { selI128(un.operand, destAddr); return; }
            if (un.op != "-" && un.op != "~") {
                fail("selector: unsupported i128 unary operator '" + un.op + "'");
                return;
            }
            // Evaluate operand into the destination, then complement/negate in place.
            selI128(un.operand, destAddr);
            if (failed_) return;
            VReg lo = loadHalf(destAddr, 0);
            VReg hi = loadHalf(destAddr, 8);
            emit({MOpcode::Not, {MOperand::useDefVReg(lo)}});
            emit({MOpcode::Not, {MOperand::useDefVReg(hi)}});
            if (un.op == "-") {
                // -x = ~x + 1 across 128 bits (carry propagated from lo to hi).
                VReg one = fn_->newVReg();
                emit({MOpcode::MovRI, {MOperand::defVReg(one), MOperand::immediate(1)}});
                VReg oldLo = fn_->newVReg();
                emit({MOpcode::MovRR, {MOperand::defVReg(oldLo), MOperand::useVReg(lo)}});
                emit({MOpcode::Add, {MOperand::useDefVReg(lo), MOperand::useVReg(one)}});
                VReg carry = i128AddCarry(lo, oldLo);  // carry = (lo < oldLo)
                emit({MOpcode::Add, {MOperand::useDefVReg(hi), MOperand::useVReg(carry)}});
            }
            storeHalf(destAddr, 0, lo);
            storeHalf(destAddr, 8, hi);
            return;
        }
        case AST::NodeType::FunctionCall: {
            // A call returning i128: the value comes back in RAX:RDX (both SysV and
            // Win64 return a 16-byte integer in the int-return register pair).
            VReg retAddr = selCallReturningI128(expr);
            if (failed_) return;
            if (retAddr != destAddr) {
                VReg lo = loadHalf(retAddr, 0);
                VReg hi = loadHalf(retAddr, 8);
                storeHalf(destAddr, 0, lo);
                storeHalf(destAddr, 8, hi);
            }
            return;
        }
        default:
            fail("selector: unsupported i128 expression form");
            return;
    }
}

void InstructionSelector::selF128(const AST::NodePtr& expr, VReg destAddr) {
    if (!expr) { fail("selector: null f128 expression"); return; }

    switch (expr->nodeType()) {
        case AST::NodeType::FloatLiteral: {
            const auto& lit = static_cast<const AST::FloatLiteral&>(*expr);
            auto [loBits, hiBits] = doubleToF128Bits(lit.value);
            VReg lo = fn_->newVReg();
            VReg hi = fn_->newVReg();
            emit({MOpcode::MovRI,
                  {MOperand::defVReg(lo),
                   MOperand::immediate(static_cast<std::int64_t>(loBits))}});
            emit({MOpcode::MovRI,
                  {MOperand::defVReg(hi),
                   MOperand::immediate(static_cast<std::int64_t>(hiBits))}});
            storeHalf(destAddr, 0, lo);
            storeHalf(destAddr, 8, hi);
            return;
        }
        case AST::NodeType::IdentifierExpr:
        case AST::NodeType::MemberAccess:
        case AST::NodeType::DereferenceExpr: {
            ElemAddr src = computeLValueAddr(static_cast<const AST::ExprAST*>(expr.get()));
            if (failed_) return;
            VReg srcAddr = materializeAddr(src);
            VReg lo = loadHalf(srcAddr, 0);
            VReg hi = loadHalf(srcAddr, 8);
            storeHalf(destAddr, 0, lo);
            storeHalf(destAddr, 8, hi);
            return;
        }
        case AST::NodeType::CastExpr: {
            const auto& cast = static_cast<const AST::CastExpr&>(*expr);
            Types::TypeRef from = concreteTypeOf(cast.expression.get());
            if (isFloat128(from)) {
                selF128(cast.expression, destAddr);
                return;
            }
            fail("selector: f128 conversions are not supported yet");
            return;
        }
        case AST::NodeType::BinaryOperation: {
            const auto& b = static_cast<const AST::BinaryOperationExpr&>(*expr);
            selF128Binary(b.op, b.lhs, b.rhs, destAddr);
            return;
        }
        case AST::NodeType::UnaryExpr: {
            const auto& un = static_cast<const AST::UnaryExpr&>(*expr);
            if (un.op == "+") { selF128(un.operand, destAddr); return; }
            if (un.op == "-") {
                selF128(un.operand, destAddr);
                if (failed_) return;
                VReg hi = loadHalf(destAddr, 8);
                VReg sign = fn_->newVReg();
                emit({MOpcode::MovRI,
                      {MOperand::defVReg(sign),
                       MOperand::immediate(static_cast<std::int64_t>(0x8000000000000000ULL))}});
                emit({MOpcode::Xor, {MOperand::useDefVReg(hi), MOperand::useVReg(sign)}});
                storeHalf(destAddr, 8, hi);
                return;
            }
            fail("selector: unsupported f128 unary operator '" + un.op + "'");
            return;
        }
        case AST::NodeType::FunctionCall:
            fail("selector: f128 call ABI is not supported yet");
            return;
        default:
            fail("selector: unsupported f128 expression form");
            return;
    }
}

// carry of `oldVal + something == sum`: an unsigned add overflowed iff the sum
// wrapped below either input, i.e. (sum <u oldVal). Returns 0/1.
VReg InstructionSelector::i128AddCarry(VReg sum, VReg operandBefore) {
    emit({MOpcode::Cmp, {MOperand::useVReg(sum), MOperand::useVReg(operandBefore)}});
    VReg c = fn_->newVReg();
    MInst set{MOpcode::SetCC, {MOperand::defVReg(c)}};
    set.cond = Cond::ULT;
    emit(set);
    return c;
}

void InstructionSelector::selI128Binary(const std::string& op, const AST::NodePtr& l,
                                        const AST::NodePtr& r, VReg destAddr,
                                        bool isSigned) {
    // Evaluate both operands into their own 16-byte temps (their addresses come
    // back from selExpr because they are i128-typed).
    VReg aAddr = selExpr(l);
    VReg bAddr = selExpr(r);
    if (failed_) return;

    if (op == "/" || op == "%") {
        selI128DivMod(op, aAddr, bAddr, destAddr, isSigned);
        return;
    }

    VReg aLo = loadHalf(aAddr, 0), aHi = loadHalf(aAddr, 8);
    VReg bLo = loadHalf(bAddr, 0), bHi = loadHalf(bAddr, 8);

    if (op == "&" || op == "|" || op == "^") {
        MOpcode mop = op == "&" ? MOpcode::And : (op == "|" ? MOpcode::Or : MOpcode::Xor);
        emit({mop, {MOperand::useDefVReg(aLo), MOperand::useVReg(bLo)}});
        emit({mop, {MOperand::useDefVReg(aHi), MOperand::useVReg(bHi)}});
        storeHalf(destAddr, 0, aLo);
        storeHalf(destAddr, 8, aHi);
        return;
    }

    if (op == "+") {
        VReg oldLo = fn_->newVReg();
        emit({MOpcode::MovRR, {MOperand::defVReg(oldLo), MOperand::useVReg(aLo)}});
        emit({MOpcode::Add, {MOperand::useDefVReg(aLo), MOperand::useVReg(bLo)}});
        VReg carry = i128AddCarry(aLo, oldLo);
        emit({MOpcode::Add, {MOperand::useDefVReg(aHi), MOperand::useVReg(bHi)}});
        emit({MOpcode::Add, {MOperand::useDefVReg(aHi), MOperand::useVReg(carry)}});
        storeHalf(destAddr, 0, aLo);
        storeHalf(destAddr, 8, aHi);
        return;
    }

    if (op == "-") {
        // borrow = (aLo <u bLo); lo = aLo - bLo; hi = aHi - bHi - borrow.
        emit({MOpcode::Cmp, {MOperand::useVReg(aLo), MOperand::useVReg(bLo)}});
        VReg borrow = fn_->newVReg();
        MInst set{MOpcode::SetCC, {MOperand::defVReg(borrow)}};
        set.cond = Cond::ULT;
        emit(set);
        emit({MOpcode::Sub, {MOperand::useDefVReg(aLo), MOperand::useVReg(bLo)}});
        emit({MOpcode::Sub, {MOperand::useDefVReg(aHi), MOperand::useVReg(bHi)}});
        emit({MOpcode::Sub, {MOperand::useDefVReg(aHi), MOperand::useVReg(borrow)}});
        storeHalf(destAddr, 0, aLo);
        storeHalf(destAddr, 8, aHi);
        return;
    }

    if (op == "*") {
        // (aHi:aLo) * (bHi:bLo), keeping the low 128 bits:
        //   lo  = aLo * bLo                       (low 64)
        //   hi  = high64(aLo*bLo) + aLo*bHi + aHi*bLo
        VReg lo = fn_->newVReg();
        emit({MOpcode::MovRR, {MOperand::defVReg(lo), MOperand::useVReg(aLo)}});
        emit({MOpcode::IMul, {MOperand::useDefVReg(lo), MOperand::useVReg(bLo)}});

        VReg hi = fn_->newVReg();
        MInst mh{MOpcode::UMulHi,
                 {MOperand::defVReg(hi), MOperand::useVReg(aLo), MOperand::useVReg(bLo)}};
        mh.clobbers = {PhysReg::RAX, PhysReg::RDX};
        emit(mh);

        VReg t1 = fn_->newVReg();
        emit({MOpcode::MovRR, {MOperand::defVReg(t1), MOperand::useVReg(aLo)}});
        emit({MOpcode::IMul, {MOperand::useDefVReg(t1), MOperand::useVReg(bHi)}});
        emit({MOpcode::Add, {MOperand::useDefVReg(hi), MOperand::useVReg(t1)}});

        VReg t2 = fn_->newVReg();
        emit({MOpcode::MovRR, {MOperand::defVReg(t2), MOperand::useVReg(aHi)}});
        emit({MOpcode::IMul, {MOperand::useDefVReg(t2), MOperand::useVReg(bLo)}});
        emit({MOpcode::Add, {MOperand::useDefVReg(hi), MOperand::useVReg(t2)}});

        storeHalf(destAddr, 0, lo);
        storeHalf(destAddr, 8, hi);
        return;
    }

    fail("selector: unsupported i128 binary operator '" + op + "'");
}

void InstructionSelector::selI128Shift(const std::string& op,
                                       const AST::NodePtr& valueExpr,
                                       const AST::NodePtr& amountExpr, VReg destAddr,
                                       bool isSigned) {
    const bool left = (op == "<<");
    if (!left && op != ">>") {
        fail("selector: unsupported i128 shift operator '" + op + "'");
        return;
    }
    // Evaluate the value into destAddr first; the shift then reads its halves and
    // writes the (possibly shifted) result back, branching on whether the shift
    // amount is < 64 or >= 64. All live state crosses blocks through memory
    // (destAddr / a fresh slot), so no vreg needs to survive a branch.
    std::uint32_t srcSlot = fn_->addFrameSlot(16, 16, /*isSpill=*/false);
    VReg srcAddr = fn_->newVReg();
    emit({MOpcode::LeaSlot, {MOperand::defVReg(srcAddr), MOperand::slot(srcSlot)}});
    selI128(valueExpr, srcAddr);
    if (failed_) return;

    // The shift amount is a <=64-bit integer; mask to 0..127 (shifts by >=128 are
    // UB, we let them produce 0 / sign as the per-branch logic dictates).
    VReg amt = selExpr(amountExpr);
    if (failed_) return;
    // amount lives in memory too so it survives the branch.
    std::uint32_t amtSlot = fn_->addFrameSlot(8, 8, /*isSpill=*/false);
    { MInst st{MOpcode::Store, {MOperand::slot(amtSlot), MOperand::useVReg(amt)}};
      st.width = 8; st.isSigned = false; emit(st); }

    std::uint32_t smallB = fn_->addBlock();  // amount < 64
    std::uint32_t bigB = fn_->addBlock();    // amount >= 64
    std::uint32_t joinB = fn_->addBlock();

    // if (amount < 64) goto smallB else goto bigB
    VReg a0 = fn_->newVReg();
    { MInst ld{MOpcode::Load, {MOperand::defVReg(a0), MOperand::slot(amtSlot)}};
      ld.width = 8; ld.isSigned = false; emit(ld); }
    VReg c64 = fn_->newVReg();
    emit({MOpcode::MovRI, {MOperand::defVReg(c64), MOperand::immediate(64)}});
    emit({MOpcode::Cmp, {MOperand::useVReg(a0), MOperand::useVReg(c64)}});
    { MInst jcc{MOpcode::Jcc, {MOperand::lbl(bigB)}}; jcc.cond = Cond::UGE; emit(jcc); }
    emit({MOpcode::Jmp, {MOperand::lbl(smallB)}});

    auto loadAmt = [&]() {
        VReg n = fn_->newVReg();
        MInst ld{MOpcode::Load, {MOperand::defVReg(n), MOperand::slot(amtSlot)}};
        ld.width = 8; ld.isSigned = false; emit(ld);
        return n;
    };
    auto shiftReg = [&](VReg val, VReg cnt, bool isLeft, bool arith) {
        MInst sh{isLeft ? MOpcode::Shl : MOpcode::Shr,
                 {MOperand::useDefVReg(val), MOperand::useVReg(cnt)}};
        sh.isSigned = arith;
        sh.clobbers = {PhysReg::RCX};
        emit(sh);
    };
    auto comp64 = [&](VReg n) {  // 64 - n
        VReg inv = fn_->newVReg();
        emit({MOpcode::MovRI, {MOperand::defVReg(inv), MOperand::immediate(64)}});
        emit({MOpcode::Sub, {MOperand::useDefVReg(inv), MOperand::useVReg(n)}});
        return inv;
    };

    // ---- amount < 64 ----
    curBlock_ = smallB;
    {
        VReg lo = loadHalf(srcAddr, 0);
        VReg hi = loadHalf(srcAddr, 8);
        VReg n = loadAmt();
        if (left) {
            // hi = (hi << n) | (lo >> (64-n)); lo = lo << n
            VReg loForHi = fn_->newVReg();
            emit({MOpcode::MovRR, {MOperand::defVReg(loForHi), MOperand::useVReg(lo)}});
            VReg inv = comp64(n);
            shiftReg(loForHi, inv, /*left=*/false, /*arith=*/false);
            shiftReg(hi, n, /*left=*/true, false);
            emit({MOpcode::Or, {MOperand::useDefVReg(hi), MOperand::useVReg(loForHi)}});
            shiftReg(lo, n, /*left=*/true, false);
        } else {
            // lo = (lo >> n) | (hi << (64-n)); hi = hi >> n  (arith if signed)
            VReg hiForLo = fn_->newVReg();
            emit({MOpcode::MovRR, {MOperand::defVReg(hiForLo), MOperand::useVReg(hi)}});
            VReg inv = comp64(n);
            shiftReg(hiForLo, inv, /*left=*/true, false);
            shiftReg(lo, n, /*left=*/false, /*arith=*/false);
            emit({MOpcode::Or, {MOperand::useDefVReg(lo), MOperand::useVReg(hiForLo)}});
            shiftReg(hi, n, /*left=*/false, /*arith=*/isSigned);
        }
        storeHalf(destAddr, 0, lo);
        storeHalf(destAddr, 8, hi);
        emit({MOpcode::Jmp, {MOperand::lbl(joinB)}});
    }

    // ---- amount >= 64 ----
    curBlock_ = bigB;
    {
        VReg lo = loadHalf(srcAddr, 0);
        VReg hi = loadHalf(srcAddr, 8);
        VReg n = loadAmt();
        VReg m = fn_->newVReg();  // n - 64
        emit({MOpcode::MovRR, {MOperand::defVReg(m), MOperand::useVReg(n)}});
        VReg c64b = fn_->newVReg();
        emit({MOpcode::MovRI, {MOperand::defVReg(c64b), MOperand::immediate(64)}});
        emit({MOpcode::Sub, {MOperand::useDefVReg(m), MOperand::useVReg(c64b)}});
        if (left) {
            // hi = lo << (n-64); lo = 0
            shiftReg(lo, m, /*left=*/true, false);
            VReg zero = fn_->newVReg();
            emit({MOpcode::MovRI, {MOperand::defVReg(zero), MOperand::immediate(0)}});
            storeHalf(destAddr, 0, zero);
            storeHalf(destAddr, 8, lo);
        } else {
            // lo = hi >> (n-64); hi = signed ? (hi>>63) : 0
            VReg hiForLo = fn_->newVReg();
            emit({MOpcode::MovRR, {MOperand::defVReg(hiForLo), MOperand::useVReg(hi)}});
            shiftReg(hiForLo, m, /*left=*/false, /*arith=*/isSigned);
            storeHalf(destAddr, 0, hiForLo);
            VReg newHi = fn_->newVReg();
            if (isSigned) {
                emit({MOpcode::MovRR, {MOperand::defVReg(newHi), MOperand::useVReg(hi)}});
                VReg c63 = fn_->newVReg();
                emit({MOpcode::MovRI, {MOperand::defVReg(c63), MOperand::immediate(63)}});
                shiftReg(newHi, c63, /*left=*/false, /*arith=*/true);
            } else {
                emit({MOpcode::MovRI, {MOperand::defVReg(newHi), MOperand::immediate(0)}});
            }
            storeHalf(destAddr, 8, newHi);
        }
        emit({MOpcode::Jmp, {MOperand::lbl(joinB)}});
    }

    curBlock_ = joinB;
}

void InstructionSelector::selI128DivMod(const std::string& op, VReg aAddr, VReg bAddr,
                                        VReg destAddr, bool isSigned) {
    // Call the by-pointer runtime helper: helper(result*, a*, b*). The arg order
    // (result,a,b) maps onto intArgRegs[0..2], identical on SysV and Win64.
    std::string sym;
    if (op == "/") sym = isSigned ? "__ins_divti3" : "__ins_udivti3";
    else           sym = isSigned ? "__ins_modti3" : "__ins_umodti3";
    // The public helpers call shared internal workers via ordinary (non-runtime)
    // calls, which selCall does not auto-register. Request the whole helper set so
    // the module emitter's worklist pulls in every transitively-referenced body.
    requestedRuntime_.insert("__ins_u128_divmod");
    requestedRuntime_.insert("__ins_i128_neg");
    requestedRuntime_.insert("__ins_i128_copy");
    requestedRuntime_.insert("__ins_i128_neg_p");
    requestedRuntime_.insert("__ins_udivti3");
    requestedRuntime_.insert("__ins_umodti3");
    requestedRuntime_.insert("__ins_divti3");
    requestedRuntime_.insert("__ins_modti3");
    emitRuntimeCall(sym, {destAddr, aAddr, bAddr}, kInvalidVReg, /*returnsFloat=*/false);
}

void InstructionSelector::selF128Binary(const std::string& op, const AST::NodePtr& l,
                                        const AST::NodePtr& r, VReg destAddr) {
    std::string sym;
    if (op == "+") sym = "__addtf3";
    else if (op == "-") sym = "__subtf3";
    else if (op == "*") sym = "__multf3";
    else if (op == "/") sym = "__divtf3";
    else { fail("selector: unsupported f128 binary operator '" + op + "'"); return; }

    VReg aAddr = selExpr(l);
    VReg bAddr = selExpr(r);
    if (failed_) return;

    emitFW(MOpcode::FLoadInd,
           {MOperand::defPhysXmm(XmmReg::XMM0), MOperand::useVReg(aAddr),
            MOperand::immediate(0)}, 16);
    emitFW(MOpcode::FLoadInd,
           {MOperand::defPhysXmm(XmmReg::XMM1), MOperand::useVReg(bAddr),
            MOperand::immediate(0)}, 16);
    MInst callInst{MOpcode::Call, {MOperand::sym(sym)}};
    callInst.clobbers = callClobbers(abi_);
    emit(callInst);
    emitFW(MOpcode::FStoreInd,
           {MOperand::useVReg(destAddr), MOperand::immediate(0),
            MOperand::usePhysXmm(XmmReg::XMM0)}, 16);
}

VReg InstructionSelector::selCallReturningI128(const AST::NodePtr& expr) {
    // A call whose result type is i128. selCall already reassembles the RAX:RDX
    // return pair into a 16-byte temp and returns that temp's address.
    const auto& call = static_cast<const AST::FunctionCallExpr&>(*expr);
    return selCall(call);
}

VReg InstructionSelector::selFloatLiteral(const AST::FloatLiteral& lit) {
    // Load the IEEE-754 bit pattern into an XMM register via a GP imm. For f32 we
    // store the 32-bit single-precision pattern (zero-extended); for f64 the full
    // 64-bit double pattern. For f16 the value is kept in the register as f32
    // (the compute form), pre-rounded to half precision so it matches f16 storage.
    Types::TypeRef lt = concreteTypeOf(&lit);
    if (isFloat16(lt)) {
        float f = halfRoundF32(static_cast<float>(lit.value));
        std::uint32_t u;
        std::memcpy(&u, &f, sizeof(u));
        std::int64_t bits = static_cast<std::int64_t>(static_cast<std::uint64_t>(u));
        VReg v = fn_->newVReg(RegClass::XMM);
        emitFW(MOpcode::FConst, {MOperand::defVReg(v), MOperand::immediate(bits)}, 4);
        return v;
    }
    std::uint8_t fw = floatWidthOf(lt);
    std::int64_t bits;
    if (fw == 4) {
        float f = static_cast<float>(lit.value);
        std::uint32_t u;
        std::memcpy(&u, &f, sizeof(u));
        bits = static_cast<std::int64_t>(static_cast<std::uint64_t>(u));
    } else {
        double d = lit.value;
        std::memcpy(&bits, &d, sizeof(bits));
    }
    VReg v = fn_->newVReg(RegClass::XMM);
    emitFW(MOpcode::FConst, {MOperand::defVReg(v), MOperand::immediate(bits)}, fw);
    return v;
}

VReg InstructionSelector::emitStrlenOf(VReg ptr) {
    // i = 0; while (ptr[i] != 0) i++; return i.  Counter lives in a frame slot.
    std::uint32_t iSlot = fn_->addFrameSlot(8, 8, /*isSpill=*/false);
    VReg zero = fn_->newVReg();
    emit({MOpcode::MovRI, {MOperand::defVReg(zero), MOperand::immediate(0)}});
    {
        MInst st{MOpcode::Store, {MOperand::slot(iSlot), MOperand::useVReg(zero)}};
        st.width = 8;
        emit(st);
    }
    std::uint32_t headerB = fn_->addBlock();
    std::uint32_t bodyB = fn_->addBlock();
    std::uint32_t exitB = fn_->addBlock();
    emit({MOpcode::Jmp, {MOperand::lbl(headerB)}});

    curBlock_ = headerB;
    VReg iVal = fn_->newVReg();
    {
        MInst ld{MOpcode::Load, {MOperand::defVReg(iVal), MOperand::slot(iSlot)}};
        ld.width = 8; ld.isSigned = false;
        emit(ld);
    }
    VReg addr = fn_->newVReg();
    emit({MOpcode::LeaIndex,
          {MOperand::defVReg(addr), MOperand::useVReg(ptr), MOperand::useVReg(iVal),
           MOperand::immediate(0)}});
    fn_->block(curBlock_).insts.back().scale = 1;  // byte addressing
    VReg ch = fn_->newVReg();
    {
        MInst ld{MOpcode::LoadInd,
                 {MOperand::defVReg(ch), MOperand::useVReg(addr), MOperand::immediate(0)}};
        ld.width = 1; ld.isSigned = false;
        emit(ld);
    }
    VReg zc = fn_->newVReg();
    emit({MOpcode::MovRI, {MOperand::defVReg(zc), MOperand::immediate(0)}});
    emit({MOpcode::Cmp, {MOperand::useVReg(ch), MOperand::useVReg(zc)}});
    {
        MInst jcc{MOpcode::Jcc, {MOperand::lbl(exitB)}};
        jcc.cond = Cond::EQ;
        emit(jcc);
    }
    emit({MOpcode::Jmp, {MOperand::lbl(bodyB)}});

    curBlock_ = bodyB;
    VReg iVal2 = fn_->newVReg();
    {
        MInst ld{MOpcode::Load, {MOperand::defVReg(iVal2), MOperand::slot(iSlot)}};
        ld.width = 8; ld.isSigned = false;
        emit(ld);
    }
    VReg one = fn_->newVReg();
    emit({MOpcode::MovRI, {MOperand::defVReg(one), MOperand::immediate(1)}});
    emit({MOpcode::Add, {MOperand::useDefVReg(iVal2), MOperand::useVReg(one)}});
    {
        MInst st{MOpcode::Store, {MOperand::slot(iSlot), MOperand::useVReg(iVal2)}};
        st.width = 8;
        emit(st);
    }
    emit({MOpcode::Jmp, {MOperand::lbl(headerB)}});

    curBlock_ = exitB;
    VReg result = fn_->newVReg();
    {
        MInst ld{MOpcode::Load, {MOperand::defVReg(result), MOperand::slot(iSlot)}};
        ld.width = 8; ld.isSigned = false;
        emit(ld);
    }
    return result;
}

VReg InstructionSelector::selStringLiteral(const AST::StringLiteral& lit) {
    // Interpolated strings ($x / ${expr}) build a buffer via the __ins_fmt_*
    // formatters; plain literals intern their bytes into .rodata.
    if (lit.hasInterpolation) {
        return selInterpolation(lit);
    }
    // Intern the literal: pick a unique symbol name and hand the bytes to the
    // function's string pool. The lowering interns identical contents into a
    // single .rodata blob and defines this symbol at its offset. The value of a
    // `text` is the RIP-relative address of those bytes.
    std::string sym = ".Lstr." + std::to_string(stringCounter_++);
    fn_->addStringConstant(sym, lit.value);
    VReg v = fn_->newVReg();
    emit({MOpcode::Lea, {MOperand::defVReg(v), MOperand::sym(sym)}});
    return v;
}

// Capacity of the interpolation scratch buffer (matches the LLVM path's
// kInterpBufferSize). buf is `kInterpBufferSize + 1` bytes so __ins_fmt_finish
// can always write a terminating NUL at index `cap`.
static constexpr unsigned kInterpBufferSize = 1024;

VReg InstructionSelector::emitRuntimeCall(const std::string& symbol,
                                          const std::vector<VReg>& args,
                                          VReg floatArg, bool returnsFloat) {
    requestedRuntime_.insert(symbol);
    const auto& argRegs = abi_.intArgRegs;
    const auto& xmmArgRegs = abi_.xmmArgRegs;

    // Build the positional argument list. The optional f64 (when present) is the
    // LAST positional argument in every formatter (fmt_float(buf,cap,off,v)).
    struct RArg { VReg v; bool isFloat; };
    std::vector<RArg> a;
    a.reserve(args.size() + 1);
    for (VReg v : args) a.push_back({v, false});
    if (floatArg != kInvalidVReg) a.push_back({floatArg, true});

    // Classify each argument into a GP reg, XMM reg, or stack slot, mirroring
    // selCall's ABI rules (Win64 shares one positional cursor across the GP and
    // XMM files; System V advances them independently). Helpers can exceed 4 GP
    // args (fmt_int has 5), so stack passing is required.
    enum class Dest { GP, XMM, Stack };
    struct Place { Dest dest; std::size_t reg; std::size_t stackIndex; };
    std::vector<Place> places(a.size());
    unsigned gpCursor = 0, xmmCursor = 0, stackCursor = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].isFloat) {
            unsigned xidx = abi_.sharedArgRegIndex ? gpCursor : xmmCursor;
            if (xidx < xmmArgRegs.size()) {
                places[i] = {Dest::XMM, xidx, 0};
                if (abi_.sharedArgRegIndex) ++gpCursor; else ++xmmCursor;
            } else {
                places[i] = {Dest::Stack, 0, stackCursor++};
                if (abi_.sharedArgRegIndex) ++gpCursor;
            }
        } else {
            if (gpCursor < argRegs.size()) {
                places[i] = {Dest::GP, gpCursor, 0};
                ++gpCursor;
                if (abi_.sharedArgRegIndex) ++xmmCursor;
            } else {
                places[i] = {Dest::Stack, 0, stackCursor++};
            }
        }
    }

    // Reserve the outgoing-arg region (shadow space + any stack args).
    std::size_t stackArgs = stackCursor;
    if (stackArgs > 0) {
        std::int64_t bytes = static_cast<std::int64_t>(abi_.shadowSpace) +
                             8 * static_cast<std::int64_t>(stackArgs);
        fn_->noteOutgoingArgBytes(bytes);
    } else if (abi_.shadowSpace > 0) {
        fn_->noteOutgoingArgBytes(static_cast<std::int64_t>(abi_.shadowSpace));
    }

    // Store stack args first (free to use scratch), then load register args last.
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (places[i].dest != Dest::Stack) continue;
        std::int64_t off = static_cast<std::int64_t>(abi_.shadowSpace) +
                           8 * static_cast<std::int64_t>(places[i].stackIndex);
        if (a[i].isFloat) {
            emitFW(MOpcode::FStoreOutgoing,
                   {MOperand::immediate(off), MOperand::useVReg(a[i].v)}, 8);
        } else {
            emit({MOpcode::StoreOutgoing,
                  {MOperand::immediate(off), MOperand::useVReg(a[i].v)}});
        }
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (places[i].dest == Dest::GP) {
            emit({MOpcode::MovRR,
                  {MOperand::defPhys(argRegs[places[i].reg]), MOperand::useVReg(a[i].v)}});
        } else if (places[i].dest == Dest::XMM) {
            emitFW(MOpcode::FMovRR,
                   {MOperand::defPhysXmm(xmmArgRegs[places[i].reg]),
                    MOperand::useVReg(a[i].v)}, 8);
        }
    }

    MInst callInst{MOpcode::Call, {MOperand::sym(symbol)}};
    callInst.clobbers = callClobbers(abi_);
    emit(callInst);
    if (returnsFloat) {
        VReg res = fn_->newVReg(RegClass::XMM);
        emitFW(MOpcode::FMovRR,
               {MOperand::defVReg(res), MOperand::usePhysXmm(abi_.xmmReturnReg)}, 8);
        return res;
    }
    VReg res = fn_->newVReg();
    emit({MOpcode::MovRR, {MOperand::defVReg(res), MOperand::usePhys(abi_.intReturnReg)}});
    return res;
}

// --- Interpolation buffer helpers ------------------------------------------
// These operate on the buffer/cursor described by `bufSlot`/`offSlot`/`cap`,
// re-deriving the buffer address and cursor each time (cheap LeaSlot/Load,
// avoids long-lived vregs that would force spills across the helper calls).

void InstructionSelector::emitFormatRawLit(std::uint32_t bufSlot,
                                           std::uint32_t offSlot, unsigned cap,
                                           const std::string& s) {
    if (s.empty()) return;
    std::string sym = ".Lstr." + std::to_string(stringCounter_++);
    fn_->addStringConstant(sym, s);
    VReg buf = fn_->newVReg();
    emit({MOpcode::LeaSlot, {MOperand::defVReg(buf), MOperand::slot(bufSlot)}});
    VReg capV = fn_->newVReg();
    emit({MOpcode::MovRI,
          {MOperand::defVReg(capV), MOperand::immediate(static_cast<std::int64_t>(cap))}});
    VReg o = fn_->newVReg();
    { MInst ld{MOpcode::Load, {MOperand::defVReg(o), MOperand::slot(offSlot)}};
      ld.width = 8; ld.isSigned = false; emit(ld); }
    VReg ptr = fn_->newVReg();
    emit({MOpcode::Lea, {MOperand::defVReg(ptr), MOperand::sym(sym)}});
    VReg len = fn_->newVReg();
    emit({MOpcode::MovRI,
          {MOperand::defVReg(len),
           MOperand::immediate(static_cast<std::int64_t>(s.size()))}});
    VReg r = emitRuntimeCall("__ins_fmt_raw", {buf, capV, o, ptr, len},
                             kInvalidVReg, /*returnsFloat=*/false);
    { MInst st{MOpcode::Store, {MOperand::slot(offSlot), MOperand::useVReg(r)}};
      st.width = 8; st.isSigned = false; emit(st); }
}

void InstructionSelector::emitFormatScalar(std::uint32_t bufSlot,
                                           std::uint32_t offSlot, unsigned cap,
                                           VReg v, Types::TypeRef t) {
    auto bufAddr = [&]() -> VReg {
        VReg b = fn_->newVReg();
        emit({MOpcode::LeaSlot, {MOperand::defVReg(b), MOperand::slot(bufSlot)}});
        return b;
    };
    auto capVal = [&]() -> VReg {
        VReg c = fn_->newVReg();
        emit({MOpcode::MovRI,
              {MOperand::defVReg(c), MOperand::immediate(static_cast<std::int64_t>(cap))}});
        return c;
    };
    auto loadOff = [&]() -> VReg {
        VReg o = fn_->newVReg();
        MInst ld{MOpcode::Load, {MOperand::defVReg(o), MOperand::slot(offSlot)}};
        ld.width = 8; ld.isSigned = false; emit(ld);
        return o;
    };
    auto storeOff = [&](VReg o) {
        MInst st{MOpcode::Store, {MOperand::slot(offSlot), MOperand::useVReg(o)}};
        st.width = 8; st.isSigned = false; emit(st);
    };

    if (!t) return;
    if (isFloatType(t)) {
        if (floatWidthOf(t) != 8) {
            VReg w = fn_->newVReg(RegClass::XMM);
            MInst cvt{MOpcode::CvtF2F, {MOperand::defVReg(w), MOperand::useVReg(v)}};
            cvt.width = 8; emit(cvt);
            v = w;
        }
        VReg o = loadOff();
        VReg r = emitRuntimeCall("__ins_fmt_float", {bufAddr(), capVal(), o}, v,
                                 /*returnsFloat=*/false);
        storeOff(r);
        return;
    }
    if (t->kind == Types::Kind::Text) {
        VReg len = emitStrlenOf(v);
        VReg o = loadOff();
        VReg r = emitRuntimeCall("__ins_fmt_raw", {bufAddr(), capVal(), o, v, len},
                                 kInvalidVReg, /*returnsFloat=*/false);
        storeOff(r);
        return;
    }
    if (t->kind == Types::Kind::Bool) {
        std::string tsym = ".Lstr." + std::to_string(stringCounter_++);
        std::string fsym = ".Lstr." + std::to_string(stringCounter_++);
        fn_->addStringConstant(tsym, "true");
        fn_->addStringConstant(fsym, "false");
        std::uint32_t trueB = fn_->addBlock();
        std::uint32_t falseB = fn_->addBlock();
        std::uint32_t joinB = fn_->addBlock();
        std::uint32_t pSlot = fn_->addFrameSlot(8, 8, false);
        std::uint32_t lSlot = fn_->addFrameSlot(8, 8, false);
        VReg zero = fn_->newVReg();
        emit({MOpcode::MovRI, {MOperand::defVReg(zero), MOperand::immediate(0)}});
        emit({MOpcode::Cmp, {MOperand::useVReg(v), MOperand::useVReg(zero)}});
        { MInst jcc{MOpcode::Jcc, {MOperand::lbl(falseB)}}; jcc.cond = Cond::EQ; emit(jcc); }
        emit({MOpcode::Jmp, {MOperand::lbl(trueB)}});
        curBlock_ = trueB;
        {
            VReg p = fn_->newVReg();
            emit({MOpcode::Lea, {MOperand::defVReg(p), MOperand::sym(tsym)}});
            MInst st{MOpcode::Store, {MOperand::slot(pSlot), MOperand::useVReg(p)}};
            st.width = 8; st.isSigned = false; emit(st);
            VReg l = fn_->newVReg();
            emit({MOpcode::MovRI, {MOperand::defVReg(l), MOperand::immediate(4)}});
            MInst stl{MOpcode::Store, {MOperand::slot(lSlot), MOperand::useVReg(l)}};
            stl.width = 8; stl.isSigned = false; emit(stl);
            emit({MOpcode::Jmp, {MOperand::lbl(joinB)}});
        }
        curBlock_ = falseB;
        {
            VReg p = fn_->newVReg();
            emit({MOpcode::Lea, {MOperand::defVReg(p), MOperand::sym(fsym)}});
            MInst st{MOpcode::Store, {MOperand::slot(pSlot), MOperand::useVReg(p)}};
            st.width = 8; st.isSigned = false; emit(st);
            VReg l = fn_->newVReg();
            emit({MOpcode::MovRI, {MOperand::defVReg(l), MOperand::immediate(5)}});
            MInst stl{MOpcode::Store, {MOperand::slot(lSlot), MOperand::useVReg(l)}};
            stl.width = 8; stl.isSigned = false; emit(stl);
            emit({MOpcode::Jmp, {MOperand::lbl(joinB)}});
        }
        curBlock_ = joinB;
        VReg p = fn_->newVReg();
        { MInst ld{MOpcode::Load, {MOperand::defVReg(p), MOperand::slot(pSlot)}};
          ld.width = 8; ld.isSigned = false; emit(ld); }
        VReg l = fn_->newVReg();
        { MInst ld{MOpcode::Load, {MOperand::defVReg(l), MOperand::slot(lSlot)}};
          ld.width = 8; ld.isSigned = false; emit(ld); }
        VReg o = loadOff();
        VReg r = emitRuntimeCall("__ins_fmt_raw", {bufAddr(), capVal(), o, p, l},
                                 kInvalidVReg, /*returnsFloat=*/false);
        storeOff(r);
        return;
    }
    if (isIntegerLike(t)) {
        if (widthOf(t) == 1) {
            VReg o = loadOff();
            VReg addr = fn_->newVReg();
            emit({MOpcode::LeaIndex,
                  {MOperand::defVReg(addr), MOperand::useVReg(bufAddr()),
                   MOperand::useVReg(o), MOperand::immediate(0)}});
            fn_->block(curBlock_).insts.back().scale = 1;
            { MInst st{MOpcode::StoreInd,
                       {MOperand::useVReg(addr), MOperand::immediate(0),
                        MOperand::useVReg(v)}};
              st.width = 1; st.isSigned = false; emit(st); }
            VReg one = fn_->newVReg();
            emit({MOpcode::MovRI, {MOperand::defVReg(one), MOperand::immediate(1)}});
            emit({MOpcode::Add, {MOperand::useDefVReg(o), MOperand::useVReg(one)}});
            storeOff(o);
        } else {
            if (widthOf(t) != 8) {
                MInst ext{MOpcode::Ext, {MOperand::useDefVReg(v)}};
                ext.width = static_cast<std::uint8_t>(widthOf(t));
                ext.isSigned = isSignedOf(t);
                emit(ext);
            }
            VReg sgn = fn_->newVReg();
            emit({MOpcode::MovRI,
                  {MOperand::defVReg(sgn),
                   MOperand::immediate(isSignedOf(t) ? 1 : 0)}});
            VReg o = loadOff();
            VReg r = emitRuntimeCall("__ins_fmt_int",
                                     {bufAddr(), capVal(), o, v, sgn},
                                     kInvalidVReg, /*returnsFloat=*/false);
            storeOff(r);
        }
        return;
    }
    // Enums, pointers, and other kinds are intentionally not formatted (matches
    // the LLVM path, where they fall through emitFormatValue's default).
}

void InstructionSelector::emitFormatAggregate(std::uint32_t bufSlot,
                                              std::uint32_t offSlot, unsigned cap,
                                              VReg structAddr, Types::TypeRef type,
                                              unsigned depth) {
    if (!type) return;

    // Prefer a user-declared `toString` method (class only): call it with `this`
    // and append the returned text.
    if (const Sema::ClassInfo* ci = classInfoFor(type)) {
        auto it = ci->methodMangled.find("toString");
        if (it != ci->methodMangled.end()) {
            VReg str = emitDirectCall(it->second, {structAddr}, {false}, {0},
                                      /*resultTy=*/nullptr, kInvalidVReg, nullptr,
                                      kInvalidVReg);
            if (failed_) return;
            // Append the returned C-string (text) directly.
            VReg len = emitStrlenOf(str);
            VReg buf = fn_->newVReg();
            emit({MOpcode::LeaSlot, {MOperand::defVReg(buf), MOperand::slot(bufSlot)}});
            VReg capV = fn_->newVReg();
            emit({MOpcode::MovRI,
                  {MOperand::defVReg(capV), MOperand::immediate(static_cast<std::int64_t>(cap))}});
            VReg o = fn_->newVReg();
            { MInst ld{MOpcode::Load, {MOperand::defVReg(o), MOperand::slot(offSlot)}};
              ld.width = 8; ld.isSigned = false; emit(ld); }
            VReg r = emitRuntimeCall("__ins_fmt_raw", {buf, capV, o, str, len},
                                     kInvalidVReg, /*returnsFloat=*/false);
            { MInst st{MOpcode::Store, {MOperand::slot(offSlot), MOperand::useVReg(r)}};
              st.width = 8; st.isSigned = false; emit(st); }
            return;
        }
    }

    if (depth >= 8) {
        emitFormatRawLit(bufSlot, offSlot, cap, type->name + " { .. }");
        return;
    }

    const Sema::StructInfo* si = structInfoFor(type);
    if (!si || si->fields.empty()) {
        emitFormatRawLit(bufSlot, offSlot, cap, type->name + " {}");
        return;
    }

    emitFormatRawLit(bufSlot, offSlot, cap, type->name + " { ");
    for (std::size_t i = 0; i < si->fields.size(); ++i) {
        const auto& f = si->fields[i];
        std::string prefix = (i == 0 ? std::string("") : ", ") + f.first + ": ";
        emitFormatRawLit(bufSlot, offSlot, cap, prefix);
        if (failed_) return;

        std::int64_t foff = 0; Types::TypeRef fty = nullptr;
        if (!fieldOffsetOf(type, f.first, foff, fty)) {
            fail("selector: unknown field in struct interpolation");
            return;
        }
        // Field address = structAddr + foff.
        VReg fieldAddr = fn_->newVReg();
        emit({MOpcode::MovRR, {MOperand::defVReg(fieldAddr), MOperand::useVReg(structAddr)}});
        if (foff != 0) {
            VReg d = fn_->newVReg();
            emit({MOpcode::MovRI, {MOperand::defVReg(d), MOperand::immediate(foff)}});
            emit({MOpcode::Add, {MOperand::useDefVReg(fieldAddr), MOperand::useVReg(d)}});
        }
        if (fty && (fty->kind == Types::Kind::Struct ||
                    fty->kind == Types::Kind::Class)) {
            emitFormatAggregate(bufSlot, offSlot, cap, fieldAddr, fty, depth + 1);
        } else {
            // Load the scalar field through its address and format it.
            VReg fv = fn_->newVReg(isFloatType(fty) ? RegClass::XMM : RegClass::GPR);
            if (isFloatType(fty)) {
                MInst ld{MOpcode::FLoadInd,
                         {MOperand::defVReg(fv), MOperand::useVReg(fieldAddr),
                          MOperand::immediate(0)}};
                ld.width = static_cast<std::uint8_t>(floatWidthOf(fty));
                emit(ld);
            } else {
                MInst ld{MOpcode::LoadInd,
                         {MOperand::defVReg(fv), MOperand::useVReg(fieldAddr),
                          MOperand::immediate(0)}};
                ld.width = static_cast<std::uint8_t>(widthOf(fty));
                ld.isSigned = isSignedOf(fty);
                emit(ld);
            }
            emitFormatScalar(bufSlot, offSlot, cap, fv, fty);
        }
        if (failed_) return;
    }
    emitFormatRawLit(bufSlot, offSlot, cap, " }");
}

VReg InstructionSelector::selInterpolation(const AST::StringLiteral& lit) {
    // Allocate a fixed scratch buffer on the stack, append each literal segment
    // and each formatted value
    // via the __ins_fmt_* helpers (tracking a running `off` cursor), then
    // NUL-terminate via __ins_fmt_finish. The interpolation's value is the buffer
    // address (a `text`).
    const unsigned cap = kInterpBufferSize;
    std::uint32_t bufSlot =
        fn_->addFrameSlot(cap + 1, 16, /*isSpill=*/false);
    VReg buf = fn_->newVReg();
    emit({MOpcode::LeaSlot, {MOperand::defVReg(buf), MOperand::slot(bufSlot)}});

    VReg capV = fn_->newVReg();
    emit({MOpcode::MovRI,
          {MOperand::defVReg(capV), MOperand::immediate(static_cast<std::int64_t>(cap))}});

    // `off` cursor in a frame slot so it survives across the helper calls.
    std::uint32_t offSlot = fn_->addFrameSlot(8, 8, /*isSpill=*/false);
    {
        VReg z = fn_->newVReg();
        emit({MOpcode::MovRI, {MOperand::defVReg(z), MOperand::immediate(0)}});
        MInst st{MOpcode::Store, {MOperand::slot(offSlot), MOperand::useVReg(z)}};
        st.width = 8; st.isSigned = false;
        emit(st);
    }
    auto loadOff = [&]() -> VReg {
        VReg o = fn_->newVReg();
        MInst ld{MOpcode::Load, {MOperand::defVReg(o), MOperand::slot(offSlot)}};
        ld.width = 8; ld.isSigned = false;
        emit(ld);
        return o;
    };
    auto storeOff = [&](VReg o) {
        MInst st{MOpcode::Store, {MOperand::slot(offSlot), MOperand::useVReg(o)}};
        st.width = 8; st.isSigned = false;
        emit(st);
    };
    // Re-derive the buffer address each time it is needed (a single vreg held
    // live across many calls would force a spill; an LeaSlot is cheap).
    auto bufAddr = [&]() -> VReg {
        VReg b = fn_->newVReg();
        emit({MOpcode::LeaSlot, {MOperand::defVReg(b), MOperand::slot(bufSlot)}});
        return b;
    };
    auto capVal = [&]() -> VReg {
        VReg c = fn_->newVReg();
        emit({MOpcode::MovRI,
              {MOperand::defVReg(c), MOperand::immediate(static_cast<std::int64_t>(cap))}});
        return c;
    };

    // append raw bytes from a pointer of known/runtime length:
    //   off = __ins_fmt_raw(buf, cap, off, src, len)
    auto appendRaw = [&](VReg src, VReg len) {
        VReg o = loadOff();
        VReg r = emitRuntimeCall("__ins_fmt_raw",
                                 {bufAddr(), capVal(), o, src, len},
                                 kInvalidVReg, /*returnsFloat=*/false);
        storeOff(r);
    };

    const auto& lits = lit.literalParts;
    const auto& exprs = lit.exprParts;
    for (std::size_t i = 0; i < lits.size(); ++i) {
        // --- literal segment ---
        const std::string& seg = lits[i];
        if (!seg.empty()) {
            std::string sym = ".Lstr." + std::to_string(stringCounter_++);
            fn_->addStringConstant(sym, seg);
            VReg ptr = fn_->newVReg();
            emit({MOpcode::Lea, {MOperand::defVReg(ptr), MOperand::sym(sym)}});
            VReg len = fn_->newVReg();
            emit({MOpcode::MovRI,
                  {MOperand::defVReg(len),
                   MOperand::immediate(static_cast<std::int64_t>(seg.size()))}});
            appendRaw(ptr, len);
        }
        // --- interpolated value ---
        if (i < exprs.size() && exprs[i]) {
            Types::TypeRef t = concreteTypeOf(exprs[i].get());
            if (!t) { fail("selector: interpolation value has no type"); return kInvalidVReg; }
            if (isFloatType(t)) {
                // off = __ins_fmt_float(buf, cap, off, (f64)v)
                VReg v = selExpr(exprs[i]);
                if (failed_) return kInvalidVReg;
                // Widen f32 -> f64 if needed (helper takes f64).
                if (floatWidthOf(t) != 8) {
                    VReg w = fn_->newVReg(RegClass::XMM);
                    MInst cvt{MOpcode::CvtF2F, {MOperand::defVReg(w), MOperand::useVReg(v)}};
                    cvt.width = 8;  // dest precision
                    emit(cvt);
                    v = w;
                }
                VReg o = loadOff();
                VReg r = emitRuntimeCall("__ins_fmt_float",
                                         {bufAddr(), capVal(), o}, v,
                                         /*returnsFloat=*/false);
                storeOff(r);
            } else if (t->kind == Types::Kind::Text) {
                // off = __ins_fmt_raw(buf, cap, off, v, strlen(v))
                VReg v = selExpr(exprs[i]);
                if (failed_) return kInvalidVReg;
                VReg len = emitStrlenOf(v);
                appendRaw(v, len);
            } else if (t->kind == Types::Kind::Bool) {
                // append "true"/"false" by branching on the bool value.
                VReg v = selExpr(exprs[i]);
                if (failed_) return kInvalidVReg;
                std::string tsym = ".Lstr." + std::to_string(stringCounter_++);
                std::string fsym = ".Lstr." + std::to_string(stringCounter_++);
                fn_->addStringConstant(tsym, "true");
                fn_->addStringConstant(fsym, "false");
                // ptr = v ? &"true" : &"false"; len = v ? 4 : 5
                std::uint32_t trueB = fn_->addBlock();
                std::uint32_t falseB = fn_->addBlock();
                std::uint32_t joinB = fn_->addBlock();
                std::uint32_t pSlot = fn_->addFrameSlot(8, 8, false);
                std::uint32_t lSlot = fn_->addFrameSlot(8, 8, false);
                VReg zero = fn_->newVReg();
                emit({MOpcode::MovRI, {MOperand::defVReg(zero), MOperand::immediate(0)}});
                emit({MOpcode::Cmp, {MOperand::useVReg(v), MOperand::useVReg(zero)}});
                { MInst jcc{MOpcode::Jcc, {MOperand::lbl(falseB)}}; jcc.cond = Cond::EQ; emit(jcc); }
                emit({MOpcode::Jmp, {MOperand::lbl(trueB)}});
                curBlock_ = trueB;
                {
                    VReg p = fn_->newVReg();
                    emit({MOpcode::Lea, {MOperand::defVReg(p), MOperand::sym(tsym)}});
                    MInst st{MOpcode::Store, {MOperand::slot(pSlot), MOperand::useVReg(p)}};
                    st.width = 8; st.isSigned = false; emit(st);
                    VReg l = fn_->newVReg();
                    emit({MOpcode::MovRI, {MOperand::defVReg(l), MOperand::immediate(4)}});
                    MInst stl{MOpcode::Store, {MOperand::slot(lSlot), MOperand::useVReg(l)}};
                    stl.width = 8; stl.isSigned = false; emit(stl);
                    emit({MOpcode::Jmp, {MOperand::lbl(joinB)}});
                }
                curBlock_ = falseB;
                {
                    VReg p = fn_->newVReg();
                    emit({MOpcode::Lea, {MOperand::defVReg(p), MOperand::sym(fsym)}});
                    MInst st{MOpcode::Store, {MOperand::slot(pSlot), MOperand::useVReg(p)}};
                    st.width = 8; st.isSigned = false; emit(st);
                    VReg l = fn_->newVReg();
                    emit({MOpcode::MovRI, {MOperand::defVReg(l), MOperand::immediate(5)}});
                    MInst stl{MOpcode::Store, {MOperand::slot(lSlot), MOperand::useVReg(l)}};
                    stl.width = 8; stl.isSigned = false; emit(stl);
                    emit({MOpcode::Jmp, {MOperand::lbl(joinB)}});
                }
                curBlock_ = joinB;
                VReg p = fn_->newVReg();
                { MInst ld{MOpcode::Load, {MOperand::defVReg(p), MOperand::slot(pSlot)}};
                  ld.width = 8; ld.isSigned = false; emit(ld); }
                VReg l = fn_->newVReg();
                { MInst ld{MOpcode::Load, {MOperand::defVReg(l), MOperand::slot(lSlot)}};
                  ld.width = 8; ld.isSigned = false; emit(ld); }
                appendRaw(p, l);
            } else if (isIntegerLike(t)) {
                VReg v = selExpr(exprs[i]);
                if (failed_) return kInvalidVReg;
                // char / u8 (1-byte int): store the raw byte directly (matches
                // the LLVM path's special-case) -- no decimal formatting.
                if (widthOf(t) == 1) {
                    // if (off < cap) { buf[off] = v; off++ }
                    VReg o = loadOff();
                    VReg addr = fn_->newVReg();
                    emit({MOpcode::LeaIndex,
                          {MOperand::defVReg(addr), MOperand::useVReg(bufAddr()),
                           MOperand::useVReg(o), MOperand::immediate(0)}});
                    fn_->block(curBlock_).insts.back().scale = 1;
                    { MInst st{MOpcode::StoreInd,
                               {MOperand::useVReg(addr), MOperand::immediate(0),
                                MOperand::useVReg(v)}};
                      st.width = 1; st.isSigned = false; emit(st); }
                    VReg one = fn_->newVReg();
                    emit({MOpcode::MovRI, {MOperand::defVReg(one), MOperand::immediate(1)}});
                    emit({MOpcode::Add, {MOperand::useDefVReg(o), MOperand::useVReg(one)}});
                    storeOff(o);
                } else {
                    // off = __ins_fmt_int(buf, cap, off, (i64)v, is_signed)
                    // Sign/zero-extend the value to 64 bits per its signedness.
                    if (widthOf(t) != 8) {
                        MInst ext{MOpcode::Ext, {MOperand::useDefVReg(v)}};
                        ext.width = static_cast<std::uint8_t>(widthOf(t));
                        ext.isSigned = isSignedOf(t);
                        emit(ext);
                    }
                    VReg sgn = fn_->newVReg();
                    emit({MOpcode::MovRI,
                          {MOperand::defVReg(sgn),
                           MOperand::immediate(isSignedOf(t) ? 1 : 0)}});
                    VReg o = loadOff();
                    VReg r = emitRuntimeCall("__ins_fmt_int",
                                             {bufAddr(), capVal(), o, v, sgn},
                                             kInvalidVReg, /*returnsFloat=*/false);
                    storeOff(r);
                }
            } else if (t->kind == Types::Kind::Struct ||
                       t->kind == Types::Kind::Class) {
                // Aggregate: format `Name { field: value, ... }` (or via the
                // class's toString method). Needs the value's storage address.
                VReg structAddr;
                if (t->kind == Types::Kind::Pointer) {
                    structAddr = selExpr(exprs[i]);
                } else {
                    ElemAddr a = computeLValueAddr(exprs[i].get());
                    if (failed_) return kInvalidVReg;
                    structAddr = materializeAddr(a);
                }
                if (failed_) return kInvalidVReg;
                emitFormatAggregate(bufSlot, offSlot, cap, structAddr, t, 0);
                if (failed_) return kInvalidVReg;
            } else {
                fail("selector: interpolation of this value type is not supported "
                     "by the custom backend");
                return kInvalidVReg;
            }
        }
    }

    // __ins_fmt_finish(buf, cap, off) -- NUL-terminate (void).
    {
        VReg o = loadOff();
        emitRuntimeCall("__ins_fmt_finish", {bufAddr(), capVal(), o},
                        kInvalidVReg, /*returnsFloat=*/false);
    }
    // The interpolation value is the buffer address.
    return bufAddr();
}

VReg InstructionSelector::selBuiltinCall(const AST::BuiltinCallExpr& call) {
    const std::string& name = call.name;


    // @utf16("literal"): intern a wide (UTF-16LE) string constant and yield a
    // pointer to its first u16 unit. Sema guarantees the argument is a plain
    // (non-interpolated) string literal and the result type is u16*. Each input
    // byte widens to one u16 unit (BMP-only, matching the LLVM path); a u16 NUL
    // terminates the blob. The blob is 2-byte aligned and carries its own
    // terminator (no implicit single NUL).
    if (name == "utf16") {
        auto lit = call.arguments.empty()
                       ? nullptr
                       : AST::ast_cast<AST::StringLiteral>(call.arguments[0]);
        if (!lit) {
            fail("selector: @utf16 requires a string literal argument");
            return kInvalidVReg;
        }
        std::string wide;
        wide.reserve((lit->value.size() + 1) * 2);
        for (unsigned char c : lit->value) {
            wide.push_back(static_cast<char>(c));  // low byte
            wide.push_back(0);                       // high byte (LE)
        }
        wide.push_back(0);  // u16 NUL terminator (two zero bytes)
        wide.push_back(0);
        std::string sym = ".Lwstr." + std::to_string(stringCounter_++);
        fn_->addRawConstant(sym, std::move(wide), /*align=*/2);
        VReg v = fn_->newVReg();
        emit({MOpcode::Lea, {MOperand::defVReg(v), MOperand::sym(sym)}});
        return v;
    }

    // @hash(s): 64-bit hash of a NUL-terminated string. A plain string-literal
    // argument is folded at compile time (identical to __ins_hash_bytes);
    // otherwise the runtime __ins_hash is called on the pointer.
    if (name == "hash") {
        if (call.arguments.empty()) {
            VReg z = fn_->newVReg();
            emit({MOpcode::MovRI, {MOperand::defVReg(z), MOperand::immediate(0)}});
            return z;
        }
        auto lit = AST::ast_cast<AST::StringLiteral>(call.arguments[0]);
        if (lit && !lit->hasInterpolation) {
            // Fold at compile time with the SAME algorithm the target's runtime
            // uses, so a folded literal hash equals the runtime hash bit-for-bit.
            std::uint64_t h = aesHash_ ? Hashing::aesStringHash(lit->value)
                                       : Hashing::portableStringHash(lit->value);
            VReg v = fn_->newVReg();
            emit({MOpcode::MovRI,
                  {MOperand::defVReg(v),
                   MOperand::immediate(static_cast<std::int64_t>(h))}});
            return v;
        }
        // Runtime: length via strlen, then the target's hash_bytes helper.
        VReg ptr = selExpr(call.arguments[0]);
        if (failed_) return kInvalidVReg;
        VReg len = emitStrlenOf(ptr);
        if (failed_) return kInvalidVReg;
        const char* sym = aesHash_ ? "__ins_hash_bytes_aes" : "__ins_hash_bytes";
        return emitRuntimeCall(sym, {ptr, len}, kInvalidVReg, false);
    }

    // @memcpy(dst, src, n): copy n bytes dst<-src. Inlined as a byte loop
    //   i = 0; while (i < n) { dst[i] = src[i]; i++ }
    // matching __ins_memcpy (runtime_core.cpp) / std::mem.copy. Returns void.
    if (name == "memcpy") {
        if (call.arguments.size() < 3) {
            fail("selector: @memcpy requires (dst, src, n)");
            return kInvalidVReg;
        }
        VReg dst = selExpr(call.arguments[0]);
        if (failed_) return kInvalidVReg;
        VReg src = selExpr(call.arguments[1]);
        if (failed_) return kInvalidVReg;
        VReg n = selExpr(call.arguments[2]);
        if (failed_) return kInvalidVReg;
        emitByteLoop(dst, src, kInvalidVReg, n);
        return kInvalidVReg;
    }
    // @memset(ptr, value, n): set n bytes at ptr to the low byte of value. Inlined
    //   i = 0; while (i < n) { ptr[i] = value; i++ }
    // matching __ins_memset / std::mem.set. Returns void.
    if (name == "memset") {
        if (call.arguments.size() < 3) {
            fail("selector: @memset requires (ptr, value, n)");
            return kInvalidVReg;
        }
        VReg ptr = selExpr(call.arguments[0]);
        if (failed_) return kInvalidVReg;
        VReg val = selExpr(call.arguments[1]);
        if (failed_) return kInvalidVReg;
        VReg n = selExpr(call.arguments[2]);
        if (failed_) return kInvalidVReg;
        emitByteLoop(ptr, kInvalidVReg, val, n);
        return kInvalidVReg;
    }

    // @malloc(size, [align]): allocate `size` bytes. On Win64 this is
    // HeapAlloc(GetProcessHeap(), 0, size); on Linux an anonymous mmap. Returns a
    // pointer (0 on failure). align > 16 is handled by overallocating and storing
    // the raw allocation metadata immediately before the aligned user pointer.
    if (name == "malloc") {
        if (call.arguments.empty()) {
            fail("selector: @malloc requires a size");
            return kInvalidVReg;
        }
        VReg size = selExpr(call.arguments[0]);
        if (failed_) return kInvalidVReg;
        if (call.arguments.size() >= 2) {
            VReg align = selExpr(call.arguments[1]);
            if (failed_) return kInvalidVReg;
            return emitAlignedMalloc(size, align);
        }
        return emitMalloc(size);
    }

    // @realloc(ptr, new_size, [align]) legacy form: allocate fresh and copy
    // new_size bytes, but cannot safely release mmap-backed memory because the
    // old allocation size is unknown.
    // @realloc(ptr, old_size, new_size, align) real-free form: copy
    // min(old_size, new_size), release the old block, and return the fresh block.
    if (name == "realloc") {
        if (call.arguments.size() < 2) {
            fail("selector: @realloc requires (ptr, new_size)");
            return kInvalidVReg;
        }
        VReg oldPtr = selExpr(call.arguments[0]);
        if (failed_) return kInvalidVReg;
        const bool hasOldSize = call.arguments.size() >= 4;
        VReg oldSize = kInvalidVReg;
        VReg newSize = kInvalidVReg;
        VReg align = kInvalidVReg;
        if (hasOldSize) {
            oldSize = selExpr(call.arguments[1]);
            if (failed_) return kInvalidVReg;
            newSize = selExpr(call.arguments[2]);
            if (failed_) return kInvalidVReg;
            align = selExpr(call.arguments[3]);
        } else {
            newSize = selExpr(call.arguments[1]);
            if (failed_) return kInvalidVReg;
            if (call.arguments.size() >= 3) {
                align = selExpr(call.arguments[2]);
            }
        }
        if (failed_) return kInvalidVReg;
        VReg fresh = (align != kInvalidVReg) ? emitAlignedMalloc(newSize, align)
                                             : emitMalloc(newSize);
        if (failed_) return kInvalidVReg;
        std::uint32_t doneB = fn_->addBlock();
        std::uint32_t checkFreshB = fn_->addBlock();
        std::uint32_t chooseCopySizeB = fn_->addBlock();
        std::uint32_t useOldSizeB = fn_->addBlock();
        std::uint32_t copyB = fn_->addBlock();
        VReg zero = fn_->newVReg();
        emit({MOpcode::MovRI, {MOperand::defVReg(zero), MOperand::immediate(0)}});
        emit({MOpcode::Cmp, {MOperand::useVReg(oldPtr), MOperand::useVReg(zero)}});
        {
            MInst jcc{MOpcode::Jcc, {MOperand::lbl(doneB)}};
            jcc.cond = Cond::EQ;  // oldPtr == 0 -> skip the copy
            emit(jcc);
        }
        emit({MOpcode::Jmp, {MOperand::lbl(checkFreshB)}});

        curBlock_ = checkFreshB;
        emit({MOpcode::Cmp, {MOperand::useVReg(fresh), MOperand::useVReg(zero)}});
        {
            MInst jcc{MOpcode::Jcc, {MOperand::lbl(doneB)}};
            jcc.cond = Cond::EQ;  // allocation failed -> return null, keep old block
            emit(jcc);
        }
        emit({MOpcode::Jmp,
              {MOperand::lbl(hasOldSize ? chooseCopySizeB : copyB)}});

        std::uint32_t copySizeSlot = fn_->addFrameSlot(8, 8, false);
        auto storeCopySize = [&](VReg v) {
            MInst st{MOpcode::Store, {MOperand::slot(copySizeSlot), MOperand::useVReg(v)}};
            st.width = 8;
            st.isSigned = false;
            emit(st);
        };
        auto loadCopySize = [&]() {
            VReg r = fn_->newVReg();
            MInst ld{MOpcode::Load, {MOperand::defVReg(r), MOperand::slot(copySizeSlot)}};
            ld.width = 8;
            ld.isSigned = false;
            emit(ld);
            return r;
        };

        curBlock_ = chooseCopySizeB;
        storeCopySize(newSize);
        if (hasOldSize) {
            emit({MOpcode::Cmp, {MOperand::useVReg(oldSize), MOperand::useVReg(newSize)}});
            {
                MInst jcc{MOpcode::Jcc, {MOperand::lbl(copyB)}};
                jcc.cond = Cond::GE;  // oldSize >= newSize -> copy newSize
                emit(jcc);
            }
            emit({MOpcode::Jmp, {MOperand::lbl(useOldSizeB)}});

            curBlock_ = useOldSizeB;
            storeCopySize(oldSize);
            emit({MOpcode::Jmp, {MOperand::lbl(copyB)}});
        }

        curBlock_ = copyB;
        VReg copySize = hasOldSize ? loadCopySize() : newSize;
        emitByteLoop(fresh, oldPtr, kInvalidVReg, copySize);
        if (hasOldSize) {
            emitMaybeAlignedFree(oldPtr, oldSize, align);
        }
        emit({MOpcode::Jmp, {MOperand::lbl(doneB)}});
        curBlock_ = doneB;
        return fresh;
    }

    // @free(ptr, size, ...): release memory allocated by @malloc/new when the
    // caller provides the allocation size. With no size, evaluate for side
    // effects and keep the old no-op behavior.
    if (name == "free") {
        if (call.arguments.empty()) return kInvalidVReg;
        VReg ptr = selExpr(call.arguments[0]);
        if (failed_) return kInvalidVReg;
        if (call.arguments.size() >= 2) {
            VReg size = selExpr(call.arguments[1]);
            if (failed_) return kInvalidVReg;
            VReg align = kInvalidVReg;
            if (call.arguments.size() >= 3) {
                align = selExpr(call.arguments[2]);
                if (failed_) return kInvalidVReg;
            }
            emitMaybeAlignedFree(ptr, size, align);
        }
        return kInvalidVReg;
    }

    // @panic(...): terminate the process. On Linux/SysV emit exit(60) via syscall;
    // on Win64 call ExitProcess(1) through the PE import table. Mirrors the LLVM
    // path's hard exit; the optional message argument is evaluated for effect.
    if (name == "panic") {
        if (!call.arguments.empty()) {
            selExpr(call.arguments[0]);
            if (failed_) return kInvalidVReg;
        }
        if (wasmTarget_) {
            // wasm's `unreachable` is exactly this: an immediate, unrecoverable
            // trap. Reuse the ud2 encoding, which the wasm emitter maps onto it.
            emit({MOpcode::AsmFixed, {MOperand::immediate(4)}});
        } else if (abi_.abi == Abi::Win64) {
            // ExitProcess(1): exit code in RCX (first Win64 int arg).
            VReg code = fn_->newVReg();
            emit({MOpcode::MovRI, {MOperand::defVReg(code), MOperand::immediate(1)}});
            emit({MOpcode::MovRR,
                  {MOperand::defPhys(abi_.intArgRegs[0]), MOperand::useVReg(code)}});
            MInst c{MOpcode::CallImport,
                    {MOperand::sym("ExitProcess"), MOperand::sym("kernel32.dll")}};
            c.clobbers = callClobbers(abi_);
            emit(c);
        } else {
            // Linux: exit(60), exit code 1 in RDI, syscall number 60 in RAX.
            VReg num = fn_->newVReg();
            emit({MOpcode::MovRI, {MOperand::defVReg(num), MOperand::immediate(60)}});
            emit({MOpcode::MovRR, {MOperand::defPhys(PhysReg::RAX), MOperand::useVReg(num)}});
            VReg code = fn_->newVReg();
            emit({MOpcode::MovRI, {MOperand::defVReg(code), MOperand::immediate(1)}});
            emit({MOpcode::MovRR, {MOperand::defPhys(PhysReg::RDI), MOperand::useVReg(code)}});
            MInst sys{MOpcode::Syscall, {}};
            sys.clobbers = abi_.callerSaved;
            emit(sys);
        }
        return kInvalidVReg;
    }

    fail("selector: builtin '@" + name + "' is not supported by this backend");
    return kInvalidVReg;
}

void InstructionSelector::emitByteLoop(VReg destPtr, VReg srcPtr, VReg valByte,
                                       VReg count) {
    // Counter slot so `i` survives across the loop blocks (no SSA phi here).
    std::uint32_t iSlot = fn_->addFrameSlot(8, 8, /*isSpill=*/false);
    VReg zero = fn_->newVReg();
    emit({MOpcode::MovRI, {MOperand::defVReg(zero), MOperand::immediate(0)}});
    {
        MInst st{MOpcode::Store, {MOperand::slot(iSlot), MOperand::useVReg(zero)}};
        st.width = 8;
        emit(st);
    }
    std::uint32_t headerB = fn_->addBlock();
    std::uint32_t bodyB = fn_->addBlock();
    std::uint32_t exitB = fn_->addBlock();
    emit({MOpcode::Jmp, {MOperand::lbl(headerB)}});

    // header: if (i >= count) goto exit; else goto body
    curBlock_ = headerB;
    VReg iVal = fn_->newVReg();
    {
        MInst ld{MOpcode::Load, {MOperand::defVReg(iVal), MOperand::slot(iSlot)}};
        ld.width = 8; ld.isSigned = false;
        emit(ld);
    }
    emit({MOpcode::Cmp, {MOperand::useVReg(iVal), MOperand::useVReg(count)}});
    {
        MInst jcc{MOpcode::Jcc, {MOperand::lbl(exitB)}};
        jcc.cond = Cond::UGE;  // unsigned i >= count (count is u64)
        emit(jcc);
    }
    emit({MOpcode::Jmp, {MOperand::lbl(bodyB)}});

    // body: dst[i] = (src ? src[i] : valByte); i = i + 1
    curBlock_ = bodyB;
    VReg iCur = fn_->newVReg();
    {
        MInst ld{MOpcode::Load, {MOperand::defVReg(iCur), MOperand::slot(iSlot)}};
        ld.width = 8; ld.isSigned = false;
        emit(ld);
    }
    // The byte to store: copied from src[i] (memcpy) or the value byte (memset).
    VReg byteVal;
    if (srcPtr != kInvalidVReg) {
        VReg srcAddr = fn_->newVReg();
        emit({MOpcode::LeaIndex,
              {MOperand::defVReg(srcAddr), MOperand::useVReg(srcPtr),
               MOperand::useVReg(iCur), MOperand::immediate(0)}});
        fn_->block(curBlock_).insts.back().scale = 1;  // byte addressing
        byteVal = fn_->newVReg();
        MInst ld{MOpcode::LoadInd,
                 {MOperand::defVReg(byteVal), MOperand::useVReg(srcAddr),
                  MOperand::immediate(0)}};
        ld.width = 1; ld.isSigned = false;
        emit(ld);
    } else {
        byteVal = valByte;
    }
    VReg dstAddr = fn_->newVReg();
    emit({MOpcode::LeaIndex,
          {MOperand::defVReg(dstAddr), MOperand::useVReg(destPtr),
           MOperand::useVReg(iCur), MOperand::immediate(0)}});
    fn_->block(curBlock_).insts.back().scale = 1;  // byte addressing
    {
        MInst st{MOpcode::StoreInd,
                 {MOperand::useVReg(dstAddr), MOperand::immediate(0),
                  MOperand::useVReg(byteVal)}};
        st.width = 1; st.isSigned = false;
        emit(st);
    }
    // i = i + 1
    VReg one = fn_->newVReg();
    emit({MOpcode::MovRI, {MOperand::defVReg(one), MOperand::immediate(1)}});
    emit({MOpcode::Add, {MOperand::useDefVReg(iCur), MOperand::useVReg(one)}});
    {
        MInst st{MOpcode::Store, {MOperand::slot(iSlot), MOperand::useVReg(iCur)}};
        st.width = 8;
        emit(st);
    }
    emit({MOpcode::Jmp, {MOperand::lbl(headerB)}});

    curBlock_ = exitB;
}

VReg InstructionSelector::emitSimpleImportCall(const std::string& fn,
                                               const std::string& dll,
                                               const std::vector<VReg>& args) {
    // Place up to 4 GP args in the ABI integer arg registers (eval is already
    // done; just move into place), reserve shadow space, and call through the IAT.
    const auto& argRegs = abi_.intArgRegs;
    for (std::size_t i = 0; i < args.size() && i < argRegs.size(); ++i) {
        emit({MOpcode::MovRR,
              {MOperand::defPhys(argRegs[i]), MOperand::useVReg(args[i])}});
    }
    if (abi_.shadowSpace > 0) {
        fn_->noteOutgoingArgBytes(static_cast<std::int64_t>(abi_.shadowSpace));
    }
    MInst c{MOpcode::CallImport, {MOperand::sym(fn), MOperand::sym(dll)}};
    c.clobbers = callClobbers(abi_);
    emit(c);
    VReg res = fn_->newVReg();
    emit({MOpcode::MovRR, {MOperand::defVReg(res), MOperand::usePhys(abi_.intReturnReg)}});
    return res;
}

VReg InstructionSelector::emitMalloc(VReg size) {
    if (wasmTarget_) {
        // wasm has no syscalls and no OS heap. Call the runtime allocator the
        // wasm backend synthesizes, which bump-allocates linear memory.
        // Alignment matches what mmap / HeapAlloc guarantee, so the
        // over-alignment path in emitAlignedMalloc behaves identically.
        VReg align = fn_->newVReg();
        emit({MOpcode::MovRI, {MOperand::defVReg(align), MOperand::immediate(16)}});
        emit({MOpcode::MovRR,
              {MOperand::defPhys(abi_.intArgRegs[0]), MOperand::useVReg(size)}});
        emit({MOpcode::MovRR,
              {MOperand::defPhys(abi_.intArgRegs[1]), MOperand::useVReg(align)}});
        MInst c{MOpcode::Call, {MOperand::sym(kWasmAllocSymbol)}};
        c.clobbers = callClobbers(abi_);
        emit(c);
        VReg res = fn_->newVReg();
        emit({MOpcode::MovRR,
              {MOperand::defVReg(res), MOperand::usePhys(abi_.intReturnReg)}});
        return res;
    }
    if (abi_.abi == Abi::Win64) {
        // heap = GetProcessHeap(); HeapAlloc(heap, 0, size)
        VReg heap = emitSimpleImportCall("GetProcessHeap", "kernel32.dll", {});
        VReg flags = fn_->newVReg();
        emit({MOpcode::MovRI, {MOperand::defVReg(flags), MOperand::immediate(0)}});
        return emitSimpleImportCall("HeapAlloc", "kernel32.dll", {heap, flags, size});
    }
    // Linux: mmap(NULL, size, PROT_READ|PROT_WRITE=3, MAP_PRIVATE|MAP_ANONYMOUS=0x22,
    //             fd=-1, offset=0)  via syscall 9. Result (the mapping) in RAX.
    static const PhysReg kSysRegs[7] = {
        PhysReg::RAX, PhysReg::RDI, PhysReg::RSI,
        PhysReg::RDX, PhysReg::R10, PhysReg::R8, PhysReg::R9};
    // Materialize the constant args into vregs first (size is already a vreg).
    auto imm = [&](std::int64_t v) {
        VReg r = fn_->newVReg();
        emit({MOpcode::MovRI, {MOperand::defVReg(r), MOperand::immediate(v)}});
        return r;
    };
    VReg num = imm(9);          // mmap
    VReg addr0 = imm(0);        // addr = NULL
    // size is arg2 (RSI)
    VReg prot = imm(3);         // PROT_READ|PROT_WRITE
    VReg flags = imm(0x22);     // MAP_PRIVATE|MAP_ANONYMOUS
    VReg fd = imm(-1);          // fd = -1
    VReg off = imm(0);          // offset = 0
    VReg vregs[7] = {num, addr0, size, prot, flags, fd, off};
    for (int i = 0; i < 7; ++i) {
        emit({MOpcode::MovRR,
              {MOperand::defPhys(kSysRegs[i]), MOperand::useVReg(vregs[i])}});
    }
    MInst sys{MOpcode::Syscall, {}};
    sys.clobbers = abi_.callerSaved;
    emit(sys);
    VReg res = fn_->newVReg();
    emit({MOpcode::MovRR, {MOperand::defVReg(res), MOperand::usePhys(PhysReg::RAX)}});
    return res;
}

VReg InstructionSelector::emitAlignedMalloc(VReg size, VReg align) {
    // The Win64 heap and Linux x86-64 mmap return at least 16-byte aligned
    // pointers. Larger requested alignments need a recoverable adjusted pointer:
    //   raw = os_alloc(size + align + 16)
    //   user = align_up(raw + 16, align)
    //   [user - 16] = raw
    //   [user -  8] = raw allocation size
    // `emitMaybeAlignedFree` uses the same align value to decide whether to read
    // this metadata or free the pointer directly.
    std::uint32_t resultSlot = fn_->addFrameSlot(8, 8, false);
    auto imm = [&](std::int64_t v) {
        VReg r = fn_->newVReg();
        emit({MOpcode::MovRI, {MOperand::defVReg(r), MOperand::immediate(v)}});
        return r;
    };
    auto storeResult = [&](VReg v) {
        MInst st{MOpcode::Store, {MOperand::slot(resultSlot), MOperand::useVReg(v)}};
        st.width = 8;
        st.isSigned = false;
        emit(st);
    };
    auto loadResult = [&]() {
        VReg r = fn_->newVReg();
        MInst ld{MOpcode::Load, {MOperand::defVReg(r), MOperand::slot(resultSlot)}};
        ld.width = 8;
        ld.isSigned = false;
        emit(ld);
        return r;
    };

    VReg zero = imm(0);
    storeResult(zero);

    std::uint32_t directB = fn_->addBlock();
    std::uint32_t alignedB = fn_->addBlock();
    std::uint32_t alignNonNullB = fn_->addBlock();
    std::uint32_t doneB = fn_->addBlock();

    VReg defaultAlign = imm(16);
    emit({MOpcode::Cmp, {MOperand::useVReg(align), MOperand::useVReg(defaultAlign)}});
    {
        MInst jcc{MOpcode::Jcc, {MOperand::lbl(directB)}};
        jcc.cond = Cond::ULE;
        emit(jcc);
    }
    emit({MOpcode::Jmp, {MOperand::lbl(alignedB)}});

    curBlock_ = directB;
    storeResult(emitMalloc(size));
    emit({MOpcode::Jmp, {MOperand::lbl(doneB)}});

    curBlock_ = alignedB;
    VReg total = fn_->newVReg();
    emit({MOpcode::MovRR, {MOperand::defVReg(total), MOperand::useVReg(size)}});
    emit({MOpcode::Add, {MOperand::useDefVReg(total), MOperand::useVReg(align)}});
    VReg headerBytes = imm(16);
    emit({MOpcode::Cmp, {MOperand::useVReg(total), MOperand::useVReg(size)}});
    {
        MInst jcc{MOpcode::Jcc, {MOperand::lbl(doneB)}};
        jcc.cond = Cond::ULT;
        emit(jcc);
    }
    VReg beforeHeader = fn_->newVReg();
    emit({MOpcode::MovRR, {MOperand::defVReg(beforeHeader), MOperand::useVReg(total)}});
    emit({MOpcode::Add, {MOperand::useDefVReg(total), MOperand::useVReg(headerBytes)}});
    emit({MOpcode::Cmp, {MOperand::useVReg(total), MOperand::useVReg(beforeHeader)}});
    {
        MInst jcc{MOpcode::Jcc, {MOperand::lbl(doneB)}};
        jcc.cond = Cond::ULT;
        emit(jcc);
    }
    VReg raw = emitMalloc(total);
    emit({MOpcode::Cmp, {MOperand::useVReg(raw), MOperand::useVReg(zero)}});
    {
        MInst jcc{MOpcode::Jcc, {MOperand::lbl(doneB)}};
        jcc.cond = Cond::EQ;
        emit(jcc);
    }
    emit({MOpcode::Jmp, {MOperand::lbl(alignNonNullB)}});

    curBlock_ = alignNonNullB;
    VReg candidate = fn_->newVReg();
    emit({MOpcode::MovRR, {MOperand::defVReg(candidate), MOperand::useVReg(raw)}});
    emit({MOpcode::Add, {MOperand::useDefVReg(candidate), MOperand::useVReg(headerBytes)}});
    emit({MOpcode::Add, {MOperand::useDefVReg(candidate), MOperand::useVReg(align)}});
    VReg one = imm(1);
    emit({MOpcode::Sub, {MOperand::useDefVReg(candidate), MOperand::useVReg(one)}});
    VReg quotient = fn_->newVReg();
    MInst div{MOpcode::Div,
              {MOperand::defVReg(quotient), MOperand::useVReg(candidate),
               MOperand::useVReg(align)}};
    div.isSigned = false;
    div.clobbers = {PhysReg::RAX, PhysReg::RDX};
    emit(div);
    VReg user = fn_->newVReg();
    emit({MOpcode::MovRR, {MOperand::defVReg(user), MOperand::useVReg(quotient)}});
    emit({MOpcode::IMul, {MOperand::useDefVReg(user), MOperand::useVReg(align)}});
    MInst stRaw{MOpcode::StoreInd,
                {MOperand::useVReg(user), MOperand::immediate(-16),
                 MOperand::useVReg(raw)}};
    stRaw.width = 8;
    stRaw.isSigned = false;
    emit(stRaw);
    MInst stTotal{MOpcode::StoreInd,
                  {MOperand::useVReg(user), MOperand::immediate(-8),
                   MOperand::useVReg(total)}};
    stTotal.width = 8;
    stTotal.isSigned = false;
    emit(stTotal);
    storeResult(user);
    emit({MOpcode::Jmp, {MOperand::lbl(doneB)}});

    curBlock_ = doneB;
    return loadResult();
}

void InstructionSelector::emitFree(VReg ptr, VReg size) {
    if (wasmTarget_) {
        VReg align = fn_->newVReg();
        emit({MOpcode::MovRI, {MOperand::defVReg(align), MOperand::immediate(16)}});
        VReg args[3] = {ptr, size, align};
        for (int i = 0; i < 3; ++i) {
            emit({MOpcode::MovRR,
                  {MOperand::defPhys(abi_.intArgRegs[i]), MOperand::useVReg(args[i])}});
        }
        MInst c{MOpcode::Call, {MOperand::sym(kWasmFreeSymbol)}};
        c.clobbers = callClobbers(abi_);
        emit(c);
        return;
    }
    if (abi_.abi == Abi::Win64) {
        // HeapFree(GetProcessHeap(), 0, ptr)
        VReg heap = emitSimpleImportCall("GetProcessHeap", "kernel32.dll", {});
        VReg flags = fn_->newVReg();
        emit({MOpcode::MovRI, {MOperand::defVReg(flags), MOperand::immediate(0)}});
        (void)emitSimpleImportCall("HeapFree", "kernel32.dll", {heap, flags, ptr});
        return;
    }

    // Linux: munmap(ptr, size) via syscall 11. The size must match the mmap
    // allocation size, which scalar `delete` can compute from the pointee type.
    static const PhysReg kSysRegs[3] = {PhysReg::RAX, PhysReg::RDI, PhysReg::RSI};
    VReg num = fn_->newVReg();
    emit({MOpcode::MovRI, {MOperand::defVReg(num), MOperand::immediate(11)}});
    VReg vregs[3] = {num, ptr, size};
    for (int i = 0; i < 3; ++i) {
        emit({MOpcode::MovRR,
              {MOperand::defPhys(kSysRegs[i]), MOperand::useVReg(vregs[i])}});
    }
    MInst sys{MOpcode::Syscall, {}};
    sys.clobbers = abi_.callerSaved;
    emit(sys);
}

void InstructionSelector::emitMaybeAlignedFree(VReg ptr, VReg size, VReg align) {
    auto imm = [&](std::int64_t v) {
        VReg r = fn_->newVReg();
        emit({MOpcode::MovRI, {MOperand::defVReg(r), MOperand::immediate(v)}});
        return r;
    };

    std::uint32_t checkAlignB = fn_->addBlock();
    std::uint32_t directB = fn_->addBlock();
    std::uint32_t alignedB = fn_->addBlock();
    std::uint32_t doneB = fn_->addBlock();

    VReg zero = imm(0);
    emit({MOpcode::Cmp, {MOperand::useVReg(ptr), MOperand::useVReg(zero)}});
    {
        MInst jcc{MOpcode::Jcc, {MOperand::lbl(doneB)}};
        jcc.cond = Cond::EQ;
        emit(jcc);
    }
    emit({MOpcode::Jmp, {MOperand::lbl(checkAlignB)}});

    curBlock_ = checkAlignB;
    if (align == kInvalidVReg) {
        emit({MOpcode::Jmp, {MOperand::lbl(directB)}});
    } else {
        VReg defaultAlign = imm(16);
        emit({MOpcode::Cmp, {MOperand::useVReg(align), MOperand::useVReg(defaultAlign)}});
        {
            MInst jcc{MOpcode::Jcc, {MOperand::lbl(directB)}};
            jcc.cond = Cond::ULE;
            emit(jcc);
        }
        emit({MOpcode::Jmp, {MOperand::lbl(alignedB)}});
    }

    curBlock_ = directB;
    emitFree(ptr, size);
    emit({MOpcode::Jmp, {MOperand::lbl(doneB)}});

    curBlock_ = alignedB;
    VReg raw = fn_->newVReg();
    MInst ldRaw{MOpcode::LoadInd,
                {MOperand::defVReg(raw), MOperand::useVReg(ptr),
                 MOperand::immediate(-16)}};
    ldRaw.width = 8;
    ldRaw.isSigned = false;
    emit(ldRaw);
    VReg total = fn_->newVReg();
    MInst ldTotal{MOpcode::LoadInd,
                  {MOperand::defVReg(total), MOperand::useVReg(ptr),
                   MOperand::immediate(-8)}};
    ldTotal.width = 8;
    ldTotal.isSigned = false;
    emit(ldTotal);
    emitFree(raw, total);
    emit({MOpcode::Jmp, {MOperand::lbl(doneB)}});

    curBlock_ = doneB;
}

VReg InstructionSelector::selFloatBinary(const std::string& op, const AST::NodePtr& l,
                                         const AST::NodePtr& r, std::uint8_t fw) {
    VReg lv = selExpr(l);
    VReg rv = selExpr(r);
    if (failed_) return kInvalidVReg;

    // Float remainder: there is no SSE remainder instruction. Compute the
    // truncated (toward-zero) quotient and back out the remainder:
    //   r = a - trunc(a / b) * b
    // trunc() is realized by a round-trip through a 64-bit integer
    // (cvttsd2si then cvtsi2sd), which matches C fmod for the common range
    // where |a/b| fits in a signed 64-bit integer.
    if (op == "%") {
        // q = a / b
        VReg q = fn_->newVReg(RegClass::XMM);
        emitFW(MOpcode::FMovRR, {MOperand::defVReg(q), MOperand::useVReg(lv)}, fw);
        emitFW(MOpcode::FDiv, {MOperand::useDefVReg(q), MOperand::useVReg(rv)}, fw);
        // t = trunc(q): float -> i64 (truncating) -> float
        VReg qi = fn_->newVReg();
        emitFW(MOpcode::CvtF2I, {MOperand::defVReg(qi), MOperand::useVReg(q)}, fw);
        VReg t = fn_->newVReg(RegClass::XMM);
        emitFW(MOpcode::CvtI2F, {MOperand::defVReg(t), MOperand::useVReg(qi)}, fw);
        // t = t * b
        emitFW(MOpcode::FMul, {MOperand::useDefVReg(t), MOperand::useVReg(rv)}, fw);
        // res = a - t
        VReg res = fn_->newVReg(RegClass::XMM);
        emitFW(MOpcode::FMovRR, {MOperand::defVReg(res), MOperand::useVReg(lv)}, fw);
        emitFW(MOpcode::FSub, {MOperand::useDefVReg(res), MOperand::useVReg(t)}, fw);
        return res;
    }

    MOpcode fop;
    if (op == "+") fop = MOpcode::FAdd;
    else if (op == "-") fop = MOpcode::FSub;
    else if (op == "*") fop = MOpcode::FMul;
    else if (op == "/") fop = MOpcode::FDiv;
    else { fail("selector: unsupported float operator '" + op + "'"); return kInvalidVReg; }
    // Two-address form: move lhs into a fresh result, then op with rhs.
    VReg res = fn_->newVReg(RegClass::XMM);
    emitFW(MOpcode::FMovRR, {MOperand::defVReg(res), MOperand::useVReg(lv)}, fw);
    emitFW(fop, {MOperand::useDefVReg(res), MOperand::useVReg(rv)}, fw);
    return res;
}

VReg InstructionSelector::selShift(const AST::ShiftOperationExpr& sh) {
    VReg lv = selExpr(sh.lhs);
    VReg amount = selExpr(sh.rhs);
    if (failed_) return kInvalidVReg;

    Types::TypeRef resultTy = concreteTypeOf(&sh);
    const bool resSigned = isSignedOf(resultTy);
    std::uint8_t w = static_cast<std::uint8_t>(widthOf(resultTy));

    MOpcode mop;
    if (sh.op == "<<") mop = MOpcode::Shl;
    else if (sh.op == ">>") mop = MOpcode::Shr;  // arithmetic vs logical via isSigned
    else { fail("selector: unsupported shift operator '" + sh.op + "'"); return kInvalidVReg; }

    VReg res = fn_->newVReg();
    emit({MOpcode::MovRR, {MOperand::defVReg(res), MOperand::useVReg(lv)}});
    MInst si{mop, {MOperand::useDefVReg(res), MOperand::useVReg(amount)}};
    si.isSigned = resSigned;  // for >>: arithmetic (sar) if signed, else logical (shr)
    si.clobbers = {PhysReg::RCX};  // shift count is parked in CL
    emit(si);

    if (w < 8) {
        MInst ext{MOpcode::Ext, {MOperand::useDefVReg(res)}};
        ext.width = w; ext.isSigned = resSigned;
        emit(ext);
    }
    return res;
}

VReg InstructionSelector::selUnary(const AST::UnaryExpr& un) {
    VReg ov = selExpr(un.operand);
    if (failed_) return kInvalidVReg;

    Types::TypeRef resultTy = concreteTypeOf(&un);

    // Floating-point unary: negate via sign-bit flip (FNeg), unary plus is a no-op.
    if (isFloatType(resultTy)) {
        if (un.op == "+") return ov;
        if (un.op == "-") {
            std::uint8_t fw = floatComputeWidthOf(resultTy);
            VReg res = fn_->newVReg(RegClass::XMM);
            emitFW(MOpcode::FMovRR, {MOperand::defVReg(res), MOperand::useVReg(ov)}, fw);
            emitFW(MOpcode::FNeg, {MOperand::useDefVReg(res)}, fw);
            return res;
        }
        fail("selector: unsupported float unary operator '" + un.op + "'");
        return kInvalidVReg;
    }

    const bool resSigned = isSignedOf(resultTy);
    std::uint8_t w = static_cast<std::uint8_t>(widthOf(resultTy));

    // Logical not: res = (operand == 0) ? 1 : 0, via Cmp-to-zero + SetCC.
    // The result is already a canonical 0/1, so no width re-extension is needed.
    if (un.op == "!") {
        VReg zero = fn_->newVReg();
        emit({MOpcode::MovRI, {MOperand::defVReg(zero), MOperand::immediate(0)}});
        emit({MOpcode::Cmp, {MOperand::useVReg(ov), MOperand::useVReg(zero)}});
        VReg res = fn_->newVReg();
        MInst set{MOpcode::SetCC, {MOperand::defVReg(res)}};
        set.cond = Cond::EQ;
        emit(set);
        return res;
    }

    VReg res = fn_->newVReg();
    emit({MOpcode::MovRR, {MOperand::defVReg(res), MOperand::useVReg(ov)}});

    if (un.op == "-") {
        emit({MOpcode::Neg, {MOperand::useDefVReg(res)}});
    } else if (un.op == "~") {
        emit({MOpcode::Not, {MOperand::useDefVReg(res)}});
    } else if (un.op == "+") {
        // unary plus: no-op
    } else {
        fail("selector: unsupported unary operator '" + un.op + "'");
        return kInvalidVReg;
    }

    if (w < 8) {
        MInst ext{MOpcode::Ext, {MOperand::useDefVReg(res)}};
        ext.width = w; ext.isSigned = resSigned;
        emit(ext);
    }
    return res;
}

VReg InstructionSelector::boolify(VReg v) {
    // Normalize an arbitrary integer value to a canonical 0/1 via (v != 0).
    VReg zero = fn_->newVReg();
    emit({MOpcode::MovRI, {MOperand::defVReg(zero), MOperand::immediate(0)}});
    emit({MOpcode::Cmp, {MOperand::useVReg(v), MOperand::useVReg(zero)}});
    VReg res = fn_->newVReg();
    MInst set{MOpcode::SetCC, {MOperand::defVReg(res)}};
    set.cond = Cond::NE;
    emit(set);
    return res;
}

VReg InstructionSelector::selLogical(const AST::LogicalOperationExpr& lo) {
    const bool isAnd = lo.op == "&&";
    if (!isAnd && lo.op != "||") {
        fail("selector: unsupported logical operator '" + lo.op + "'");
        return kInvalidVReg;
    }

    // Short-circuit via a 1-byte result stack slot:
    //   &&: default 0; if (lhs == 0) skip rhs (result 0); else result = bool(rhs)
    //   ||: default 1; if (lhs != 0) skip rhs (result 1); else result = bool(rhs)
    std::uint32_t resultSlot = fn_->addFrameSlot(8, 8, /*isSpill=*/false);

    // Evaluate lhs and seed the default short-circuit result.
    VReg lhs = selExpr(lo.left);
    if (failed_) return kInvalidVReg;
    VReg def = fn_->newVReg();
    emit({MOpcode::MovRI, {MOperand::defVReg(def), MOperand::immediate(isAnd ? 0 : 1)}});
    MInst st{MOpcode::Store, {MOperand::slot(resultSlot), MOperand::useVReg(def)}};
    st.width = 1; st.isSigned = false;
    emit(st);

    // Compare lhs against 0; branch to the done block on the short-circuit case
    // (&&: lhs == 0 -> done; ||: lhs != 0 -> done).
    VReg zero = fn_->newVReg();
    emit({MOpcode::MovRI, {MOperand::defVReg(zero), MOperand::immediate(0)}});
    emit({MOpcode::Cmp, {MOperand::useVReg(lhs), MOperand::useVReg(zero)}});

    std::uint32_t rhsB = fn_->addBlock();
    std::uint32_t doneB = fn_->addBlock();

    MInst jcc{MOpcode::Jcc, {MOperand::lbl(doneB)}};
    jcc.cond = isAnd ? Cond::EQ : Cond::NE;
    emit(jcc);
    // Explicitly jump to the rhs block instead of relying on physical
    // fall-through: when this logical operator is a condition inside an else
    // branch (e.g. `else if a || b`), the enclosing selIf has already appended
    // its then/else/join blocks, so `rhsB` is NOT the next block in layout and a
    // fall-through would skip rhs evaluation. (selIf/selWhile do the same.)
    emit({MOpcode::Jmp, {MOperand::lbl(rhsB)}});

    // rhs block: evaluate rhs, normalize to 0/1, store into the result slot.
    curBlock_ = rhsB;
    VReg rhs = selExpr(lo.right);
    if (failed_) return kInvalidVReg;
    VReg rhsBool = boolify(rhs);
    MInst st2{MOpcode::Store, {MOperand::slot(resultSlot), MOperand::useVReg(rhsBool)}};
    st2.width = 1; st2.isSigned = false;
    emit(st2);
    emit({MOpcode::Jmp, {MOperand::lbl(doneB)}});

    // done block: load the (0/1) result.
    curBlock_ = doneB;
    VReg res = fn_->newVReg();
    MInst ld{MOpcode::Load, {MOperand::defVReg(res), MOperand::slot(resultSlot)}};
    ld.width = 1; ld.isSigned = false;
    emit(ld);
    return res;
}

VReg InstructionSelector::narrowToHalfIfNeeded(VReg v, Types::TypeRef to) {
    // f16 values live in XMM registers as f32 (the compute form). When a value
    // becomes f16-typed (a cast/arith result), round it to half precision by
    // packing to a half and immediately expanding back to f32, matching what an
    // f16 store/reload would produce.
    if (!isFloat16(to)) return v;
    VReg h = fn_->newVReg(RegClass::XMM);
    emitFW(MOpcode::CvtF32ToF16, {MOperand::defVReg(h), MOperand::useVReg(v)}, 4);
    VReg r = fn_->newVReg(RegClass::XMM);
    emitFW(MOpcode::CvtF16ToF32, {MOperand::defVReg(r), MOperand::useVReg(h)}, 4);
    return r;
}

VReg InstructionSelector::selCast(const AST::CastExpr& cast) {
    // The source value is evaluated to a 64-bit VReg. The destination semantics
    // mirror the LLVM backend's `coerce`:
    //   int->int : truncate (handled implicitly by storing/using low bytes) or
    //              extend; we model the result width via Ext with the SOURCE
    //              signedness when widening.
    //   int<->ptr, ptr<->ptr : reinterpret the 64-bit value (no-op move).
    Types::TypeRef from = concreteTypeOf(cast.expression.get());
    Types::TypeRef to = concreteTypeOf(&cast);

    // Narrowing a 128-bit integer to a <=64-bit type: the source evaluates to the
    // address of its 16-byte value; the result is its low 64-bit word (truncation),
    // re-canonicalized to the destination width. (i128->i128 and *->i128 casts are
    // handled by selI128 via selExpr's early dispatch and never reach here.)
    if (isInt128(from) && !isInt128(to)) {
        VReg addr = selExpr(cast.expression);
        if (failed_) return kInvalidVReg;
        VReg lo = loadHalf(addr, 0);
        if (isFloatType(to)) {
            VReg res = fn_->newVReg(RegClass::XMM);
            emitFW(MOpcode::CvtI2F, {MOperand::defVReg(res), MOperand::useVReg(lo)},
                   floatComputeWidthOf(to));
            return narrowToHalfIfNeeded(res, to);
        }
        std::uint8_t w = static_cast<std::uint8_t>(widthOf(to));
        if (w < 8) {
            MInst ext{MOpcode::Ext, {MOperand::useDefVReg(lo)}};
            ext.width = w; ext.isSigned = isSignedOf(to);
            emit(ext);
        }
        return lo;
    }

    if (isFloat128(from) || isFloat128(to)) {
        fail("selector: f128 conversions are not supported yet");
        return kInvalidVReg;
    }

    VReg v = selExpr(cast.expression);
    if (failed_) return kInvalidVReg;

    const bool toFloat = isFloatType(to);
    const bool fromFloat = isFloatType(from);
    if (toFloat || fromFloat) {
        if (toFloat && fromFloat) {
            // float<->float. Values live in registers at their COMPUTE width
            // (f16 as f32), so convert between the compute widths and, when the
            // destination is f16, round the f32 result to half precision.
            std::uint8_t fwTo = floatComputeWidthOf(to);
            std::uint8_t fwFrom = floatComputeWidthOf(from);
            if (fwTo == fwFrom) return narrowToHalfIfNeeded(v, to);
            VReg res = fn_->newVReg(RegClass::XMM);
            emitFW(MOpcode::CvtF2F, {MOperand::defVReg(res), MOperand::useVReg(v)}, fwTo);
            return narrowToHalfIfNeeded(res, to);
        }
        if (toFloat) {
            // int -> float (cvtsi2sd / cvtsi2ss). Source is a 64-bit GP value;
            // tag the destination float compute precision (f16 computes as f32).
            VReg res = fn_->newVReg(RegClass::XMM);
            emitFW(MOpcode::CvtI2F, {MOperand::defVReg(res), MOperand::useVReg(v)},
                   floatComputeWidthOf(to));
            return narrowToHalfIfNeeded(res, to);
        }
        // float -> int (cvttsd2si / cvttss2si, truncating) into a GP vreg, then
        // re-canonicalize to the destination integer width. Tag the SOURCE float
        // compute precision (f16 sources are already f32 in the register).
        if (!isIntegerLike(to) && !(to && to->isPointerLike())) {
            fail("selector: unsupported cast from float (only to integer/pointer)");
            return kInvalidVReg;
        }
        VReg res = fn_->newVReg();
        emitFW(MOpcode::CvtF2I, {MOperand::defVReg(res), MOperand::useVReg(v)},
               floatComputeWidthOf(from));
        unsigned dstW = widthOf(to);
        if (dstW < 8 && isIntegerLike(to)) {
            MInst ext{MOpcode::Ext, {MOperand::useDefVReg(res)}};
            ext.width = static_cast<std::uint8_t>(dstW);
            ext.isSigned = isSignedOf(to);
            emit(ext);
        }
        return res;
    }

    // Pointer/integer reinterpretation: values are already 64-bit; nothing to do.
    // A function value (lambda / function pointer) is likewise a 64-bit address,
    // so casting it to/from an integer or pointer preserves the bit pattern.
    const bool toPtr = (to && to->isPointerLike()) || (to && to->kind == Types::Kind::Function);
    const bool fromPtr = (from && from->isPointerLike()) || (from && from->kind == Types::Kind::Function);
    if (toPtr || fromPtr) {
        // int<->ptr, ptr<->ptr, fn<->int, fn<->ptr: the 64-bit value is preserved.
        return v;
    }

    if (!isIntegerLike(to) || !isIntegerLike(from)) {
        fail("selector: unsupported cast (only integer/pointer casts supported)");
        return kInvalidVReg;
    }

    unsigned dstW = widthOf(to);
    unsigned srcW = widthOf(from);
    if (dstW == srcW) return v;  // same width: no-op

    // Re-canonicalize the value to the destination width. Loads/identifiers keep
    // values extended to 64 bits, so the VReg already holds a 64-bit value. For a
    // narrowing cast we must mask to the destination width before re-extending;
    // for a widening cast we re-extend the source's low `srcW` bytes using the
    // SOURCE signedness (matching LLVM's sext/zext-from-source rule).
    VReg res = fn_->newVReg();
    emit({MOpcode::MovRR, {MOperand::defVReg(res), MOperand::useVReg(v)}});
    if (dstW < srcW) {
        // Narrowing: keep only the low `dstW` bytes, re-extended per destination
        // signedness (the canonical 64-bit form of the narrowed value).
        MInst ext{MOpcode::Ext, {MOperand::useDefVReg(res)}};
        ext.width = static_cast<std::uint8_t>(dstW);
        ext.isSigned = isSignedOf(to);
        emit(ext);
    } else {
        // Widening: re-extend the source's low `srcW` bytes per SOURCE signedness.
        MInst ext{MOpcode::Ext, {MOperand::useDefVReg(res)}};
        ext.width = static_cast<std::uint8_t>(srcW);
        ext.isSigned = isSignedOf(from);
        emit(ext);
    }
    return res;
}

VReg InstructionSelector::materializeAddr(const ElemAddr& a) {
    // Collapse a base+disp lvalue address into a single register. A zero
    // displacement is already the base register; otherwise fold via `lea`.
    if (a.disp == 0) return a.base;
    VReg v = fn_->newVReg();
    emit({MOpcode::LeaDisp,
          {MOperand::defVReg(v), MOperand::useVReg(a.base), MOperand::immediate(a.disp)}});
    return v;
}

void InstructionSelector::emitStructCopy(const ElemAddr& dst, const ElemAddr& src,
                                         unsigned size) {
    // Byte-for-byte copy via width-greedy LoadInd/StoreInd chunks. Each chunk
    // loads from [src.base + src.disp + off] and stores to [dst.base + ...].
    // Treated as raw bytes (unsigned, no sign extension matters since we restore
    // the exact low bytes on store).
    std::int64_t off = 0;
    unsigned remaining = size;
    while (remaining > 0) {
        unsigned chunk = remaining >= 8 ? 8 : remaining >= 4 ? 4 : remaining >= 2 ? 2 : 1;
        VReg tmp = fn_->newVReg();
        MInst ld{MOpcode::LoadInd,
                 {MOperand::defVReg(tmp), MOperand::useVReg(src.base),
                  MOperand::immediate(src.disp + off)}};
        ld.width = static_cast<std::uint8_t>(chunk);
        ld.isSigned = false;
        emit(ld);
        MInst st{MOpcode::StoreInd,
                 {MOperand::useVReg(dst.base), MOperand::immediate(dst.disp + off),
                  MOperand::useVReg(tmp)}};
        st.width = static_cast<std::uint8_t>(chunk);
        st.isSigned = false;
        emit(st);
        off += chunk;
        remaining -= chunk;
    }
}

VReg InstructionSelector::selAddressOf(const AST::AddressOfExpr& addr) {
    // &<lvalue> : materialize the operand's storage address. The general lvalue
    // resolver handles &x, &a[i], &a.field, &a[i].field, &*p; a residual constant
    // displacement folds into a single `lea reg,[base+disp]`.
    if (!addr.operand) {
        fail("selector: address-of requires an operand");
        return kInvalidVReg;
    }
    ElemAddr a = computeLValueAddr(addr.operand.get());
    if (failed_) return kInvalidVReg;
    return materializeAddr(a);
}

VReg InstructionSelector::selArg(const AST::NodePtr& arg) {
    // Evaluate a call argument to the vreg that occupies its ABI slot. A struct/
    // class argument is passed BY HIDDEN POINTER (matching the callee's struct
    // param convention), so pass the address of its storage rather than a value.
    Types::TypeRef ty = concreteTypeOf(arg.get());
    if (ty && (ty->kind == Types::Kind::Struct || ty->kind == Types::Kind::Class)) {
        ElemAddr a = computeLValueAddr(arg.get());
        if (failed_) return kInvalidVReg;
        return materializeAddr(a);
    }
    return selExpr(arg);
}

bool InstructionSelector::isAggregatePrvalueTemp(const AST::ExprAST* node) {
    if (!node) return false;
    switch (node->nodeType()) {
        case AST::NodeType::FunctionCall:        // ctor call / struct-returning call
        case AST::NodeType::StructInstantiation: // Point{ ... }
        case AST::NodeType::ArrayLiteral:        // [a, b, c]
            return true;
        default:
            return false;
    }
}

void InstructionSelector::appendAggregateArgValues(
    Types::TypeRef ty, const ElemAddr& addr,
    std::vector<VReg>& argVRegs, std::vector<bool>& argIsFloat,
    std::vector<std::uint8_t>& argFloatW, std::vector<bool>& argIsAggMem,
    const AST::ExprAST* argNode) {
    // Lowers a by-value aggregate argument whose storage is at `addr` into the
    // flat arg-value list consumed by the call placement logic. Per the active
    // ABI classification:
    //   - in-memory: pass the storage ADDRESS as a single hidden-pointer GP
    //     value. A persistent lvalue is first copied into a fresh temporary so
    //     the callee receives its own object (by-value semantics) and cannot
    //     mutate the caller's; a prvalue temporary is already fresh storage that
    //     nothing else observes, so it is passed through as-is (copying it would
    //     also double-run a class temporary's destructor).
    //   - in registers: load each eightbyte from memory into a GP/XMM vreg and
    //     push it as an independent scalar arg value.
    AggregateAbi cls = classifyAggregate(ty);
    if (cls.inMemory) {
        ElemAddr pass = addr;
        if (!isAggregatePrvalueTemp(argNode)) {
            unsigned sz = sizeOf(ty), al = alignOf(ty);
            std::uint32_t tmp = fn_->addFrameSlot(sz ? sz : 8, al ? al : 8, false);
            VReg copyAddr = fn_->newVReg();
            emit({MOpcode::LeaSlot, {MOperand::defVReg(copyAddr), MOperand::slot(tmp)}});
            emitStructCopy(ElemAddr{copyAddr, 0}, addr, sz);
            if (failed_) return;
            // The copy is a distinct object; a class with a destructor is
            // destroyed once at statement end (no-op for other types).
            registerTemporaryCleanup(ty, tmp);
            pass = ElemAddr{copyAddr, 0};
        }
        VReg p = materializeAddr(pass);
        argVRegs.push_back(p);
        argIsFloat.push_back(false);
        argFloatW.push_back(8);
        argIsAggMem.push_back(true);
        return;
    }
    VReg base = materializeAddr(addr);
    for (std::size_t e = 0; e < cls.eightbytes.size(); ++e) {
        const AbiEightbyte& eb = cls.eightbytes[e];
        std::int64_t off = static_cast<std::int64_t>(e) * 8;
        if (eb.isSSE) {
            // The eightbyte is a single 4- or 8-byte float value.
            std::uint8_t fw = (eb.bytes <= 4) ? 4 : 8;
            VReg v = fn_->newVReg(RegClass::XMM);
            emitFW(MOpcode::FLoadInd,
                   {MOperand::defVReg(v), MOperand::useVReg(base),
                    MOperand::immediate(off)}, fw);
            argVRegs.push_back(v);
            argIsFloat.push_back(true);
            argFloatW.push_back(fw);
            argIsAggMem.push_back(false);
        } else {
            // INTEGER eightbyte: load `eb.bytes` bytes (zero-extended) into a GP
            // vreg. A partial (1/2/4 byte) trailing eightbyte uses that width.
            std::uint8_t w = static_cast<std::uint8_t>(eb.bytes);
            if (w != 1 && w != 2 && w != 4 && w != 8) {
                // Round up to the next loadable width; the upper bytes are
                // don't-care padding within the register.
                w = (w < 4) ? (w < 2 ? 1 : 2) : (w <= 4 ? 4 : 8);
                if (eb.bytes > 4) w = 8;
                else if (eb.bytes > 2) w = 4;
                else if (eb.bytes > 1) w = 2;
                else w = 1;
            }
            VReg v = fn_->newVReg();
            MInst ld{MOpcode::LoadInd,
                     {MOperand::defVReg(v), MOperand::useVReg(base),
                      MOperand::immediate(off)}};
            ld.width = w; ld.isSigned = false;
            emit(ld);
            argVRegs.push_back(v);
            argIsFloat.push_back(false);
            argFloatW.push_back(8);
            argIsAggMem.push_back(false);
        }
    }
}

VReg InstructionSelector::selDeref(const AST::DereferenceExpr& deref) {
    // *p : load the pointee through the pointer value in a register.
    VReg base = selExpr(deref.operand);
    if (failed_) return kInvalidVReg;
    Types::TypeRef resultTy = concreteTypeOf(&deref);
    if (isFloatType(resultTy)) {
        VReg v = fn_->newVReg(RegClass::XMM);
        emitFW(MOpcode::FLoadInd,
               {MOperand::defVReg(v), MOperand::useVReg(base), MOperand::immediate(0)},
               floatWidthOf(resultTy));
        return v;
    }
    std::uint8_t w = static_cast<std::uint8_t>(widthOf(resultTy));
    bool sgn = isSignedOf(resultTy);
    VReg v = fn_->newVReg();
    MInst ld{MOpcode::LoadInd,
             {MOperand::defVReg(v), MOperand::useVReg(base), MOperand::immediate(0)}};
    ld.width = w; ld.isSigned = sgn;
    emit(ld);
    return v;
}

unsigned InstructionSelector::elementSizeOf(Types::TypeRef ptrLike) const {
    // The byte size of the element a pointer/array/text points at. `text` has an
    // implicit u8 element (element == nullptr). Aggregate elements use sizeOf so
    // arrays of structs stride by the full (aligned) struct size.
    if (!ptrLike) return 1;
    if (ptrLike->kind == Types::Kind::Text) return 1;
    if (ptrLike->element) return sizeOf(ptrLike->element);
    return 1;
}

VReg InstructionSelector::emitScaledAddr(VReg base, VReg idx, unsigned elemSize,
                                         std::int64_t disp) {
    // Computes `base + idx*elemSize + disp` into a fresh vreg. When the element
    // size is an x86 SIB scale (1/2/4/8) the whole computation (including a
    // constant displacement) folds into a single `lea reg,[base+idx*scale+disp]`
    // (LeaIndex). Otherwise it falls back to an explicit IMul (index*size) + Add
    // (base + scaled), then an Add of the displacement.
    const bool sibScale =
        elemSize == 1 || elemSize == 2 || elemSize == 4 || elemSize == 8;
    if (sibScale) {
        VReg addr = fn_->newVReg();
        MInst lea{MOpcode::LeaIndex,
                  {MOperand::defVReg(addr), MOperand::useVReg(base),
                   MOperand::useVReg(idx), MOperand::immediate(disp)}};
        lea.scale = static_cast<std::uint8_t>(elemSize);
        emit(lea);
        return addr;
    }
    // Non-power-of-two element size: scale explicitly, then add.
    VReg sz = fn_->newVReg();
    emit({MOpcode::MovRI,
          {MOperand::defVReg(sz), MOperand::immediate(static_cast<std::int64_t>(elemSize))}});
    VReg scaled = fn_->newVReg();
    emit({MOpcode::MovRR, {MOperand::defVReg(scaled), MOperand::useVReg(idx)}});
    emit({MOpcode::IMul, {MOperand::useDefVReg(scaled), MOperand::useVReg(sz)}});
    VReg addr = fn_->newVReg();
    emit({MOpcode::MovRR, {MOperand::defVReg(addr), MOperand::useVReg(base)}});
    emit({MOpcode::Add, {MOperand::useDefVReg(addr), MOperand::useVReg(scaled)}});
    if (disp != 0) {
        VReg dv = fn_->newVReg();
        emit({MOpcode::MovRI, {MOperand::defVReg(dv), MOperand::immediate(disp)}});
        emit({MOpcode::Add, {MOperand::useDefVReg(addr), MOperand::useVReg(dv)}});
    }
    return addr;
}

VReg InstructionSelector::selPointerArith(const std::string& op,
                                          const AST::NodePtr& l, const AST::NodePtr& r,
                                          Types::TypeRef ptrTy) {
    // Determine which operand is the pointer (the other is the integer index).
    Types::TypeRef lt = concreteTypeOf(l.get());
    const bool lhsPtr = lt && lt->isPointerLike();
    const AST::NodePtr& ptrNode = lhsPtr ? l : r;
    const AST::NodePtr& idxNode = lhsPtr ? r : l;

    VReg ptr = selExpr(ptrNode);
    VReg idx = selExpr(idxNode);
    if (failed_) return kInvalidVReg;

    unsigned elemSize = elementSizeOf(ptrTy);
    // `p - i` == `p + (-i)`: negate the index so the same scaled-address path (and
    // SIB lea) applies.
    if (op == "-") {
        VReg neg = fn_->newVReg();
        emit({MOpcode::MovRR, {MOperand::defVReg(neg), MOperand::useVReg(idx)}});
        emit({MOpcode::Neg, {MOperand::useDefVReg(neg)}});
        idx = neg;
    }
    return emitScaledAddr(ptr, idx, elemSize);
}

InstructionSelector::ElemAddr
InstructionSelector::computeElementAddr(const AST::MemberAccessExpr& m,
                                        Types::TypeRef& elemTyOut) {
    // Computes the address of `base[index]`. The element type is what sema's
    // checkIndex recorded on the index node.
    elemTyOut = concreteTypeOf(&m);
    Types::TypeRef baseTy = concreteTypeOf(m.object.get());
    unsigned elemSize = elementSizeOf(baseTy);

    // Resolve the base address. A pointer/text base is a VALUE (any expression):
    // its value IS the address, so evaluate it directly. An array/aggregate base
    // is an lvalue whose storage address we compute (composes a[i][j], s.arr[i]).
    // A slice indexes through its `.ptr` field: load the pointer out of the slice
    // header (at [header + 0]) and use that as the element base.
    //
    // Under --bounds-check we also capture the source length: a slice's runtime
    // `.len` (header + 8) or a fixed array's compile-time element count. Pointer /
    // text sources carry no length and are not checked.
    VReg base;
    std::int64_t baseDisp = 0;
    VReg lenV = kInvalidVReg;     // runtime length (slice), if bounds-checking
    std::int64_t staticLen = -1;  // compile-time length (fixed array), else -1
    if (isSliceType(baseTy)) {
        ElemAddr hdr = computeLValueAddr(m.object.get());
        if (failed_) return {};
        VReg headerAddr = materializeAddr(hdr);
        base = fn_->newVReg();
        MInst ld{MOpcode::LoadInd,
                 {MOperand::defVReg(base), MOperand::useVReg(headerAddr),
                  MOperand::immediate(0)}};
        ld.width = 8; ld.isSigned = false;
        emit(ld);
        if (boundsCheck_) lenV = loadHalf(headerAddr, 8);
    } else if (baseTy && baseTy->isPointerLike()) {
        base = selExpr(m.object);
        if (failed_) return {};
    } else {
        ElemAddr baseAddr = computeLValueAddr(m.object.get());
        if (failed_) return {};
        base = baseAddr.base;
        baseDisp = baseAddr.disp;
        if (baseTy && baseTy->kind == Types::Kind::Array) {
            staticLen = baseTy->arrayLength > 0 ? baseTy->arrayLength : 0;
        }
    }

    const bool checkable = boundsCheck_ && (lenV != kInvalidVReg || staticLen >= 0);
    const bool constIdx =
        m.property && m.property->nodeType() == AST::NodeType::IntegerLiteral;

    // Bounds check. A constant index into a fixed array is verified at compile
    // time (out-of-range is a hard error); everything else compares against the
    // length at runtime and traps on failure.
    VReg idxChecked = kInvalidVReg;
    if (checkable) {
        if (constIdx) {
            std::int64_t v =
                static_cast<std::int64_t>(
                    static_cast<const AST::IntegerLiteral&>(*m.property).value);
            if (staticLen >= 0) {
                if (v < 0 || v >= staticLen) {
                    fail("selector: index " + std::to_string(v) +
                         " is out of bounds for array of length " +
                         std::to_string(staticLen));
                    return {};
                }
            } else {
                VReg idxV = fn_->newVReg();
                emit({MOpcode::MovRI, {MOperand::defVReg(idxV), MOperand::immediate(v)}});
                emitBoundsTrap(idxV, lenV, Cond::UGE);
            }
        } else {
            idxChecked = selExpr(m.property);
            if (failed_) return {};
            VReg lenReg = lenV;
            if (lenReg == kInvalidVReg) {
                lenReg = fn_->newVReg();
                emit({MOpcode::MovRI,
                      {MOperand::defVReg(lenReg), MOperand::immediate(staticLen)}});
            }
            emitBoundsTrap(idxChecked, lenReg, Cond::UGE);
        }
    }

    // Constant index: fold `index * elemSize` (+ base disp) into a displacement.
    if (constIdx) {
        const auto& lit = static_cast<const AST::IntegerLiteral&>(*m.property);
        return ElemAddr{base, baseDisp + static_cast<std::int64_t>(lit.value) *
                                             static_cast<std::int64_t>(elemSize)};
    }

    // Reuse the already-evaluated index from the bounds check when present, so the
    // index expression is not evaluated twice (it may have side effects).
    VReg idx = idxChecked != kInvalidVReg ? idxChecked : selExpr(m.property);
    if (failed_) return {};
    // Non-constant index: compute base + idx*elemSize + baseDisp in one lea.
    return ElemAddr{emitScaledAddr(base, idx, elemSize, baseDisp), 0};
}

void InstructionSelector::emitBoundsTrap(VReg a, VReg b, Cond trapCond) {
    // if (a <trapCond> b) ud2. Split the current block: continue in `okB`, trap
    // in `trapB`.
    std::uint32_t trapB = fn_->addBlock();
    std::uint32_t okB = fn_->addBlock();
    emit({MOpcode::Cmp, {MOperand::useVReg(a), MOperand::useVReg(b)}});
    { MInst jcc{MOpcode::Jcc, {MOperand::lbl(trapB)}}; jcc.cond = trapCond; emit(jcc); }
    emit({MOpcode::Jmp, {MOperand::lbl(okB)}});
    curBlock_ = trapB;
    emit({MOpcode::AsmFixed, {MOperand::immediate(4)}});  // ud2 -> SIGILL / #UD
    emit({MOpcode::Jmp, {MOperand::lbl(okB)}});           // unreachable; keeps CFG valid
    curBlock_ = okB;
}

InstructionSelector::ElemAddr
InstructionSelector::materializeStructInstantiation(const AST::StructInstantiation& lit) {
    Types::TypeRef ty = concreteTypeOf(&lit);
    if (!ty || (ty->kind != Types::Kind::Struct && ty->kind != Types::Kind::Class)) {
        fail("selector: struct literal '" + lit.typeName + "' has no aggregate type");
        return {};
    }
    unsigned sz = sizeOf(ty);
    unsigned al = alignOf(ty);
    std::uint32_t slot = fn_->addFrameSlot(sz ? sz : 8, al ? al : 8, /*isSpill=*/false);
    VReg baseAddr = fn_->newVReg();
    emit({MOpcode::LeaSlot, {MOperand::defVReg(baseAddr), MOperand::slot(slot)}});

    for (const auto& fv : lit.fieldValues) {
        std::int64_t off = 0;
        Types::TypeRef fieldTy = nullptr;
        if (!fieldOffsetOf(ty, fv.name, off, fieldTy)) {
            fail("selector: unknown field '" + fv.name + "' in literal '" + lit.typeName + "'");
            return {};
        }
        // Nested aggregate field: materialize its address and copy by bytes.
        if (fieldTy &&
            (fieldTy->kind == Types::Kind::Struct || fieldTy->kind == Types::Kind::Class)) {
            ElemAddr src = computeLValueAddr(fv.value.get());
            if (failed_) return {};
            emitStructCopy(ElemAddr{baseAddr, off}, src, sizeOf(fieldTy));
            if (failed_) return {};
            continue;
        }
        VReg v = selExpr(fv.value);
        if (failed_) return {};
        if (isFloatType(fieldTy)) {
            emitFW(MOpcode::FStoreInd,
                   {MOperand::useVReg(baseAddr), MOperand::immediate(off),
                    MOperand::useVReg(v)},
                   floatWidthOf(fieldTy));
        } else {
            MInst st{MOpcode::StoreInd,
                     {MOperand::useVReg(baseAddr), MOperand::immediate(off),
                      MOperand::useVReg(v)}};
            st.width = static_cast<std::uint8_t>(widthOf(fieldTy));
            st.isSigned = isSignedOf(fieldTy);
            emit(st);
        }
    }
    return ElemAddr{baseAddr, 0};
}

InstructionSelector::ElemAddr
InstructionSelector::materializeArrayLiteral(const AST::ArrayLiteral& lit) {
    // `[e0, e1, ...]` -> a fresh stack temp of sizeOf(T[N]) bytes; store each
    // element at base + i*stride. The element type / count come from sema's array
    // type for the literal (T = type of the first element, N = element count).
    Types::TypeRef ty = concreteTypeOf(&lit);
    if (!ty || ty->kind != Types::Kind::Array) {
        fail("selector: array literal has no array type");
        return {};
    }
    Types::TypeRef elemTy = ty->element;
    unsigned stride = elemTy ? sizeOf(elemTy) : 1;
    unsigned sz = sizeOf(ty);
    if (sz == 0) sz = stride ? stride : 8;
    unsigned al = alignOf(ty);
    std::uint32_t slot = fn_->addFrameSlot(sz, al ? al : 8, /*isSpill=*/false);
    VReg baseAddr = fn_->newVReg();
    emit({MOpcode::LeaSlot, {MOperand::defVReg(baseAddr), MOperand::slot(slot)}});

    std::int64_t off = 0;
    for (const auto& e : lit.elements) {
        // Nested aggregate element (array of structs / arrays): copy by bytes.
        if (elemTy && (elemTy->kind == Types::Kind::Struct ||
                       elemTy->kind == Types::Kind::Class ||
                       elemTy->kind == Types::Kind::Array)) {
            ElemAddr src = computeLValueAddr(e.get());
            if (failed_) return {};
            emitStructCopy(ElemAddr{baseAddr, off}, src, stride);
            if (failed_) return {};
        } else if (isFloatType(elemTy)) {
            VReg v = selExpr(e);
            if (failed_) return {};
            emitFW(MOpcode::FStoreInd,
                   {MOperand::useVReg(baseAddr), MOperand::immediate(off),
                    MOperand::useVReg(v)},
                   floatWidthOf(elemTy));
        } else {
            VReg v = selExpr(e);
            if (failed_) return {};
            MInst st{MOpcode::StoreInd,
                     {MOperand::useVReg(baseAddr), MOperand::immediate(off),
                      MOperand::useVReg(v)}};
            st.width = static_cast<std::uint8_t>(widthOf(elemTy));
            st.isSigned = isSignedOf(elemTy);
            emit(st);
        }
        off += static_cast<std::int64_t>(stride);
    }
    return ElemAddr{baseAddr, 0};
}

bool InstructionSelector::emitSliceInitInto(VReg destAddr, Types::TypeRef sliceTy,
                                            const AST::NodePtr& init) {
    if (!init) return true;
    auto storeField = [&](std::int64_t off, VReg v) {
        MInst st{MOpcode::StoreInd,
                 {MOperand::useVReg(destAddr), MOperand::immediate(off),
                  MOperand::useVReg(v)}};
        st.width = 8; st.isSigned = false;
        emit(st);
    };
    // `new T[n]` / `new T`: the allocation pointer plus its element count.
    if (init->nodeType() == AST::NodeType::NewExpression) {
        VReg countV = kInvalidVReg;
        VReg ptr = selNew(static_cast<const AST::NewExpression&>(*init), &countV);
        if (failed_) return false;
        storeField(0, ptr);
        if (countV == kInvalidVReg) {
            countV = fn_->newVReg();
            emit({MOpcode::MovRI, {MOperand::defVReg(countV), MOperand::immediate(0)}});
        }
        storeField(8, countV);
        return true;
    }
    Types::TypeRef initTy = concreteTypeOf(init.get());
    // Another slice value: copy its 16-byte { ptr, len } header verbatim.
    if (isSliceType(initTy)) {
        ElemAddr src = computeLValueAddr(init.get());
        if (failed_) return false;
        emitStructCopy(ElemAddr{destAddr, 0}, src, 16);
        return !failed_;
    }
    // Fixed array or array literal: point at its first element; the length is the
    // array's static element count.
    if (initTy && initTy->kind == Types::Kind::Array) {
        ElemAddr src = computeLValueAddr(init.get());
        if (failed_) return false;
        VReg base = materializeAddr(src);
        storeField(0, base);
        VReg len = fn_->newVReg();
        emit({MOpcode::MovRI,
              {MOperand::defVReg(len),
               MOperand::immediate(initTy->arrayLength > 0 ? initTy->arrayLength : 0)}});
        storeField(8, len);
        return true;
    }
    fail("selector: unsupported slice initializer");
    return false;
}

InstructionSelector::ElemAddr
InstructionSelector::materializeSliceValue(Types::TypeRef sliceTy,
                                           const AST::NodePtr& init) {
    std::uint32_t slot = fn_->addFrameSlot(16, 8, /*isSpill=*/false);
    VReg addr = fn_->newVReg();
    emit({MOpcode::LeaSlot, {MOperand::defVReg(addr), MOperand::slot(slot)}});
    emitSliceInitInto(addr, sliceTy, init);
    return ElemAddr{addr, 0};
}

InstructionSelector::ElemAddr
InstructionSelector::materializeSliceExpr(const AST::SliceExpr& node) {
    Types::TypeRef sliceTy = concreteTypeOf(&node);
    Types::TypeRef elemTy = sliceTy ? sliceTy->element : nullptr;
    unsigned elemSize = elemTy ? sizeOf(elemTy) : 1;
    if (elemSize == 0) elemSize = 1;

    Types::TypeRef baseTy = concreteTypeOf(node.object.get());

    // Resolve the source's base pointer and (when needed) its length.
    VReg basePtr = kInvalidVReg;
    VReg baseLen = kInvalidVReg;  // filled lazily; only needed for an open end
    if (isSliceType(baseTy)) {
        ElemAddr hdr = computeLValueAddr(node.object.get());
        if (failed_) return {};
        VReg headerAddr = materializeAddr(hdr);
        basePtr = fn_->newVReg();
        MInst ld{MOpcode::LoadInd,
                 {MOperand::defVReg(basePtr), MOperand::useVReg(headerAddr),
                  MOperand::immediate(0)}};
        ld.width = 8; ld.isSigned = false;
        emit(ld);
        baseLen = loadHalf(headerAddr, 8);
    } else if (baseTy && baseTy->kind == Types::Kind::Array) {
        ElemAddr arr = computeLValueAddr(node.object.get());
        if (failed_) return {};
        basePtr = materializeAddr(arr);
        baseLen = fn_->newVReg();
        emit({MOpcode::MovRI,
              {MOperand::defVReg(baseLen),
               MOperand::immediate(baseTy->arrayLength > 0 ? baseTy->arrayLength : 0)}});
    } else if (baseTy && baseTy->kind == Types::Kind::Text) {
        basePtr = selExpr(node.object);
        if (failed_) return {};
        // The length is needed for an open end and for the bounds check.
        if (!node.end || boundsCheck_) baseLen = emitStrlenOf(basePtr);
    } else if (baseTy && baseTy->kind == Types::Kind::Pointer) {
        basePtr = selExpr(node.object);
        if (failed_) return {};
        // sema guarantees an explicit end bound for a pointer source; its length
        // is unknown, so the upper bound cannot be range-checked.
    } else {
        fail("selector: cannot slice this value");
        return {};
    }

    // start (defaults to 0) and end (defaults to the source length).
    VReg start = kInvalidVReg;
    if (node.start) {
        start = selExpr(node.start);
        if (failed_) return {};
    }
    VReg end = kInvalidVReg;
    if (node.end) {
        end = selExpr(node.end);
        if (failed_) return {};
    } else {
        end = baseLen;
    }

    // Bounds check: 0 <= start <= end <= len. `start`/`end` default to 0 / len,
    // which are trivially in range, so only explicit bounds need checking.
    if (boundsCheck_) {
        VReg startReg = start;
        if (startReg == kInvalidVReg) {
            startReg = fn_->newVReg();
            emit({MOpcode::MovRI, {MOperand::defVReg(startReg), MOperand::immediate(0)}});
        }
        if (end != kInvalidVReg) {
            // trap if start > end (also catches a negative start, unsigned).
            emitBoundsTrap(startReg, end, Cond::UGT);
            // trap if end > len (only when the source length is known).
            if (baseLen != kInvalidVReg) {
                emitBoundsTrap(end, baseLen, Cond::UGT);
            }
        }
    }

    // newPtr = basePtr + start * elemSize   (start == 0 -> basePtr unchanged)
    VReg newPtr = basePtr;
    if (start != kInvalidVReg) {
        newPtr = emitScaledAddr(basePtr, start, elemSize);
    }

    // newLen = end - start   (start == 0 -> end)
    VReg newLen = fn_->newVReg();
    if (end == kInvalidVReg) {
        // Should not happen (sema enforces a determinable length); be safe.
        emit({MOpcode::MovRI, {MOperand::defVReg(newLen), MOperand::immediate(0)}});
    } else if (start == kInvalidVReg) {
        emit({MOpcode::MovRR, {MOperand::defVReg(newLen), MOperand::useVReg(end)}});
    } else {
        emit({MOpcode::MovRR, {MOperand::defVReg(newLen), MOperand::useVReg(end)}});
        emit({MOpcode::Sub, {MOperand::useDefVReg(newLen), MOperand::useVReg(start)}});
    }

    std::uint32_t slot = fn_->addFrameSlot(16, 8, /*isSpill=*/false);
    VReg addr = fn_->newVReg();
    emit({MOpcode::LeaSlot, {MOperand::defVReg(addr), MOperand::slot(slot)}});
    MInst stPtr{MOpcode::StoreInd,
                {MOperand::useVReg(addr), MOperand::immediate(0), MOperand::useVReg(newPtr)}};
    stPtr.width = 8; stPtr.isSigned = false;
    emit(stPtr);
    MInst stLen{MOpcode::StoreInd,
                {MOperand::useVReg(addr), MOperand::immediate(8), MOperand::useVReg(newLen)}};
    stLen.width = 8; stLen.isSigned = false;
    emit(stLen);
    return ElemAddr{addr, 0};
}

void InstructionSelector::appendSliceArgValue(Types::TypeRef sliceTy,
                                              const AST::NodePtr& arg,
                                              std::vector<VReg>& argVRegs,
                                              std::vector<bool>& argIsFloat,
                                              std::vector<std::uint8_t>& argFloatW,
                                              std::vector<bool>& argIsAggMem) {
    Types::TypeRef aty = concreteTypeOf(arg.get());
    ElemAddr addr;
    if (isSliceType(aty)) {
        addr = computeLValueAddr(arg.get());
    } else {
        // A contextual array / `new T[n]` argument: build a fresh slice header.
        addr = materializeSliceValue(sliceTy, arg);
    }
    if (failed_) return;
    appendAggregateArgValues(sliceTy, addr, argVRegs, argIsFloat, argFloatW,
                             argIsAggMem, arg.get());
}

VReg InstructionSelector::selNew(const AST::NewExpression& ne, VReg* arrayCountOut) {
    // sema types `new T` and `new T[n]` both as `T*`. Allocate a 16-byte header
    // followed by sizeOf(T) bytes per element. The user pointer skips the header:
    //   [base + 0] = element count
    //   [base + 8] = total allocation size
    //   base + 16  = first element returned to source code
    // `delete` uses the header to destruct arrays and release the original
    // allocation.
    Types::TypeRef ptrTy = concreteTypeOf(&ne);
    Types::TypeRef elemTy =
        (ptrTy && ptrTy->kind == Types::Kind::Pointer) ? ptrTy->element : nullptr;

    // Modern parsing leaves `new Cell[count](...)` as typeName == "Cell" plus
    // ne.arraySize. Keep the legacy suffix split for old/generated ASTs that may
    // still arrive as typeName == "Cell[3]".
    std::string baseName = ne.typeName;
    std::int64_t fixedCount = -1;  // -1 => not a fixed-size array spelling
    if (!baseName.empty() && baseName.back() == ']') {
        std::size_t lb = baseName.rfind('[');
        if (lb != std::string::npos) {
            std::string num = baseName.substr(lb + 1, baseName.size() - lb - 2);
            baseName = baseName.substr(0, lb);
            if (!num.empty()) {
                fixedCount = 0;
                for (char c : num) {
                    if (c < '0' || c > '9') { fixedCount = -1; break; }
                    fixedCount = fixedCount * 10 + (c - '0');
                }
            } else {
                fixedCount = 0;  // `T[]` (no count) -- treat as 0 elements
            }
        }
    }

    // Resolve the constructed class by its (element) type name. This is robust
    // against an enclosing cast (e.g. `cast<i32*>(new Cell(...))`) which would
    // otherwise make the sema result type reflect the cast target rather than T,
    // and against the array-size suffix being folded into the spelling.
    const Sema::ClassInfo* ci = nullptr;
    for (const auto& c : sema_.classes) {
        if (c.name == baseName) { ci = &c; break; }
    }
    if (!ci) ci = classInfoFor(elemTy);
    unsigned elemSz;
    if (ci) {
        // Compute the class size from its fields (struct-style layout), since a
        // cast may have hidden the element TypeRef.
        unsigned off = 0, maxAlign = 1;
        for (const auto& f : ci->fields) {
            unsigned fa = alignOf(f.second);
            if (fa == 0) fa = 1;
            off = alignUp(off, fa);
            off += sizeOf(f.second);
            maxAlign = std::max(maxAlign, fa);
        }
        elemSz = alignUp(off, maxAlign);
        if (elemSz == 0) elemSz = 1;
    } else {
        elemSz = elemTy ? sizeOf(elemTy) : 1;
        if (elemSz == 0) elemSz = 1;
    }

    bool isArray = (ne.arraySize != nullptr) || (fixedCount >= 0);

    // Element size in bytes (per element, even for the array form).
    VReg elemSzV = fn_->newVReg();
    emit({MOpcode::MovRI,
          {MOperand::defVReg(elemSzV),
           MOperand::immediate(static_cast<std::int64_t>(elemSz))}});

    if (arrayCountOut) {
        // A slice built from `new T[n]` wants the element count. For scalar `new T`
        // that count is 1.
        VReg c = fn_->newVReg();
        if (isArray) {
            // `count` is computed below for the array form; capture it lazily there.
            *arrayCountOut = kInvalidVReg;
        } else {
            emit({MOpcode::MovRI, {MOperand::defVReg(c), MOperand::immediate(1)}});
            *arrayCountOut = c;
        }
    }
    if (!isArray) {
        // Scalar `new T(...)`: allocate header + sizeOf(T), then construct in
        // place at the user pointer (base + 16).
        VReg total = fn_->newVReg();
        emit({MOpcode::MovRI,
              {MOperand::defVReg(total),
               MOperand::immediate(static_cast<std::int64_t>(elemSz + 16))}});
        VReg allocBase = emitMalloc(total);
        if (failed_) return kInvalidVReg;

        std::uint32_t resultSlot = fn_->addFrameSlot(8, 8, false);
        auto storeResult = [&](VReg v) {
            MInst st{MOpcode::Store, {MOperand::slot(resultSlot), MOperand::useVReg(v)}};
            st.width = 8;
            st.isSigned = false;
            emit(st);
        };
        auto loadResult = [&]() {
            VReg r = fn_->newVReg();
            MInst ld{MOpcode::Load, {MOperand::defVReg(r), MOperand::slot(resultSlot)}};
            ld.width = 8;
            ld.isSigned = false;
            emit(ld);
            return r;
        };
        VReg zero = fn_->newVReg();
        emit({MOpcode::MovRI, {MOperand::defVReg(zero), MOperand::immediate(0)}});
        storeResult(zero);

        std::uint32_t initB = fn_->addBlock();
        std::uint32_t doneB = fn_->addBlock();
        emit({MOpcode::Cmp, {MOperand::useVReg(allocBase), MOperand::useVReg(zero)}});
        {
            MInst jcc{MOpcode::Jcc, {MOperand::lbl(doneB)}};
            jcc.cond = Cond::EQ;
            emit(jcc);
        }
        emit({MOpcode::Jmp, {MOperand::lbl(initB)}});

        curBlock_ = initB;
        VReg one = fn_->newVReg();
        emit({MOpcode::MovRI, {MOperand::defVReg(one), MOperand::immediate(1)}});
        MInst stCount{MOpcode::StoreInd,
                      {MOperand::useVReg(allocBase), MOperand::immediate(0),
                       MOperand::useVReg(one)}};
        stCount.width = 8; stCount.isSigned = false;
        emit(stCount);
        MInst stTotal{MOpcode::StoreInd,
                      {MOperand::useVReg(allocBase), MOperand::immediate(8),
                       MOperand::useVReg(total)}};
        stTotal.width = 8; stTotal.isSigned = false;
        emit(stTotal);
        VReg ptr = fn_->newVReg();
        emit({MOpcode::LeaDisp,
              {MOperand::defVReg(ptr), MOperand::useVReg(allocBase),
               MOperand::immediate(16)}});
        if (ci) {
            if (!emitConstructorInto(ptr, ci, ne.arguments, elemSz))
                return kInvalidVReg;
        } else if (!ne.arguments.empty()) {
            // Non-class element with constructor-style args is meaningless.
            fail("selector: `new` with arguments requires a class type");
            return kInvalidVReg;
        }
        storeResult(ptr);
        emit({MOpcode::Jmp, {MOperand::lbl(doneB)}});

        curBlock_ = doneB;
        return loadResult();
    }

    // Array form. Determine the element count: the parsed count expression, or a
    // compile-time literal folded into the type spelling by legacy ASTs.
    VReg count = fn_->newVReg();
    if (ne.arraySize) {
        VReg n = selExpr(ne.arraySize);
        if (failed_) return kInvalidVReg;
        emit({MOpcode::MovRR, {MOperand::defVReg(count), MOperand::useVReg(n)}});
    } else {
        emit({MOpcode::MovRI,
              {MOperand::defVReg(count),
               MOperand::immediate(fixedCount < 0 ? 0 : fixedCount)}});
    }
    // Expose the element count to a slice initializer (`T[] s = new T[n]`). The
    // value stays live to the caller's use after this expression's blocks.
    if (arrayCountOut) *arrayCountOut = count;

    std::uint32_t resultSlot = fn_->addFrameSlot(8, 8, false);
    auto storeResult = [&](VReg v) {
        MInst st{MOpcode::Store, {MOperand::slot(resultSlot), MOperand::useVReg(v)}};
        st.width = 8;
        st.isSigned = false;
        emit(st);
    };
    auto loadResult = [&]() {
        VReg r = fn_->newVReg();
        MInst ld{MOpcode::Load, {MOperand::defVReg(r), MOperand::slot(resultSlot)}};
        ld.width = 8;
        ld.isSigned = false;
        emit(ld);
        return r;
    };
    VReg zero = fn_->newVReg();
    emit({MOpcode::MovRI, {MOperand::defVReg(zero), MOperand::immediate(0)}});
    storeResult(zero);

    std::uint32_t initB = fn_->addBlock();
    std::uint32_t doneB = fn_->addBlock();

    // payload = elemSz * count; total = payload + header. If either operation
    // wraps, leave the result as null and skip allocation/header writes.
    VReg payload = fn_->newVReg();
    emit({MOpcode::MovRI,
          {MOperand::defVReg(payload),
           MOperand::immediate(static_cast<std::int64_t>(elemSz))}});
    emit({MOpcode::IMul, {MOperand::useDefVReg(payload), MOperand::useVReg(count)}});
    VReg quotient = fn_->newVReg();
    MInst div{MOpcode::Div,
              {MOperand::defVReg(quotient), MOperand::useVReg(payload),
               MOperand::useVReg(elemSzV)}};
    div.isSigned = false;
    div.clobbers = {PhysReg::RAX, PhysReg::RDX};
    emit(div);
    emit({MOpcode::Cmp, {MOperand::useVReg(quotient), MOperand::useVReg(count)}});
    {
        MInst jcc{MOpcode::Jcc, {MOperand::lbl(doneB)}};
        jcc.cond = Cond::NE;
        emit(jcc);
    }
    VReg headerBytes = fn_->newVReg();
    emit({MOpcode::MovRI, {MOperand::defVReg(headerBytes), MOperand::immediate(16)}});
    VReg total = fn_->newVReg();
    emit({MOpcode::MovRR, {MOperand::defVReg(total), MOperand::useVReg(payload)}});
    emit({MOpcode::Add, {MOperand::useDefVReg(total), MOperand::useVReg(headerBytes)}});
    emit({MOpcode::Cmp, {MOperand::useVReg(total), MOperand::useVReg(payload)}});
    {
        MInst jcc{MOpcode::Jcc, {MOperand::lbl(doneB)}};
        jcc.cond = Cond::ULT;
        emit(jcc);
    }
    VReg allocBase = emitMalloc(total);
    if (failed_) return kInvalidVReg;
    emit({MOpcode::Cmp, {MOperand::useVReg(allocBase), MOperand::useVReg(zero)}});
    {
        MInst jcc{MOpcode::Jcc, {MOperand::lbl(doneB)}};
        jcc.cond = Cond::EQ;
        emit(jcc);
    }
    emit({MOpcode::Jmp, {MOperand::lbl(initB)}});

    curBlock_ = initB;

    MInst stCount{MOpcode::StoreInd,
                  {MOperand::useVReg(allocBase), MOperand::immediate(0),
                   MOperand::useVReg(count)}};
    stCount.width = 8; stCount.isSigned = false;
    emit(stCount);
    MInst stTotal{MOpcode::StoreInd,
                  {MOperand::useVReg(allocBase), MOperand::immediate(8),
                   MOperand::useVReg(total)}};
    stTotal.width = 8; stTotal.isSigned = false;
    emit(stTotal);
    VReg base = fn_->newVReg();
    emit({MOpcode::LeaDisp,
          {MOperand::defVReg(base), MOperand::useVReg(allocBase),
           MOperand::immediate(16)}});

    if (ci && !ci->constructorMangled.empty()) {
        // Construct each element: for (i = 0; i < count; ++i) ctor(base + i*elemSz)
        // Loop-invariant values are spilled to frame slots so they survive the
        // ctor call (which clobbers caller-saved registers). `i` is reloaded and
        // re-spilled each iteration.
        VReg i = fn_->newVReg();
        emit({MOpcode::MovRI, {MOperand::defVReg(i), MOperand::immediate(0)}});
        std::uint32_t baseSlot = fn_->addFrameSlot(8, 8, false);
        std::uint32_t countSlot = fn_->addFrameSlot(8, 8, false);
        std::uint32_t szSlot = fn_->addFrameSlot(8, 8, false);
        std::uint32_t iSlot = fn_->addFrameSlot(8, 8, false);
        auto spill = [&](std::uint32_t slot, VReg v) {
            MInst st{MOpcode::Store, {MOperand::slot(slot), MOperand::useVReg(v)}};
            st.width = 8; st.isSigned = false;
            emit(st);
        };
        auto reload = [&](std::uint32_t slot) {
            VReg r = fn_->newVReg();
            MInst ld{MOpcode::Load, {MOperand::defVReg(r), MOperand::slot(slot)}};
            ld.width = 8; ld.isSigned = false;
            emit(ld);
            return r;
        };
        spill(baseSlot, base);
        spill(countSlot, count);
        spill(szSlot, elemSzV);
        spill(iSlot, i);

        std::uint32_t headerB = fn_->addBlock();
        std::uint32_t bodyB = fn_->addBlock();
        std::uint32_t exitB = fn_->addBlock();
        emit({MOpcode::Jmp, {MOperand::lbl(headerB)}});

        curBlock_ = headerB;
        VReg iCur = reload(iSlot);
        VReg cnt = reload(countSlot);
        emit({MOpcode::Cmp, {MOperand::useVReg(iCur), MOperand::useVReg(cnt)}});
        MInst jcc{MOpcode::Jcc, {MOperand::lbl(exitB)}};
        jcc.cond = Cond::GE;  // i >= count -> done
        emit(jcc);
        emit({MOpcode::Jmp, {MOperand::lbl(bodyB)}});

        curBlock_ = bodyB;
        VReg iBody = reload(iSlot);
        VReg baseBody = reload(baseSlot);
        VReg szBody = reload(szSlot);
        // off = i * elemSz
        VReg off = fn_->newVReg();
        emit({MOpcode::MovRR, {MOperand::defVReg(off), MOperand::useVReg(iBody)}});
        emit({MOpcode::IMul, {MOperand::useDefVReg(off), MOperand::useVReg(szBody)}});
        // elem = base + off
        VReg elem = fn_->newVReg();
        emit({MOpcode::MovRR, {MOperand::defVReg(elem), MOperand::useVReg(baseBody)}});
        emit({MOpcode::Add, {MOperand::useDefVReg(elem), MOperand::useVReg(off)}});
        if (!emitConstructorInto(elem, ci, ne.arguments, elemSz))
            return kInvalidVReg;
        // ++i ; store back
        VReg iNext = reload(iSlot);
        VReg one = fn_->newVReg();
        emit({MOpcode::MovRI, {MOperand::defVReg(one), MOperand::immediate(1)}});
        emit({MOpcode::Add, {MOperand::useDefVReg(iNext), MOperand::useVReg(one)}});
        spill(iSlot, iNext);
        emit({MOpcode::Jmp, {MOperand::lbl(headerB)}});

        curBlock_ = exitB;
        // Reload the base pointer as the expression's result.
        storeResult(reload(baseSlot));
        emit({MOpcode::Jmp, {MOperand::lbl(doneB)}});
    } else if (ne.arguments.empty()) {
        storeResult(base);
        emit({MOpcode::Jmp, {MOperand::lbl(doneB)}});
    } else {
        fail("selector: `new T[n]` with constructor arguments requires class '" +
             baseName + "' to declare a constructor");
        return kInvalidVReg;
    }

    curBlock_ = doneB;
    return loadResult();
}

void InstructionSelector::selDelete(const AST::DeleteExpression& de) {
    if (!de.operand) return;

    Types::TypeRef ptrTy = concreteTypeOf(de.operand.get());
    Types::TypeRef elemTy =
        (ptrTy && ptrTy->kind == Types::Kind::Pointer) ? ptrTy->element : nullptr;
    if (!elemTy) {
        fail("selector: delete operand must be a pointer");
        return;
    }

    VReg ptr = selExpr(de.operand);
    if (failed_) return;

    // `delete null` is a no-op.
    std::uint32_t deleteB = fn_->addBlock();
    std::uint32_t doneB = fn_->addBlock();
    VReg zero = fn_->newVReg();
    emit({MOpcode::MovRI, {MOperand::defVReg(zero), MOperand::immediate(0)}});
    emit({MOpcode::Cmp, {MOperand::useVReg(ptr), MOperand::useVReg(zero)}});
    {
        MInst jcc{MOpcode::Jcc, {MOperand::lbl(doneB)}};
        jcc.cond = Cond::EQ;
        emit(jcc);
    }
    emit({MOpcode::Jmp, {MOperand::lbl(deleteB)}});

    curBlock_ = deleteB;
    std::uint32_t ptrSlot = fn_->addFrameSlot(8, 8, false);
    MInst st{MOpcode::Store, {MOperand::slot(ptrSlot), MOperand::useVReg(ptr)}};
    st.width = 8;
    st.isSigned = false;
    emit(st);

    VReg header = fn_->newVReg();
    emit({MOpcode::LeaDisp,
          {MOperand::defVReg(header), MOperand::useVReg(ptr),
           MOperand::immediate(-16)}});
    VReg count = fn_->newVReg();
    MInst ldCount{MOpcode::LoadInd,
                  {MOperand::defVReg(count), MOperand::useVReg(header),
                   MOperand::immediate(0)}};
    ldCount.width = 8; ldCount.isSigned = false;
    emit(ldCount);
    VReg total = fn_->newVReg();
    MInst ldTotal{MOpcode::LoadInd,
                  {MOperand::defVReg(total), MOperand::useVReg(header),
                   MOperand::immediate(8)}};
    ldTotal.width = 8; ldTotal.isSigned = false;
    emit(ldTotal);

    std::uint32_t headerSlot = fn_->addFrameSlot(8, 8, false);
    std::uint32_t totalSlot = fn_->addFrameSlot(8, 8, false);
    std::uint32_t countSlot = fn_->addFrameSlot(8, 8, false);
    auto spill8 = [&](std::uint32_t slot, VReg v) {
        MInst s{MOpcode::Store, {MOperand::slot(slot), MOperand::useVReg(v)}};
        s.width = 8; s.isSigned = false;
        emit(s);
    };
    auto reload8 = [&](std::uint32_t slot) {
        VReg r = fn_->newVReg();
        MInst l{MOpcode::Load, {MOperand::defVReg(r), MOperand::slot(slot)}};
        l.width = 8; l.isSigned = false;
        emit(l);
        return r;
    };
    spill8(headerSlot, header);
    spill8(totalSlot, total);
    spill8(countSlot, count);

    const Sema::ClassInfo* ci = classInfoFor(elemTy);
    if (ci && !ci->destructorMangled.empty()) {
        // Destruct arrays in reverse construction order:
        //   i = count; while (i != 0) { --i; dtor(ptr + i*elemSz); }
        VReg elemSzV = fn_->newVReg();
        emit({MOpcode::MovRI,
              {MOperand::defVReg(elemSzV),
               MOperand::immediate(static_cast<std::int64_t>(std::max(1u, sizeOf(elemTy))))}});
        std::uint32_t elemSzSlot = fn_->addFrameSlot(8, 8, false);
        spill8(elemSzSlot, elemSzV);

        std::uint32_t loopB = fn_->addBlock();
        std::uint32_t bodyB = fn_->addBlock();
        std::uint32_t afterDtorB = fn_->addBlock();
        emit({MOpcode::Jmp, {MOperand::lbl(loopB)}});

        curBlock_ = loopB;
        VReg iCur = reload8(countSlot);
        VReg zero2 = fn_->newVReg();
        emit({MOpcode::MovRI, {MOperand::defVReg(zero2), MOperand::immediate(0)}});
        emit({MOpcode::Cmp, {MOperand::useVReg(iCur), MOperand::useVReg(zero2)}});
        {
            MInst jcc{MOpcode::Jcc, {MOperand::lbl(afterDtorB)}};
            jcc.cond = Cond::EQ;
            emit(jcc);
        }
        emit({MOpcode::Jmp, {MOperand::lbl(bodyB)}});

        curBlock_ = bodyB;
        VReg iNext = reload8(countSlot);
        VReg one = fn_->newVReg();
        emit({MOpcode::MovRI, {MOperand::defVReg(one), MOperand::immediate(1)}});
        emit({MOpcode::Sub, {MOperand::useDefVReg(iNext), MOperand::useVReg(one)}});
        spill8(countSlot, iNext);

        VReg userPtr = reload8(ptrSlot);
        VReg elemSzReloaded = reload8(elemSzSlot);
        VReg off = fn_->newVReg();
        emit({MOpcode::MovRR, {MOperand::defVReg(off), MOperand::useVReg(iNext)}});
        emit({MOpcode::IMul, {MOperand::useDefVReg(off), MOperand::useVReg(elemSzReloaded)}});
        VReg elemPtr = fn_->newVReg();
        emit({MOpcode::MovRR, {MOperand::defVReg(elemPtr), MOperand::useVReg(userPtr)}});
        emit({MOpcode::Add, {MOperand::useDefVReg(elemPtr), MOperand::useVReg(off)}});
        emitDirectCall(ci->destructorMangled, {elemPtr}, {false}, {8},
                       /*resultTy=*/nullptr, /*sretResult=*/kInvalidVReg);
        if (failed_) return;
        emit({MOpcode::Jmp, {MOperand::lbl(loopB)}});

        curBlock_ = afterDtorB;
    }

    VReg freePtr = reload8(headerSlot);
    VReg freeSize = reload8(totalSlot);
    emitFree(freePtr, freeSize);
    if (!failed_) emit({MOpcode::Jmp, {MOperand::lbl(doneB)}});

    curBlock_ = doneB;
}

InstructionSelector::ElemAddr
InstructionSelector::computeLValueAddr(const AST::ExprAST* node) {
    // Resolves the address of an addressable lvalue as `base + disp`. Composes so
    // that `a[i].field`, `p.field`, `a[i]`, `*p`, and pointer identifiers all fold
    // their displacements into a single memory operand / lea.
    if (!node) { fail("selector: null lvalue"); return {}; }
    switch (node->nodeType()) {
        case AST::NodeType::MemberAccess: {
            const auto& m = static_cast<const AST::MemberAccessExpr&>(*node);
            if (m.computed) {
                Types::TypeRef elemTy = nullptr;
                return computeElementAddr(m, elemTy);
            }
            // `E.Variant` unit-variant value used as an aggregate lvalue.
            {
                const Sema::SumTypeInfo* st = nullptr;
                const Sema::SumVariant* v = nullptr;
                if (resolveSumVariant(m, st, v) && v) {
                    return materializeSumConstruct(*st, *v, {});
                }
            }
            // a.field : address of the aggregate + field byte offset.
            std::string field;
            if (m.property && m.property->nodeType() == AST::NodeType::IdentifierExpr) {
                field = static_cast<const AST::IdentifierExpr&>(*m.property).name;
            }
            Types::TypeRef objTy = concreteTypeOf(m.object.get());
            // Pointer-to-struct base: the pointer VALUE is the aggregate address.
            if (objTy && objTy->kind == Types::Kind::Pointer && objTy->element) {
                std::int64_t off = 0; Types::TypeRef ft = nullptr;
                if (!fieldOffsetOf(objTy->element, field, off, ft)) {
                    fail("selector: unknown field '" + field + "'");
                    return {};
                }
                VReg base = selExpr(m.object);
                if (failed_) return {};
                return ElemAddr{base, off};
            }
            // Value struct base: recurse for the aggregate's own address.
            std::int64_t off = 0; Types::TypeRef ft = nullptr;
            if (!fieldOffsetOf(objTy, field, off, ft)) {
                fail("selector: unknown field '" + field + "'");
                return {};
            }
            ElemAddr aggr = computeLValueAddr(m.object.get());
            if (failed_) return {};
            return ElemAddr{aggr.base, aggr.disp + off};
        }
        case AST::NodeType::DereferenceExpr: {
            // *p : the pointer value is the address.
            const auto& d = static_cast<const AST::DereferenceExpr&>(*node);
            VReg base = selExpr(d.operand);
            if (failed_) return {};
            return ElemAddr{base, 0};
        }
        case AST::NodeType::IdentifierExpr: {
            // The lvalue location of a variable. For a scalar or an inline struct
            // value, that is the variable's stack slot. For a struct parameter
            // (passed by hidden pointer), the slot holds a pointer to caller-owned
            // storage, so the address is the loaded pointer value.
            const auto& id = static_cast<const AST::IdentifierExpr&>(*node);
            LocalInfo li;
            if (!lookupLocal(id.name, li)) {
                // Module-level global: its lvalue is the symbol's address.
                if (const Sema::GlobalInfo* g = lookupGlobal(id.name)) {
                    return ElemAddr{globalAddr(*g), 0};
                }
                // A bare function name used as an address (e.g. `&wnd_proc`):
                // yield the function's own address via a RIP-relative LEA of its
                // symbol. This is what makes `&func` / `cast<u64>(&func)` produce
                // a callable function pointer, as required for C callbacks such
                // as a Win32 WNDPROC or any fnCall<...> target.
                for (const auto& fi : sema_.functions) {
                    if (fi.name != id.name && fi.mangledName != id.name) continue;
                    const std::string sym =
                        fi.mangledName.empty() ? fi.name : fi.mangledName;
                    VReg a = fn_->newVReg();
                    emit({MOpcode::Lea, {MOperand::defVReg(a), MOperand::sym(sym)}});
                    // Taking a function's address implies it may be called, so
                    // honor its link directives (lib(...)) as a direct call would.
                    (void)importDllFor(sym);
                    return ElemAddr{a, 0};
                }
                fail("selector: unknown lvalue '" + id.name + "'");
                return {};
            }
            if (li.kind == LocalKind::AggregatePtr) {
                VReg v = fn_->newVReg();
                MInst ld{MOpcode::Load, {MOperand::defVReg(v), MOperand::slot(li.slot)}};
                ld.width = 8; ld.isSigned = false;
                emit(ld);
                return ElemAddr{v, 0};
            }
            VReg v = fn_->newVReg();
            emit({MOpcode::LeaSlot, {MOperand::defVReg(v), MOperand::slot(li.slot)}});
            return ElemAddr{v, 0};
        }
        case AST::NodeType::FunctionCall: {
            // A call returning a struct yields the address of its result storage
            // (sret temp). Evaluating it produces that address directly.
            const auto& c = static_cast<const AST::FunctionCallExpr&>(*node);
            VReg addr = selCall(c);
            if (failed_) return {};
            return ElemAddr{addr, 0};
        }
        case AST::NodeType::StructInstantiation: {
            // A struct literal (Point{ ... }) is materialized into a fresh stack
            // temp; its address backs the aggregate rvalue.
            return materializeStructInstantiation(
                static_cast<const AST::StructInstantiation&>(*node));
        }
        case AST::NodeType::ArrayLiteral: {
            // An array literal [a, b, c] is materialized into a fresh stack temp;
            // its address backs the aggregate rvalue (copy-init source / &literal).
            return materializeArrayLiteral(
                static_cast<const AST::ArrayLiteral&>(*node));
        }
        case AST::NodeType::SliceExpr: {
            // A sub-slice `object[a..b]` materialized into a fresh { ptr, len }
            // header; its address backs the slice rvalue.
            return materializeSliceExpr(static_cast<const AST::SliceExpr&>(*node));
        }
        default:
            fail("selector: expression is not an addressable lvalue");
            return {};
    }
}

VReg InstructionSelector::selMemberLoad(const AST::MemberAccessExpr& m) {
    // a[i] / a.field / a[i].field (rvalue): load the element/field through
    // `[base + disp]`. The displacement (constant index scaling + field offset)
    // folds into the load's own memory operand.
    Types::TypeRef resultTy = concreteTypeOf(&m);
    ElemAddr a = computeLValueAddr(&m);
    if (failed_) return kInvalidVReg;
    if (isFloatType(resultTy)) {
        VReg v = fn_->newVReg(RegClass::XMM);
        emitFW(MOpcode::FLoadInd,
               {MOperand::defVReg(v), MOperand::useVReg(a.base), MOperand::immediate(a.disp)},
               floatWidthOf(resultTy));
        return v;
    }
    std::uint8_t w = static_cast<std::uint8_t>(widthOf(resultTy));
    bool sgn = isSignedOf(resultTy);
    VReg v = fn_->newVReg();
    MInst ld{MOpcode::LoadInd,
             {MOperand::defVReg(v), MOperand::useVReg(a.base), MOperand::immediate(a.disp)}};
    ld.width = w; ld.isSigned = sgn;
    emit(ld);
    return v;
}

std::string InstructionSelector::importDllFor(const std::string& name) const {
    std::string dll;
    for (const auto& fi : sema_.functions) {
        if (!fi.isExternal || !fi.decl) continue;
        // Extern functions keep their source name; match it (or the mangled one).
        if (fi.name != name && fi.mangledName != name) continue;
        // This extern is being referenced. Inspect its link directives:
        //   dll("lib.dll")  -> PE import table (first one wins; Windows only).
        //   lib("name")     -> ELF shared library to link (`-lname`), repeatable.
        for (const auto& attr : fi.decl->attributes) {
            if (attr.name == "dll" && !attr.value.empty() && dll.empty()) {
                dll = attr.value;
            } else if (attr.name == "lib" && !attr.value.empty()) {
                requiredLibs_.insert(attr.value);
            }
        }
        break;  // matched the referenced function; no other decl shares its name
    }
    return dll;
}

bool InstructionSelector::selIntrinsicCall(const std::string& name,
                                           const AST::NodeList& arguments,
                                           const AST::ExprAST& callNode,
                                           VReg& out) {
    out = kInvalidVReg;

    // volatileLoad<T>(ptr): a plain load that must not be optimized away. On
    // x86-64 a naturally-aligned mov is already a correct volatile load (we never
    // reorder/elide memory ops), so this lowers to a width-typed LoadInd.
    if (name == "volatileLoad") {
        if (arguments.empty()) { fail("selector: volatileLoad requires a pointer"); return true; }
        VReg ptr = selExpr(arguments[0]);
        if (failed_) return true;
        Types::TypeRef ty = concreteTypeOf(&callNode);
        VReg r = fn_->newVReg();
        MInst ld{MOpcode::LoadInd,
                 {MOperand::defVReg(r), MOperand::useVReg(ptr), MOperand::immediate(0)}};
        ld.width = static_cast<std::uint8_t>(widthOf(ty));
        ld.isSigned = isSignedOf(ty);
        emit(ld);
        out = r;
        return true;
    }

    // volatileStore(ptr, val): a plain store that must not be elided. Lowers to a
    // width-typed StoreInd (width taken from the stored value's type).
    if (name == "volatileStore") {
        if (arguments.size() < 2) { fail("selector: volatileStore requires (ptr, value)"); return true; }
        VReg ptr = selExpr(arguments[0]);
        if (failed_) return true;
        VReg val = selExpr(arguments[1]);
        if (failed_) return true;
        Types::TypeRef vty = concreteTypeOf(arguments[1].get());
        MInst st{MOpcode::StoreInd,
                 {MOperand::useVReg(ptr), MOperand::immediate(0), MOperand::useVReg(val)}};
        st.width = static_cast<std::uint8_t>(widthOf(vty));
        emit(st);
        return true;
    }

    // atomicLoad<T>(ptr): seq-cst load. An aligned mov already gives the right
    // ordering on x86-64, so this is a width-typed LoadInd.
    if (name == "atomicLoad") {
        if (arguments.empty()) { fail("selector: atomicLoad requires a pointer"); return true; }
        VReg ptr = selExpr(arguments[0]);
        if (failed_) return true;
        Types::TypeRef ty = concreteTypeOf(&callNode);
        VReg r = fn_->newVReg();
        MInst ld{MOpcode::LoadInd,
                 {MOperand::defVReg(r), MOperand::useVReg(ptr), MOperand::immediate(0)}};
        ld.width = static_cast<std::uint8_t>(widthOf(ty));
        ld.isSigned = isSignedOf(ty);
        emit(ld);
        out = r;
        return true;
    }

    // atomicStore(ptr, val): seq-cst store = plain store + mfence on x86-64.
    if (name == "atomicStore") {
        if (arguments.size() < 2) { fail("selector: atomicStore requires (ptr, value)"); return true; }
        VReg ptr = selExpr(arguments[0]);
        if (failed_) return true;
        VReg val = selExpr(arguments[1]);
        if (failed_) return true;
        Types::TypeRef vty = concreteTypeOf(arguments[1].get());
        MInst st{MOpcode::StoreInd,
                 {MOperand::useVReg(ptr), MOperand::immediate(0), MOperand::useVReg(val)}};
        st.width = static_cast<std::uint8_t>(widthOf(vty));
        emit(st);
        emit({MOpcode::Fence, {}});
        return true;
    }

    // atomicFence(): full sequential-consistency barrier (mfence).
    if (name == "atomicFence") {
        emit({MOpcode::Fence, {}});
        return true;
    }

    // atomicFetchAdd<T>(ptr, val): atomically *ptr += val, returns the old value
    // (lock xadd).
    if (name == "atomicFetchAdd") {
        if (arguments.size() < 2) { fail("selector: atomicFetchAdd requires (ptr, value)"); return true; }
        VReg ptr = selExpr(arguments[0]);
        if (failed_) return true;
        VReg val = selExpr(arguments[1]);
        if (failed_) return true;
        Types::TypeRef ty = concreteTypeOf(&callNode);
        VReg r = fn_->newVReg();
        MInst x{MOpcode::AtomicXAdd,
                {MOperand::defVReg(r), MOperand::useVReg(ptr),
                 MOperand::immediate(0), MOperand::useDefVReg(val)}};
        x.width = static_cast<std::uint8_t>(widthOf(ty));
        emit(x);
        out = r;
        return true;
    }

    // atomicCompareExchange(ptr, expected, desired): atomically set *ptr=desired
    // iff *ptr==expected; returns true on success (lock cmpxchg). The three inputs
    // are spilled to frame slots so lowering can pin expected in RAX with no
    // register-aliasing hazard.
    if (name == "atomicCompareExchange") {
        if (arguments.size() < 3) { fail("selector: atomicCompareExchange requires (ptr, expected, desired)"); return true; }
        VReg ptr = selExpr(arguments[0]);
        if (failed_) return true;
        VReg expected = selExpr(arguments[1]);
        if (failed_) return true;
        VReg desired = selExpr(arguments[2]);
        if (failed_) return true;
        Types::TypeRef vty = concreteTypeOf(arguments[1].get());
        std::uint8_t w = static_cast<std::uint8_t>(widthOf(vty));
        std::uint32_t ptrSlot = fn_->addFrameSlot(8, 8, /*isSpill=*/false);
        std::uint32_t expSlot = fn_->addFrameSlot(8, 8, /*isSpill=*/false);
        std::uint32_t desSlot = fn_->addFrameSlot(8, 8, /*isSpill=*/false);
        auto spill = [&](std::uint32_t slot, VReg v) {
            MInst st{MOpcode::Store, {MOperand::slot(slot), MOperand::useVReg(v)}};
            st.width = 8; st.isSigned = false;
            emit(st);
        };
        spill(ptrSlot, ptr);
        spill(expSlot, expected);
        spill(desSlot, desired);
        VReg ok = fn_->newVReg();
        MInst cx{MOpcode::AtomicCmpXchg,
                 {MOperand::defVReg(ok), MOperand::slot(expSlot),
                  MOperand::slot(ptrSlot), MOperand::slot(desSlot)}};
        cx.width = w;
        cx.clobbers = {PhysReg::RAX};
        emit(cx);
        out = ok;
        return true;
    }

    // asm[<RetType>]("template"[, "constraints", args...]): the custom backend
    // has no text assembler, so only a fixed set of bare-instruction templates is
    // supported (no `$0` operand substitution). The optional LLVM-style
    // constraint string binds the call's argument expressions to specific
    // physical registers (inputs), names the register that holds the result
    // (output), and lists clobbered registers. Examples (from the runtime):
    //   asm<i64>("syscall", "={rax},{rax},{rdi},~{rcx},~{r11},~{memory}", 12, 0)
    // Here `={rax}` is the output (result read from rax), `{rax},{rdi}` bind the
    // two trailing args (12, 0) to rax/rdi, and `~{rcx},~{r11},~{memory}` are
    // clobbers (`memory` is a fence-style marker we accept and ignore).
    if (name == "asm") {
        std::string templ;
        if (!arguments.empty()) {
            if (auto lit = AST::ast_cast<AST::StringLiteral>(arguments[0])) {
                templ = lit->value;
            }
        }
        std::string t;
        {
            std::size_t b = templ.find_first_not_of(" \t\r\n");
            std::size_t e = templ.find_last_not_of(" \t\r\n");
            if (b != std::string::npos) t = templ.substr(b, e - b + 1);
        }
        for (auto& c : t) c = static_cast<char>(std::tolower((unsigned char)c));
        std::int64_t sel = -1;
        if (t.empty() || t == "nop") sel = 0;
        else if (t == "syscall") sel = 1;
        else if (t == "int3") sel = 2;
        else if (t == "mfence") sel = 3;
        else if (t == "ud2") sel = 4;
        else if (t == "pause") sel = 5;
        else if (t == "cpuid") sel = 6;
        else if (t == "hlt") sel = 7;
        if (sel < 0) {
            fail("selector: asm template \"" + templ +
                 "\" is not supported by the custom backend (supported: nop, "
                 "syscall, int3, mfence, ud2, pause, cpuid, hlt)");
            return true;
        }

        // Map an LLVM register-class name to a PhysReg. Returns PhysReg::None for
        // names we don't model (e.g. "memory"/"cc" pseudo-clobbers).
        auto regByName = [](std::string n) -> PhysReg {
            for (auto& c : n) c = static_cast<char>(std::tolower((unsigned char)c));
            static const std::pair<const char*, PhysReg> kNames[] = {
                {"rax", PhysReg::RAX}, {"eax", PhysReg::RAX}, {"ax", PhysReg::RAX}, {"al", PhysReg::RAX},
                {"rbx", PhysReg::RBX}, {"ebx", PhysReg::RBX}, {"bx", PhysReg::RBX}, {"bl", PhysReg::RBX},
                {"rcx", PhysReg::RCX}, {"ecx", PhysReg::RCX}, {"cx", PhysReg::RCX}, {"cl", PhysReg::RCX},
                {"rdx", PhysReg::RDX}, {"edx", PhysReg::RDX}, {"dx", PhysReg::RDX}, {"dl", PhysReg::RDX},
                {"rsi", PhysReg::RSI}, {"esi", PhysReg::RSI}, {"si", PhysReg::RSI}, {"sil", PhysReg::RSI},
                {"rdi", PhysReg::RDI}, {"edi", PhysReg::RDI}, {"di", PhysReg::RDI}, {"dil", PhysReg::RDI},
                {"rbp", PhysReg::RBP}, {"rsp", PhysReg::RSP},
                {"r8", PhysReg::R8}, {"r9", PhysReg::R9}, {"r10", PhysReg::R10}, {"r11", PhysReg::R11},
                {"r12", PhysReg::R12}, {"r13", PhysReg::R13}, {"r14", PhysReg::R14}, {"r15", PhysReg::R15},
            };
            for (const auto& e : kNames) {
                if (n == e.first) return e.second;
            }
            return PhysReg::None;
        };

        // Parse the constraint string (argument[1]) into output / input / clobber
        // register bindings. Tokens are comma-separated. A leading '=' marks an
        // output, a leading '~' marks a clobber, and a bare `{reg}` is an input
        // bound (in order) to the call's trailing register arguments.
        std::string constraints;
        if (arguments.size() >= 2) {
            if (auto lit = AST::ast_cast<AST::StringLiteral>(arguments[1])) {
                constraints = lit->value;
            }
        }
        PhysReg outputReg = PhysReg::None;
        std::vector<PhysReg> inputRegs;
        std::vector<PhysReg> clobberRegs;
        {
            // Extract the register name inside the first {...} of a token, if any.
            auto braceReg = [&](const std::string& tok) -> PhysReg {
                std::size_t l = tok.find('{');
                std::size_t r = tok.find('}', l == std::string::npos ? 0 : l);
                if (l == std::string::npos || r == std::string::npos || r <= l + 1)
                    return PhysReg::None;
                return regByName(tok.substr(l + 1, r - l - 1));
            };
            std::size_t i = 0;
            while (i < constraints.size()) {
                std::size_t j = constraints.find(',', i);
                if (j == std::string::npos) j = constraints.size();
                std::string tok = constraints.substr(i, j - i);
                i = j + 1;
                // Trim whitespace.
                std::size_t b = tok.find_first_not_of(" \t\r\n");
                std::size_t e = tok.find_last_not_of(" \t\r\n");
                if (b == std::string::npos) continue;
                tok = tok.substr(b, e - b + 1);
                if (tok.empty()) continue;
                if (tok[0] == '~') {
                    PhysReg r = braceReg(tok);
                    if (r != PhysReg::None) clobberRegs.push_back(r);
                    // "~{memory}" / "~{cc}" are accepted and ignored.
                } else if (tok[0] == '=') {
                    PhysReg r = braceReg(tok);
                    if (r != PhysReg::None) outputReg = r;
                } else {
                    PhysReg r = braceReg(tok);
                    if (r != PhysReg::None) inputRegs.push_back(r);
                }
            }
        }

        // The trailing call arguments (after template + constraints) are the
        // input values, in the same order as the input constraints.
        std::vector<VReg> inputVRegs;
        for (std::size_t a = 2; a < arguments.size(); ++a) {
            VReg v = selExpr(arguments[a]);
            if (failed_) return true;
            inputVRegs.push_back(v);
        }
        if (inputVRegs.size() != inputRegs.size()) {
            fail("selector: asm input-operand count (" +
                 std::to_string(inputVRegs.size()) +
                 ") does not match the number of register input constraints (" +
                 std::to_string(inputRegs.size()) + ")");
            return true;
        }
        // Evaluate all inputs first (above), THEN move them into the bound
        // physical registers, so argument evaluation cannot clobber an
        // already-loaded input register.
        for (std::size_t k = 0; k < inputRegs.size(); ++k) {
            emit({MOpcode::MovRR,
                  {MOperand::defPhys(inputRegs[k]), MOperand::useVReg(inputVRegs[k])}});
        }

        MInst asmInst{MOpcode::AsmFixed, {MOperand::immediate(sel)}};
        // Record clobbers so the allocator/frame layout preserve them. The
        // template's own implicit clobbers (e.g. syscall trashes rcx/r11) are
        // already named explicitly via `~{...}` in the constraint string.
        asmInst.clobbers = clobberRegs;
        // An explicit output register is also defined by the instruction; make
        // sure the allocator treats it as clobbered until we copy it out.
        if (outputReg != PhysReg::None) asmInst.clobbers.push_back(outputReg);
        emit(asmInst);

        // If the call yields a value, copy the named output register into a vreg.
        if (outputReg != PhysReg::None) {
            VReg res = fn_->newVReg();
            emit({MOpcode::MovRR,
                  {MOperand::defVReg(res), MOperand::usePhys(outputReg)}});
            out = res;
        }
        return true;
    }

    return false;  // not an identifier-call intrinsic
}

const Sema::ClassInfo* InstructionSelector::classInfoFor(Types::TypeRef t) const {
    if (!t) return nullptr;
    // Accept a pointer-to-class as well as a value class type.
    Types::TypeRef ct = (t->kind == Types::Kind::Pointer && t->element) ? t->element : t;
    if (ct->kind != Types::Kind::Class) return nullptr;
    for (const auto& ci : sema_.classes) {
        if (ci.name == ct->name) return &ci;
    }
    return nullptr;
}

VReg InstructionSelector::emitDirectCall(const std::string& symbol,
                                         const std::vector<VReg>& argVRegs,
                                         const std::vector<bool>& argIsFloat,
                                         const std::vector<std::uint8_t>& argFloatW,
                                         Types::TypeRef resultTy, VReg sretResult,
                                         const AggregateAbi* retCls,
                                         VReg retReassembleAddr) {
    const auto& argRegs = abi_.intArgRegs;
    const auto& xmmArgRegs = abi_.xmmArgRegs;

    // Classify each argument into a destination register/stack slot, mirroring
    // selCall's placement (Win64 shares one positional cursor; System V uses two).
    enum class ArgDest { GP, XMM, Stack };
    struct ArgPlace { ArgDest dest; std::size_t reg; std::size_t stackIndex; };
    std::vector<ArgPlace> places(argVRegs.size());
    unsigned gpCursor = 0, xmmCursor = 0, stackCursor = 0;
    for (std::size_t i = 0; i < argVRegs.size(); ++i) {
        if (argIsFloat[i]) {
            unsigned xidx = abi_.sharedArgRegIndex ? gpCursor : xmmCursor;
            if (xidx < xmmArgRegs.size()) {
                places[i] = {ArgDest::XMM, xidx, 0};
                if (abi_.sharedArgRegIndex) ++gpCursor; else ++xmmCursor;
            } else {
                places[i] = {ArgDest::Stack, 0, stackCursor++};
                if (abi_.sharedArgRegIndex) ++gpCursor;
            }
        } else {
            if (gpCursor < argRegs.size()) {
                places[i] = {ArgDest::GP, gpCursor, 0};
                ++gpCursor;
                if (abi_.sharedArgRegIndex) ++xmmCursor;
            } else {
                places[i] = {ArgDest::Stack, 0, stackCursor++};
            }
        }
    }

    std::size_t stackArgs = stackCursor;
    if (stackArgs > 0) {
        std::int64_t bytes = static_cast<std::int64_t>(abi_.shadowSpace) +
                             8 * static_cast<std::int64_t>(stackArgs);
        fn_->noteOutgoingArgBytes(bytes);
    } else if (abi_.shadowSpace > 0) {
        fn_->noteOutgoingArgBytes(static_cast<std::int64_t>(abi_.shadowSpace));
    }

    for (std::size_t i = 0; i < argVRegs.size(); ++i) {
        if (places[i].dest != ArgDest::Stack) continue;
        std::int64_t off = static_cast<std::int64_t>(abi_.shadowSpace) +
                           8 * static_cast<std::int64_t>(places[i].stackIndex);
        if (argIsFloat[i]) {
            emitFW(MOpcode::FStoreOutgoing,
                   {MOperand::immediate(off), MOperand::useVReg(argVRegs[i])}, argFloatW[i]);
        } else {
            emit({MOpcode::StoreOutgoing,
                  {MOperand::immediate(off), MOperand::useVReg(argVRegs[i])}});
        }
    }
    for (std::size_t i = 0; i < argVRegs.size(); ++i) {
        if (places[i].dest == ArgDest::GP) {
            emit({MOpcode::MovRR,
                  {MOperand::defPhys(argRegs[places[i].reg]), MOperand::useVReg(argVRegs[i])}});
        } else if (places[i].dest == ArgDest::XMM) {
            emitFW(MOpcode::FMovRR,
                   {MOperand::defPhysXmm(xmmArgRegs[places[i].reg]),
                    MOperand::useVReg(argVRegs[i])}, argFloatW[i]);
        }
    }

    std::string importDll = importDllFor(symbol);
    MInst callInst =
        importDll.empty()
            ? MInst{MOpcode::Call, {MOperand::sym(symbol)}}
            : MInst{MOpcode::CallImport, {MOperand::sym(symbol), MOperand::sym(importDll)}};
    callInst.clobbers = callClobbers(abi_);
    emit(callInst);

    if (sretResult != kInvalidVReg) return sretResult;
    // Register-classified aggregate result: write the ABI return registers back
    // into caller-owned result storage and yield its address.
    if (retCls && !retCls->inMemory && retReassembleAddr != kInvalidVReg) {
        static const PhysReg kIntRet[2] = {PhysReg::RAX, PhysReg::RDX};
        static const XmmReg kSseRet[2] = {XmmReg::XMM0, XmmReg::XMM1};
        unsigned intIdx = 0, sseIdx = 0;
        for (std::size_t e = 0; e < retCls->eightbytes.size(); ++e) {
            const AbiEightbyte& eb = retCls->eightbytes[e];
            std::int64_t off = static_cast<std::int64_t>(e) * 8;
            if (eb.isSSE) {
                std::uint8_t fw = (eb.bytes <= 4) ? 4 : 8;
                VReg v = fn_->newVReg(RegClass::XMM);
                emitFW(MOpcode::FMovRR,
                       {MOperand::defVReg(v), MOperand::usePhysXmm(kSseRet[sseIdx++])}, fw);
                emitFW(MOpcode::FStoreInd,
                       {MOperand::useVReg(retReassembleAddr), MOperand::immediate(off),
                        MOperand::useVReg(v)}, fw);
            } else {
                std::uint8_t w = static_cast<std::uint8_t>(eb.bytes);
                if (w > 4) w = 8; else if (w > 2) w = 4; else if (w > 1) w = 2; else w = 1;
                VReg v = fn_->newVReg();
                emit({MOpcode::MovRR,
                      {MOperand::defVReg(v), MOperand::usePhys(kIntRet[intIdx++])}});
                MInst st{MOpcode::StoreInd,
                         {MOperand::useVReg(retReassembleAddr), MOperand::immediate(off),
                          MOperand::useVReg(v)}};
                st.width = w; st.isSigned = false;
                emit(st);
            }
        }
        return retReassembleAddr;
    }
    if (isFloatType(resultTy)) {
        if (isFloat16(resultTy)) {
            // The XMM return reg holds a packed half; expand to the f32 compute
            // form before handing the result to the caller.
            VReg res = fn_->newVReg(RegClass::XMM);
            emitFW(MOpcode::CvtF16ToF32,
                   {MOperand::defVReg(res), MOperand::usePhysXmm(abi_.xmmReturnReg)}, 4);
            return res;
        }
        VReg res = fn_->newVReg(RegClass::XMM);
        emitFW(MOpcode::FMovRR,
               {MOperand::defVReg(res), MOperand::usePhysXmm(abi_.xmmReturnReg)},
               floatWidthOf(resultTy));
        return res;
    }
    VReg res = fn_->newVReg();
    emit({MOpcode::MovRR, {MOperand::defVReg(res), MOperand::usePhys(abi_.intReturnReg)}});
    return res;
}

bool InstructionSelector::selMethodCall(const AST::FunctionCallExpr& call, VReg& out) {
    // Only a non-computed member-access callee (obj.method) can be a method call.
    if (!call.callee || call.callee->nodeType() != AST::NodeType::MemberAccess)
        return false;
    const auto& member = static_cast<const AST::MemberAccessExpr&>(*call.callee);
    if (member.computed) return false;  // a[i]() is not a method call
    if (!member.property || member.property->nodeType() != AST::NodeType::IdentifierExpr)
        return false;
    const std::string& methodName =
        static_cast<const AST::IdentifierExpr&>(*member.property).name;

    Types::TypeRef objTy = concreteTypeOf(member.object.get());
    const Sema::ClassInfo* ci = classInfoFor(objTy);
    if (!ci) return false;  // not a class object -> not a method call (e.g. a.field)

    // Committed to handling a method call: capture any pending elision destination
    // before evaluating the receiver/arguments (nested calls must not see it).
    const VReg elideDest = pendingAggResultDest_;
    pendingAggResultDest_ = kInvalidVReg;

    auto mit = ci->methodMangled.find(methodName);
    if (mit == ci->methodMangled.end()) {
        // A member access on a class that is not a method: treat as a field load
        // (let the normal path handle it). Returning false would re-enter selCall
        // which can't lower a field as a callee; this is a real error.
        fail("selector: class '" + ci->name + "' has no method '" + methodName + "'");
        out = kInvalidVReg;
        return true;
    }
    const std::string& mangled = mit->second;

    // Find the method's FunctionInfo for parameter typing (paramTypes[0] = this).
    const Sema::FunctionInfo* mInfo = nullptr;
    for (const auto& fi : sema_.functions) {
        if (fi.mangledName == mangled) { mInfo = &fi; break; }
    }

    // Compute the `this` pointer: if the object is already a pointer, its value
    // is `this`; otherwise take the object's lvalue address.
    VReg thisPtr;
    if (objTy && objTy->kind == Types::Kind::Pointer) {
        thisPtr = selExpr(member.object);
    } else {
        ElemAddr addr = computeLValueAddr(member.object.get());
        if (failed_) { out = kInvalidVReg; return true; }
        thisPtr = materializeAddr(addr);
    }
    if (failed_) { out = kInvalidVReg; return true; }

    // Build the argument list: this, then the explicit arguments. An aggregate
    // return uses sret: arg0 stays `this`? No — sret pointer precedes `this`.
    Types::TypeRef callTy = concreteTypeOf(&call);
    const bool sretCall = isAggregateType(callTy) || (callTy && callTy->kind == Types::Kind::Any);
    std::vector<VReg> argVRegs;
    std::vector<bool> argIsFloat;
    std::vector<std::uint8_t> argFloatW;
    std::vector<bool> argIsAggMem;
    VReg sretTempAddr = kInvalidVReg;
    AggregateAbi retCls;
    bool retInRegs = false;
    if (sretCall) {
        retCls = classifyAggregate(callTy);
        if (elideDest != kInvalidVReg) {
            // Return-value elision: build the result directly into the caller's
            // destination; no temporary, so nothing extra to destroy.
            sretTempAddr = elideDest;
        } else {
            // Discarded / subexpression result: own storage that must be destroyed
            // at statement end (registerTemporaryCleanup is a no-op unless callTy
            // has a destructor).
            unsigned sz = sizeOf(callTy), al = alignOf(callTy);
            std::uint32_t tmp = fn_->addFrameSlot(sz ? sz : 8, al ? al : 8, false);
            sretTempAddr = fn_->newVReg();
            emit({MOpcode::LeaSlot, {MOperand::defVReg(sretTempAddr), MOperand::slot(tmp)}});
            registerTemporaryCleanup(callTy, tmp);
        }
        if (retCls.inMemory) {
            argVRegs.push_back(sretTempAddr); argIsFloat.push_back(false);
            argFloatW.push_back(8); argIsAggMem.push_back(true);
        } else {
            retInRegs = true;
        }
    }
    argVRegs.push_back(thisPtr); argIsFloat.push_back(false); argFloatW.push_back(8);
    argIsAggMem.push_back(false);
    for (std::size_t ai = 0; ai < call.arguments.size(); ++ai) {
        const auto& arg = call.arguments[ai];
        Types::TypeRef aty = concreteTypeOf(arg.get());
        if (isFloat128(aty)) {
            fail("selector: f128 call ABI is not supported yet");
            out = kInvalidVReg;
            return true;
        }
        // Method paramTypes[0] is `this`; the explicit args start at index 1.
        Types::TypeRef expected =
            (mInfo && ai + 1 < mInfo->paramTypes.size()) ? mInfo->paramTypes[ai + 1]
                                                         : nullptr;
        if (isSliceType(aty) || isSliceType(expected)) {
            Types::TypeRef sliceTy = isSliceType(aty) ? aty : expected;
            appendSliceArgValue(sliceTy, arg, argVRegs, argIsFloat, argFloatW, argIsAggMem);
            if (failed_) { out = kInvalidVReg; return true; }
            continue;
        }
        if (aty && (aty->kind == Types::Kind::Struct || aty->kind == Types::Kind::Class)) {
            ElemAddr a = computeLValueAddr(arg.get());
            if (failed_) { out = kInvalidVReg; return true; }
            appendAggregateArgValues(aty, a, argVRegs, argIsFloat, argFloatW, argIsAggMem,
                                     arg.get());
            if (failed_) { out = kInvalidVReg; return true; }
            continue;
        }
        bool isF = isFloatType(aty);
        VReg av = selArg(arg);
        if (failed_) { out = kInvalidVReg; return true; }
        if (isFloat16(aty)) {
            // Pack the f32 compute value to a half in the XMM low 16 bits; the
            // callee expands it on entry. Marshalled as a 4-byte movss (which
            // copies the low 32 bits, including our packed half).
            VReg h = fn_->newVReg(RegClass::XMM);
            emitFW(MOpcode::CvtF32ToF16, {MOperand::defVReg(h), MOperand::useVReg(av)}, 4);
            av = h;
        }
        argVRegs.push_back(av);
        argIsFloat.push_back(isF);
        // f16 args were packed into the XMM low 16 bits above and are moved as a
        // 4-byte movss (the upper bits are zero), so report width 4 for them.
        argFloatW.push_back(isFloat16(aty) ? 4 : (isF ? floatWidthOf(aty) : 8));
        argIsAggMem.push_back(false);
    }
    (void)mInfo;
    // For a register-returned struct, emitDirectCall reassembles the ABI return
    // registers into our result temp when we pass sretReassemble=true.
    out = emitDirectCall(mangled, argVRegs, argIsFloat, argFloatW, callTy,
                         retInRegs ? kInvalidVReg : sretTempAddr,
                         retInRegs ? &retCls : nullptr,
                         retInRegs ? sretTempAddr : kInvalidVReg);
    return true;
}

const Sema::ClassInfo*
InstructionSelector::resolveConstructorCall(const AST::FunctionCallExpr& call) {
    // `T(...)` where T is a class name: the callee is an identifier whose sema
    // call type is that class (an aggregate).
    if (!call.callee || call.callee->nodeType() != AST::NodeType::IdentifierExpr)
        return nullptr;
    const std::string& name =
        static_cast<const AST::IdentifierExpr&>(*call.callee).name;

    Types::TypeRef callTy = concreteTypeOf(&call);

    // Generic constructor calls are written with the template name (`Box`) but
    // sema resolves them to a concrete class (`Box_i32`) and concrete ctor
    // symbol. Prefer that resolved class so the call gets an implicit `this`
    // argument instead of falling through to the ordinary free-call path.
    auto targetIt = sema_.callTargets.find(&call);
    if (targetIt != sema_.callTargets.end()) {
        if (const Sema::ClassInfo* resolved = classInfoFor(callTy)) {
            if (!resolved->constructorMangled.empty() &&
                targetIt->second == resolved->constructorMangled) {
                return resolved;
            }
        }
    }

    // Non-generic constructor call: source callee name is the class name.
    for (const auto& c : sema_.classes) {
        if (c.name == name) return &c;
    }
    return nullptr;
}

bool InstructionSelector::tryEmitConstructorInPlace(const AST::ExprAST* init,
                                                    VReg destAddr) {
    // Copy-elision for `T x = T(...)`: construct directly into the destination
    // rather than into a temporary that is then byte-copied and separately
    // destructed (which would run the destructor twice).
    if (!init || init->nodeType() != AST::NodeType::FunctionCall) return false;
    const auto& call = static_cast<const AST::FunctionCallExpr&>(*init);
    const Sema::ClassInfo* ci = resolveConstructorCall(call);
    if (!ci) return false;
    Types::TypeRef callTy = concreteTypeOf(&call);
    unsigned sz = sizeOf(callTy);
    if (sz == 0) sz = 8;
    emitConstructorInto(destAddr, ci, call.arguments, sz);
    return true;
}

bool InstructionSelector::emitAggregatePrvalueInPlace(const AST::NodePtr& init,
                                                      VReg destAddr) {
    // Constructor call: build directly into destAddr.
    if (tryEmitConstructorInPlace(init.get(), destAddr)) return true;
    // Non-constructor call returning an in-memory aggregate: thread destAddr as
    // the call's sret pointer so the result lands in the destination with no
    // intermediate temporary (and thus no leaked/double-destroyed husk). Small
    // register-returned aggregates (never a class with a destructor) fall through
    // to the caller's byte copy.
    if (init && init->nodeType() == AST::NodeType::FunctionCall) {
        Types::TypeRef rty = concreteTypeOf(init.get());
        if (rty && (rty->kind == Types::Kind::Struct || rty->kind == Types::Kind::Class) &&
            classifyAggregate(rty).inMemory) {
            pendingAggResultDest_ = destAddr;
            selExpr(init);
            pendingAggResultDest_ = kInvalidVReg;  // defensive; selCall also clears
            return true;
        }
    }
    return false;
}

bool InstructionSelector::selConstructorCall(const AST::FunctionCallExpr& call, VReg& out) {
    // `T(...)` where T is a class name: allocate a temp, run the ctor (if
    // declared) with this = temp address, and yield the temp's address.
    const Sema::ClassInfo* ci = resolveConstructorCall(call);
    if (!ci) return false;
    // Constructor elision is driven by tryEmitConstructorInPlace, not by the
    // pending-destination channel; drop any pending destination so it cannot leak
    // into this or a later call.
    pendingAggResultDest_ = kInvalidVReg;
    Types::TypeRef callTy = concreteTypeOf(&call);

    unsigned sz = sizeOf(callTy), al = alignOf(callTy);
    if (sz == 0) sz = 8;
    std::uint32_t tmp = fn_->addFrameSlot(sz, al ? al : 8, false);
    VReg objAddr = fn_->newVReg();
    emit({MOpcode::LeaSlot, {MOperand::defVReg(objAddr), MOperand::slot(tmp)}});

    if (!emitConstructorInto(objAddr, ci, call.arguments, sz)) {
        out = kInvalidVReg;
        return true;
    }
    registerTemporaryCleanup(callTy, tmp);
    out = objAddr;  // the aggregate value is its storage address
    return true;
}

bool InstructionSelector::emitConstructorInto(VReg thisAddr,
                                              const Sema::ClassInfo* ci,
                                              const AST::NodeList& arguments,
                                              unsigned objSize) {
    if (ci && !ci->constructorMangled.empty()) {
        std::vector<VReg> argVRegs{thisAddr};
        std::vector<bool> argIsFloat{false};
        std::vector<std::uint8_t> argFloatW{8};
        std::vector<bool> argIsAggMem{false};
        for (std::size_t ai = 0; ai < arguments.size(); ++ai) {
            const auto& arg = arguments[ai];
            Types::TypeRef aty = concreteTypeOf(arg.get());
            if (isFloat128(aty)) {
                fail("selector: f128 call ABI is not supported yet");
                return false;
            }
            Types::TypeRef expected =
                (ci && ai < ci->constructorParams.size()) ? ci->constructorParams[ai]
                                                          : nullptr;
            if (isSliceType(aty) || isSliceType(expected)) {
                Types::TypeRef sliceTy = isSliceType(aty) ? aty : expected;
                appendSliceArgValue(sliceTy, arg, argVRegs, argIsFloat, argFloatW, argIsAggMem);
                if (failed_) return false;
                continue;
            }
            if (aty && (aty->kind == Types::Kind::Struct || aty->kind == Types::Kind::Class)) {
                ElemAddr a = computeLValueAddr(arg.get());
                if (failed_) return false;
                appendAggregateArgValues(aty, a, argVRegs, argIsFloat, argFloatW, argIsAggMem,
                                         arg.get());
                if (failed_) return false;
                continue;
            }
            bool isF = isFloatType(aty);
            VReg av = selArg(arg);
            if (failed_) return false;
            if (isFloat16(aty)) {
                VReg h = fn_->newVReg(RegClass::XMM);
                emitFW(MOpcode::CvtF32ToF16, {MOperand::defVReg(h), MOperand::useVReg(av)}, 4);
                av = h;
            }
            argVRegs.push_back(av);
            argIsFloat.push_back(isF);
            argFloatW.push_back(isFloat16(aty) ? 4 : (isF ? floatWidthOf(aty) : 8));
            argIsAggMem.push_back(false);
            if (failed_) return false;
        }
        // Constructor returns void; ignore its result. `this` (thisAddr) is the value.
        emitDirectCall(ci->constructorMangled, argVRegs, argIsFloat, argFloatW,
                       /*resultTy=*/nullptr, /*sretResult=*/kInvalidVReg);
        return !failed_;
    }
    // No declared constructor: passing arguments is a selection error (there is
    // nothing to receive them). With no arguments, zero-initialize the storage
    // so fields start defined.
    if (!arguments.empty()) {
        fail("selector: class '" + (ci ? ci->name : std::string("?")) +
             "' has no constructor but " + std::to_string(arguments.size()) +
             " argument(s) were passed");
        return false;
    }
    VReg zero = fn_->newVReg();
    emit({MOpcode::MovRI, {MOperand::defVReg(zero), MOperand::immediate(0)}});
    for (unsigned off = 0; off + 8 <= objSize; off += 8) {
        MInst st{MOpcode::StoreInd,
                 {MOperand::useVReg(thisAddr), MOperand::immediate((std::int64_t)off),
                  MOperand::useVReg(zero)}};
        st.width = 8; st.isSigned = false;
        emit(st);
    }
    for (unsigned off = (objSize / 8) * 8; off < objSize; ++off) {
        MInst st{MOpcode::StoreInd,
                 {MOperand::useVReg(thisAddr), MOperand::immediate((std::int64_t)off),
                  MOperand::useVReg(zero)}};
        st.width = 1; st.isSigned = false;
        emit(st);
    }
    return true;
}

bool InstructionSelector::selClassOperator(const std::string& op, const AST::NodePtr& l,
                                           const AST::NodePtr& r, VReg& out) {
    Types::TypeRef lt = concreteTypeOf(l.get());
    const Sema::ClassInfo* ci = classInfoFor(lt);
    if (!ci) return false;  // lhs is not a class -> primitive op
    auto oit = ci->operatorMangled.find(op);
    if (oit == ci->operatorMangled.end()) return false;  // no overload for this op
    const std::string& mangled = oit->second;

    // this = address of lhs object (value class) or its pointer value.
    VReg thisPtr;
    if (lt && lt->kind == Types::Kind::Pointer) {
        thisPtr = selExpr(l);
    } else {
        ElemAddr addr = computeLValueAddr(l.get());
        if (failed_) { out = kInvalidVReg; return true; }
        thisPtr = materializeAddr(addr);
    }
    if (failed_) { out = kInvalidVReg; return true; }

    std::vector<VReg> argVRegs{thisPtr};
    std::vector<bool> argIsFloat{false};
    std::vector<std::uint8_t> argFloatW{8};
    std::vector<bool> argIsAggMem{false};
    Types::TypeRef rty = concreteTypeOf(r.get());
    if (isFloat128(rty)) {
        fail("selector: f128 call ABI is not supported yet");
        out = kInvalidVReg;
        return true;
    }
    if (rty && (rty->kind == Types::Kind::Struct || rty->kind == Types::Kind::Class)) {
        ElemAddr a = computeLValueAddr(r.get());
        if (failed_) { out = kInvalidVReg; return true; }
        appendAggregateArgValues(rty, a, argVRegs, argIsFloat, argFloatW, argIsAggMem,
                                 r.get());
    } else {
        bool isF = isFloatType(rty);
        VReg av = selArg(r);
        if (failed_) { out = kInvalidVReg; return true; }
        if (isFloat16(rty)) {
            VReg h = fn_->newVReg(RegClass::XMM);
            emitFW(MOpcode::CvtF32ToF16, {MOperand::defVReg(h), MOperand::useVReg(av)}, 4);
            av = h;
        }
        argVRegs.push_back(av);
        argIsFloat.push_back(isF);
        argFloatW.push_back(isFloat16(rty) ? 4 : (isF ? floatWidthOf(rty) : 8));
        argIsAggMem.push_back(false);
    }
    if (failed_) { out = kInvalidVReg; return true; }

    // The operator's result type is the call/binary expression's type. Use the
    // operator method's recorded return type via its FunctionInfo when available.
    Types::TypeRef resultTy = nullptr;
    for (const auto& fi : sema_.functions) {
        if (fi.mangledName == mangled) { resultTy = fi.returnType; break; }
    }
    out = emitDirectCall(mangled, argVRegs, argIsFloat, argFloatW, resultTy, kInvalidVReg);
    return true;
}

VReg InstructionSelector::selCall(const AST::FunctionCallExpr& call) {
    // Identifier-call intrinsics (no `@`): volatileLoad/Store, atomic*, asm.
    if (call.callee &&
        call.callee->nodeType() == AST::NodeType::IdentifierExpr) {
        const std::string& cn =
            static_cast<const AST::IdentifierExpr&>(*call.callee).name;
        if (cn == "volatileLoad" || cn == "volatileStore" || cn == "atomicLoad" ||
            cn == "atomicStore" || cn == "atomicFence" || cn == "atomicFetchAdd" ||
            cn == "atomicCompareExchange" || cn == "asm") {
            VReg out = kInvalidVReg;
            selIntrinsicCall(cn, call.arguments, call, out);
            return out;
        }
    }

    // Sum-type variant construction `E.Variant(args)`: build the tagged aggregate.
    if (call.callee && call.callee->nodeType() == AST::NodeType::MemberAccess) {
        const auto& m = static_cast<const AST::MemberAccessExpr&>(*call.callee);
        const Sema::SumTypeInfo* st = nullptr;
        const Sema::SumVariant* v = nullptr;
        if (resolveSumVariant(m, st, v) && v) {
            VReg dest = pendingAggResultDest_;
            pendingAggResultDest_ = kInvalidVReg;
            ElemAddr a = materializeSumConstruct(*st, *v, call.arguments, dest);
            if (failed_) return kInvalidVReg;
            return materializeAddr(a);
        }
    }

    // Class constructor call form `T(...)`: the callee is an identifier that
    // names a class. Allocate a temp, run the constructor (if any), yield the
    // temp's address as the (aggregate) value.
    {
        VReg out = kInvalidVReg;
        if (selConstructorCall(call, out)) return out;
    }

    // Method call `obj.method(...)`: the callee is a (non-computed) member
    // access whose object has class type. Resolve via sema and emit with `this`.
    {
        VReg out = kInvalidVReg;
        if (selMethodCall(call, out)) return out;
    }

    // Free-function call. Capture any pending elision destination now, before
    // evaluating the indirect target / arguments (nested calls must not see it).
    const VReg elideDest = pendingAggResultDest_;
    pendingAggResultDest_ = kInvalidVReg;

    // Indirect call via the `fnCall<Ret>(target, arg0, arg1, ...)` intrinsic:
    // arguments[0] is the callee ADDRESS (a u64/pointer value), arguments[1..] are
    // the real call arguments, and the result type is sema's type for the call
    // (the <Ret> generic, or void). We materialize the target into its own frame
    // slot up front so argument-register marshalling cannot clobber it, then emit
    // a `call qword[rbp+slot]`.
    const bool isFnCallIntrinsic =
        call.callee && call.callee->nodeType() == AST::NodeType::IdentifierExpr &&
        static_cast<const AST::IdentifierExpr&>(*call.callee).name == "fnCall";

    // Calling a function-typed local variable (e.g. a lambda stored in `auto f`):
    // `f(args)` loads the pointer value from `f` and calls through it. Unlike
    // fnCall, every argument is a real call argument (no target arg to skip).
    bool isFnVarCall = false;
    VReg closureEnv = kInvalidVReg;  // env ptr for closure calls
    if (!isFnCallIntrinsic && call.callee &&
        call.callee->nodeType() == AST::NodeType::IdentifierExpr) {
        LocalInfo li;
        const std::string& cn =
            static_cast<const AST::IdentifierExpr&>(*call.callee).name;
        if (lookupLocal(cn, li) && li.type) {
            if (li.type->kind == Types::Kind::Function ||
                li.type->kind == Types::Kind::Closure) {
                isFnVarCall = true;
                if (li.type->kind == Types::Kind::Closure) {
                    closureEnv = selExpr(call.callee);
                    if (failed_) return kInvalidVReg;
                }
            }
        }
    }

    const bool isIndirect = isFnCallIntrinsic || isFnVarCall;

    std::uint32_t indirectTargetSlot = 0;
    if (isIndirect) {
        VReg targetV;
        if (isFnCallIntrinsic) {
            if (call.arguments.empty()) {
                fail("selector: fnCall requires a target address");
                return kInvalidVReg;
            }
            targetV = selExpr(call.arguments[0]);
        } else {
            targetV = selExpr(call.callee);
        }
        if (failed_) return kInvalidVReg;
        if (closureEnv != kInvalidVReg) {
            // Closure: load fn_ptr from [closure_ptr + 0]
            VReg fnPtr = fn_->newVReg();
            MInst ld{MOpcode::LoadInd,
                     {MOperand::defVReg(fnPtr), MOperand::useVReg(closureEnv),
                      MOperand::immediate(0)}};
            ld.width = 8; ld.isSigned = false;
            emit(ld);
            targetV = fnPtr;
        }
        indirectTargetSlot = fn_->addFrameSlot(8, 8, /*isSpill=*/false);
        MInst st{MOpcode::Store,
                 {MOperand::slot(indirectTargetSlot), MOperand::useVReg(targetV)}};
        st.width = 8; st.isSigned = false;
        emit(st);
    }

    // Resolve the callee symbol. Prefer the sema-resolved mangled target
    // (populated for generic instantiations / constructor calls). Otherwise, for
    // an ordinary free-function call, resolve the callee's mangled symbol by
    // looking its source name up in the analyzed function table; if it is not
    // found there (e.g. a runtime/extern symbol), fall back to the raw name.
    std::string target;
    auto it = sema_.callTargets.find(&call);
    if (isIndirect) {
        // target resolved to a frame slot above; no symbol needed.
    } else if (it != sema_.callTargets.end()) {
        target = it->second;
    } else if (call.callee &&
               call.callee->nodeType() == AST::NodeType::IdentifierExpr) {
        const std::string& srcName =
            static_cast<const AST::IdentifierExpr&>(*call.callee).name;
        target = srcName;
        for (const auto& fi : sema_.functions) {
            if (fi.name == srcName && !fi.mangledName.empty()) {
                target = fi.mangledName;
                break;
            }
        }
    } else {
        fail("selector: unresolved call target");
        return kInvalidVReg;
    }

    // Struct return-by-value: allocate result storage in our frame and pass its
    // address as a hidden first argument. The call's value is that address (so
    // the result can be used as a struct lvalue: f().field, passing f() onward).
    Types::TypeRef callTy = concreteTypeOf(&call);
    const bool sretCall = isAggregateType(callTy) || (callTy && callTy->kind == Types::Kind::Any);
    VReg sretTempAddr = kInvalidVReg;
    AggregateAbi retCls;
    bool retInRegs = false;  // struct returned in registers (writeback after call)

    // Expected parameter types for the resolved callee (used to wrap contextual
    // slice arguments such as an array or `new T[n]` passed to a `T[]` param).
    const std::vector<Types::TypeRef>* calleeParams = nullptr;
    if (!isIndirect && !target.empty()) {
        for (const auto& fi : sema_.functions) {
            const std::string& sym = fi.mangledName.empty() ? fi.name : fi.mangledName;
            if (sym == target) { calleeParams = &fi.paramTypes; break; }
        }
    }

    // Evaluate arguments left-to-right into vregs. A struct argument lowers to a
    // hidden pointer (the address of its storage). `argIsFloat` records whether
    // each evaluated arg is a double (passed in an XMM register).
    std::vector<VReg> argVRegs;
    std::vector<bool> argIsFloat;
    std::vector<std::uint8_t> argFloatW;  // float precision (4/8) per arg
    std::vector<bool> argIsAggMem;        // per value: in-memory aggregate pointer?
    if (sretCall) {
        if ((callTy->kind == Types::Kind::Struct ||
             callTy->kind == Types::Kind::Class) && !structInfoFor(callTy)) {
            fail("selector: unknown struct return type for call");
            return kInvalidVReg;
        }
        // The call's value is a storage address (so the result can be used as a
        // struct lvalue: f().field, passed onward, etc.). When the caller provided
        // an elision destination, build directly into it; otherwise allocate a
        // result temp and register it for destruction so a discarded class result
        // is not leaked (registerTemporaryCleanup is a no-op for non-destructor
        // types).
        retCls = classifyAggregate(callTy);
        if (elideDest != kInvalidVReg) {
            sretTempAddr = elideDest;
        } else {
            unsigned sz = sizeOf(callTy), al = alignOf(callTy);
            std::uint32_t tmp = fn_->addFrameSlot(sz ? sz : 8, al ? al : 8, /*isSpill=*/false);
            sretTempAddr = fn_->newVReg();
            emit({MOpcode::LeaSlot, {MOperand::defVReg(sretTempAddr), MOperand::slot(tmp)}});
            registerTemporaryCleanup(callTy, tmp);
        }
        if (retCls.inMemory) {
            // Memory return: pass the storage address as a hidden first argument.
            argVRegs.push_back(sretTempAddr);
            argIsFloat.push_back(false);
            argFloatW.push_back(8);
            argIsAggMem.push_back(true);
        } else {
            // Register return: registers are written back into the temp after
            // the call (no hidden pointer argument).
            retInRegs = true;
        }
    }
    if (closureEnv != kInvalidVReg) {
        argVRegs.push_back(closureEnv);
        argIsFloat.push_back(false);
        argFloatW.push_back(8);
        argIsAggMem.push_back(false);
    }
    for (std::size_t ai = 0; ai < call.arguments.size(); ++ai) {
        // For fnCall, arguments[0] is the target address (already materialized),
        // not a call argument; the real arguments start at index 1. A
        // function-typed-variable call passes every argument normally.
        if (isFnCallIntrinsic && ai == 0) continue;
        const auto& arg = call.arguments[ai];
        Types::TypeRef aty = concreteTypeOf(arg.get());
        if (isFloat128(aty)) {
            fail("selector: f128 call ABI is not supported yet");
            return kInvalidVReg;
        }
        // Slice parameter: pass a 16-byte { ptr, len } header. The argument may be
        // an existing slice value or a contextual array / `new T[n]`.
        Types::TypeRef expected =
            (calleeParams && ai < calleeParams->size()) ? (*calleeParams)[ai] : nullptr;
        if (isSliceType(aty) || isSliceType(expected)) {
            Types::TypeRef sliceTy = isSliceType(aty) ? aty : expected;
            appendSliceArgValue(sliceTy, arg, argVRegs, argIsFloat, argFloatW, argIsAggMem);
            if (failed_) return kInvalidVReg;
            continue;
        }
        if (aty && (aty->kind == Types::Kind::Struct || aty->kind == Types::Kind::Class)) {
            ElemAddr a = computeLValueAddr(arg.get());
            if (failed_) return kInvalidVReg;
            appendAggregateArgValues(aty, a, argVRegs, argIsFloat, argFloatW, argIsAggMem,
                                     arg.get());
            if (failed_) return kInvalidVReg;
            continue;
        }
        // 128-bit integer argument. SysV: two GP words (low, high). Win64: passed
        // by reference -- materialize the value into a temp and pass its address.
        if (isInt128(aty)) {
            VReg valAddr = selExpr(arg);  // address of the 16-byte value
            if (failed_) return kInvalidVReg;
            if (abi_.abi == Abi::Win64) {
                argVRegs.push_back(valAddr);  // pointer to the 16-byte copy
                argIsFloat.push_back(false);
                argFloatW.push_back(8);
                argIsAggMem.push_back(true);
            } else {
                VReg lo = loadHalf(valAddr, 0);
                VReg hi = loadHalf(valAddr, 8);
                argVRegs.push_back(lo);
                argIsFloat.push_back(false);
                argFloatW.push_back(8);
                argIsAggMem.push_back(false);
                argVRegs.push_back(hi);
                argIsFloat.push_back(false);
                argFloatW.push_back(8);
                argIsAggMem.push_back(false);
            }
            continue;
        }
        bool isF = isFloatType(aty);
        VReg av = selArg(arg);
        if (failed_) return kInvalidVReg;
        if (isFloat16(aty)) {
            VReg h = fn_->newVReg(RegClass::XMM);
            emitFW(MOpcode::CvtF32ToF16, {MOperand::defVReg(h), MOperand::useVReg(av)}, 4);
            av = h;
        }
        argVRegs.push_back(av);
        argIsFloat.push_back(isF);
        argFloatW.push_back(isFloat16(aty) ? 4 : (isF ? floatWidthOf(aty) : 8));
        argIsAggMem.push_back(false);
        if (failed_) return kInvalidVReg;
    }

    const auto& argRegs = abi_.intArgRegs;
    const auto& xmmArgRegs = abi_.xmmArgRegs;

    // Classify each argument into a destination: a GP arg register, an XMM arg
    // register, or a stack slot. Win64 shares one positional cursor between the
    // GP and XMM register files; System V advances them independently.
    enum class ArgDest { GP, XMM, Stack };
    struct ArgPlace { ArgDest dest; std::size_t reg; std::size_t stackIndex; };
    std::vector<ArgPlace> places(argVRegs.size());
    unsigned gpCursor = 0, xmmCursor = 0, stackCursor = 0;
    for (std::size_t i = 0; i < argVRegs.size(); ++i) {
        if (argIsFloat[i]) {
            unsigned xidx = abi_.sharedArgRegIndex ? gpCursor : xmmCursor;
            if (xidx < xmmArgRegs.size()) {
                places[i] = {ArgDest::XMM, xidx, 0};
                if (abi_.sharedArgRegIndex) ++gpCursor; else ++xmmCursor;
            } else {
                places[i] = {ArgDest::Stack, 0, stackCursor++};
                if (abi_.sharedArgRegIndex) ++gpCursor;
            }
        } else {
            if (gpCursor < argRegs.size()) {
                places[i] = {ArgDest::GP, gpCursor, 0};
                ++gpCursor;
                if (abi_.sharedArgRegIndex) ++xmmCursor;
            } else {
                places[i] = {ArgDest::Stack, 0, stackCursor++};
            }
        }
    }

    // Arguments that did not get a register are passed on the stack. The ABI
    // requires the k-th stack arg at [rsp + shadow + 8*k] at the moment of the
    // call (shadow space precedes them on Win64). We reserve this region once in
    // the frame (push callee-saved before `sub rsp`, so rsp is stable at the
    // frame bottom) and store each stack arg there before the call.
    std::size_t stackArgs = stackCursor;
    if (stackArgs > 0) {
        std::int64_t bytes = static_cast<std::int64_t>(abi_.shadowSpace) +
                             8 * static_cast<std::int64_t>(stackArgs);
        fn_->noteOutgoingArgBytes(bytes);
    } else if (abi_.shadowSpace > 0) {
        // Win64 still requires shadow space reserved even for register-only calls.
        fn_->noteOutgoingArgBytes(static_cast<std::int64_t>(abi_.shadowSpace));
    }

    // Store stack-passed args first (they may use scratch/regs freely), then load
    // register args last so the argument registers are not clobbered in between.
    for (std::size_t i = 0; i < argVRegs.size(); ++i) {
        if (places[i].dest != ArgDest::Stack) continue;
        std::int64_t off = static_cast<std::int64_t>(abi_.shadowSpace) +
                           8 * static_cast<std::int64_t>(places[i].stackIndex);
        if (argIsFloat[i]) {
            emitFW(MOpcode::FStoreOutgoing,
                   {MOperand::immediate(off), MOperand::useVReg(argVRegs[i])}, argFloatW[i]);
        } else {
            emit({MOpcode::StoreOutgoing,
                  {MOperand::immediate(off), MOperand::useVReg(argVRegs[i])}});
        }
    }

    // Move each register argument into its ABI register.
    for (std::size_t i = 0; i < argVRegs.size(); ++i) {
        if (places[i].dest == ArgDest::GP) {
            emit({MOpcode::MovRR,
                  {MOperand::defPhys(argRegs[places[i].reg]), MOperand::useVReg(argVRegs[i])}});
        } else if (places[i].dest == ArgDest::XMM) {
            emitFW(MOpcode::FMovRR,
                   {MOperand::defPhysXmm(xmmArgRegs[places[i].reg]),
                    MOperand::useVReg(argVRegs[i])}, argFloatW[i]);
        }
    }

    std::string importDll = isIndirect ? std::string() : importDllFor(target);
    MInst callInst =
        isIndirect
            ? MInst{MOpcode::CallIndirect, {MOperand::slot(indirectTargetSlot)}}
        : importDll.empty()
            ? MInst{MOpcode::Call, {MOperand::sym(target)}}
            : MInst{MOpcode::CallImport,
                    {MOperand::sym(target), MOperand::sym(importDll)}};
    if (isIndirect) {
        // x86 calls through an address and needs nothing further, but wasm's
        // call_indirect names the callee's type, so record enough of the shape
        // here for a backend to rebuild it: whether a result comes back, then one
        // flag per argument saying which register file carried it. Without this
        // the signature is unrecoverable once selection has finished.
        const bool hasResult = callTy && !callTy->isVoid();
        callInst.operands.push_back(MOperand::immediate(hasResult ? 1 : 0));
        for (std::size_t i = 0; i < argVRegs.size(); ++i) {
            callInst.operands.push_back(MOperand::immediate(argIsFloat[i] ? 1 : 0));
        }
    }
    callInst.clobbers = callClobbers(abi_);
    emit(callInst);

    // For an sret call the canonical result is our frame temp (its address is the
    // call's value). A register-returned struct is reassembled into that temp from
    // the ABI return registers; a memory-returned struct was already written there
    // by the callee via the hidden pointer.
    if (sretCall) {
        if (retInRegs) {
            // System V: INTEGER eightbytes come back in RAX then RDX; SSE in
            // XMM0 then XMM1. Win64 only ever returns one INTEGER eightbyte (RAX).
            static const PhysReg kIntRet[2] = {PhysReg::RAX, PhysReg::RDX};
            static const XmmReg kSseRet[2] = {XmmReg::XMM0, XmmReg::XMM1};
            unsigned intIdx = 0, sseIdx = 0;
            for (std::size_t e = 0; e < retCls.eightbytes.size(); ++e) {
                const AbiEightbyte& eb = retCls.eightbytes[e];
                std::int64_t off = static_cast<std::int64_t>(e) * 8;
                if (eb.isSSE) {
                    std::uint8_t fw = (eb.bytes <= 4) ? 4 : 8;
                    VReg v = fn_->newVReg(RegClass::XMM);
                    emitFW(MOpcode::FMovRR,
                           {MOperand::defVReg(v), MOperand::usePhysXmm(kSseRet[sseIdx++])}, fw);
                    emitFW(MOpcode::FStoreInd,
                           {MOperand::useVReg(sretTempAddr), MOperand::immediate(off),
                            MOperand::useVReg(v)}, fw);
                } else {
                    std::uint8_t w = static_cast<std::uint8_t>(eb.bytes);
                    if (w > 4) w = 8; else if (w > 2) w = 4; else if (w > 1) w = 2; else w = 1;
                    VReg v = fn_->newVReg();
                    emit({MOpcode::MovRR,
                          {MOperand::defVReg(v), MOperand::usePhys(kIntRet[intIdx++])}});
                    MInst st{MOpcode::StoreInd,
                             {MOperand::useVReg(sretTempAddr), MOperand::immediate(off),
                              MOperand::useVReg(v)}};
                    st.width = w; st.isSigned = false;
                    emit(st);
                }
            }
        }
        return sretTempAddr;
    }

    // Float result comes back in the XMM return register; integers/pointers RAX.
    if (isFloat128(callTy)) {
        fail("selector: f128 return ABI is not supported yet");
        return kInvalidVReg;
    }
    if (isFloatType(callTy)) {
        if (isFloat16(callTy)) {
            VReg res = fn_->newVReg(RegClass::XMM);
            emitFW(MOpcode::CvtF16ToF32,
                   {MOperand::defVReg(res), MOperand::usePhysXmm(abi_.xmmReturnReg)}, 4);
            return res;
        }
        VReg res = fn_->newVReg(RegClass::XMM);
        emitFW(MOpcode::FMovRR,
               {MOperand::defVReg(res), MOperand::usePhysXmm(abi_.xmmReturnReg)},
               floatWidthOf(callTy));
        return res;
    }

    // A 16-byte integer (i128/u128) is returned in the RAX:RDX pair (low:high on
    // both ABIs). Capture both halves into a fresh 16-byte temp and yield its
    // address (the i128 memory-value representation).
    if (isInt128(callTy)) {
        VReg lo = fn_->newVReg();
        emit({MOpcode::MovRR, {MOperand::defVReg(lo), MOperand::usePhys(PhysReg::RAX)}});
        VReg hi = fn_->newVReg();
        emit({MOpcode::MovRR, {MOperand::defVReg(hi), MOperand::usePhys(PhysReg::RDX)}});
        std::uint32_t slot = fn_->addFrameSlot(16, 16, /*isSpill=*/false);
        VReg addr = fn_->newVReg();
        emit({MOpcode::LeaSlot, {MOperand::defVReg(addr), MOperand::slot(slot)}});
        storeHalf(addr, 0, lo);
        storeHalf(addr, 8, hi);
        return addr;
    }

    // Result is in RAX; copy into a fresh vreg.
    VReg res = fn_->newVReg();
    emit({MOpcode::MovRR, {MOperand::defVReg(res), MOperand::usePhys(abi_.intReturnReg)}});
    return res;
}


// --- raw inline-assembly blocks ---------------------------------------------

namespace {

// Mnemonics whose first operand is a destination. Used only to refuse writes to
// rsp/rbp: the block sits inside an ordinary frame whose prologue and epilogue
// own those two registers, so letting a block redefine them would silently
// corrupt the caller's stack rather than fail.
bool asmWritesFirstOperand(const std::string& mn) {
    static const char* kWriters[] = {
        "mov", "movzx", "movsx", "lea", "add", "sub", "and", "or", "xor",
        "adc", "sbb", "inc", "dec", "neg", "not", "shl", "shr", "sar",
        "rol", "ror", "imul", "xchg", "pop",
    };
    for (const char* w : kWriters) {
        if (mn == w) return true;
    }
    return false;
}

}  // namespace

void InstructionSelector::selAsmBlock(const AST::InlineAsmExpr& node) {
    Backend::AsmProgram prog;
    std::string err;
    if (!Backend::parseAsmBlock(node.rawBody, prog, err)) {
        fail("asm block: " + err);
        return;
    }

    // Directives. `keep(reg)` reserves a register for the block.
    for (const auto& attr : node.attributes) {
        if (attr.name != "keep") {
            fail("asm block: unknown directive '" + attr.name +
                 "' (the only directive is keep(reg))");
            return;
        }
        std::string name = attr.value;
        for (auto& c : name) c = static_cast<char>(std::tolower((unsigned char)c));
        int idx = -1;
        unsigned w = 0;
        if (!Backend::asmRegByName(name, idx, w)) {
            fail("asm block: keep(" + attr.value +
                 ") does not name a general-purpose register");
            return;
        }
        prog.keepRegs.push_back(idx);
    }

    // `$var` -> the local's frame slot. Locals already live in slots, so this is
    // just a lookup; the byte offset is filled in during lowering.
    for (auto& inst : prog.insts) {
        for (auto& op : inst.ops) {
            if (op.kind != Backend::AsmOpKind::Slot) continue;
            LocalInfo li;
            if (!lookupLocal(op.var, li)) {
                fail("asm block: '$" + op.var + "' does not name a variable in scope");
                return;
            }
            op.slot = li.slot;
            if (op.width == 0) op.width = li.width;
        }
    }

    // Refuse to let a block redefine the frame registers.
    for (const auto& inst : prog.insts) {
        if (inst.ops.empty()) continue;
        const auto& dst = inst.ops[0];
        const bool frameReg = dst.kind == Backend::AsmOpKind::Reg &&
                              (dst.reg == 4 /*rsp*/ || dst.reg == 5 /*rbp*/);
        if (frameReg && asmWritesFirstOperand(inst.mnemonic)) {
            fail("asm block: line " + std::to_string(inst.line) +
                 " writes " + std::string(dst.reg == 4 ? "rsp" : "rbp") +
                 ", which this function's prologue and epilogue own; use "
                 "[naked(on)] to take over the frame");
            return;
        }
    }

    const std::uint32_t index = static_cast<std::uint32_t>(fn_->asmBlocks.size());
    const std::vector<int> keeps = prog.keepRegs;
    fn_->asmBlocks.push_back(std::move(prog));

    MInst inst{MOpcode::AsmBlock, {MOperand::immediate(index)}};
    // The clobber contract: a block is assumed to destroy every caller-saved
    // register, plus anything it reserved with keep(). Declaring less than the
    // block really touches therefore costs a little performance and can never
    // cost correctness. Registers listed here are also treated as "used" by the
    // frame layout, so a reserved callee-saved register gets saved and restored.
    for (PhysReg r : abi_.callerSaved) inst.clobbers.push_back(r);
    for (int k : keeps) {
        const PhysReg r = static_cast<PhysReg>(k);
        bool present = false;
        for (PhysReg c : inst.clobbers) {
            if (c == r) present = true;
        }
        if (!present) inst.clobbers.push_back(r);
    }
    emit(inst);
}

}  // namespace Backend






