#include <backend/asm_block.hpp>

#include <cctype>
#include <cstdlib>
#include <fadec-enc2.h>

namespace Backend {

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    const std::size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    const std::size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

struct RegName {
    const char* name;
    int idx;
    unsigned width;
};

// x86-64 GP encoding order: 0=A 1=C 2=D 3=B 4=SP 5=BP 6=SI 7=DI, then r8..r15.
// ah/ch/dh/bh are deliberately absent: they need Fadec's high-byte register kind
// and are never worth the ambiguity in hand-written blocks.
const RegName kRegs[] = {
    {"rax", 0, 8},  {"rcx", 1, 8},  {"rdx", 2, 8},  {"rbx", 3, 8},
    {"rsp", 4, 8},  {"rbp", 5, 8},  {"rsi", 6, 8},  {"rdi", 7, 8},
    {"r8", 8, 8},   {"r9", 9, 8},   {"r10", 10, 8}, {"r11", 11, 8},
    {"r12", 12, 8}, {"r13", 13, 8}, {"r14", 14, 8}, {"r15", 15, 8},

    {"eax", 0, 4},  {"ecx", 1, 4},  {"edx", 2, 4},  {"ebx", 3, 4},
    {"esp", 4, 4},  {"ebp", 5, 4},  {"esi", 6, 4},  {"edi", 7, 4},
    {"r8d", 8, 4},  {"r9d", 9, 4},  {"r10d", 10, 4}, {"r11d", 11, 4},
    {"r12d", 12, 4}, {"r13d", 13, 4}, {"r14d", 14, 4}, {"r15d", 15, 4},

    {"ax", 0, 2},   {"cx", 1, 2},   {"dx", 2, 2},   {"bx", 3, 2},
    {"sp", 4, 2},   {"bp", 5, 2},   {"si", 6, 2},   {"di", 7, 2},
    {"r8w", 8, 2},  {"r9w", 9, 2},  {"r10w", 10, 2}, {"r11w", 11, 2},
    {"r12w", 12, 2}, {"r13w", 13, 2}, {"r14w", 14, 2}, {"r15w", 15, 2},

    {"al", 0, 1},   {"cl", 1, 1},   {"dl", 2, 1},   {"bl", 3, 1},
    {"spl", 4, 1},  {"bpl", 5, 1},  {"sil", 6, 1},  {"dil", 7, 1},
    {"r8b", 8, 1},  {"r9b", 9, 1},  {"r10b", 10, 1}, {"r11b", 11, 1},
    {"r12b", 12, 1}, {"r13b", 13, 1}, {"r14b", 14, 1}, {"r15b", 15, 1},
};

bool parseInt(const std::string& text, long long& out) {
    if (text.empty()) return false;
    std::string s = text;
    bool neg = false;
    if (s[0] == '+' || s[0] == '-') {
        neg = (s[0] == '-');
        s.erase(s.begin());
    }
    if (s.empty()) return false;
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s = s.substr(2);
    } else if (s.size() > 1 && (s.back() == 'h' || s.back() == 'H')) {
        base = 16;  // NASM's trailing-h form
        s.pop_back();
    } else if (s.size() > 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
        base = 2;
        s = s.substr(2);
    }
    if (s.empty()) return false;
    char* end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, base);
    if (end == nullptr || *end != '\0') return false;
    out = neg ? -v : v;
    return true;
}

// Splits an operand list on commas that are not inside brackets.
std::vector<std::string> splitOperands(const std::string& s) {
    std::vector<std::string> out;
    int depth = 0;
    std::string cur;
    for (const char c : s) {
        if (c == '[') ++depth;
        if (c == ']') --depth;
        if (c == ',' && depth == 0) {
            out.push_back(trim(cur));
            cur.clear();
            continue;
        }
        cur.push_back(c);
    }
    const std::string last = trim(cur);
    if (!last.empty()) out.push_back(last);
    return out;
}

// `[base + index*scale + disp]`, `[base - disp]`, `[$var]`, `[$var + 8]`.
bool parseMem(const std::string& inner, AsmOperand& op, AsmProgram& prog,
              std::string& err) {
    // Split into +/- separated terms, keeping each sign.
    struct Term {
        bool negative;
        std::string text;
    };
    std::vector<Term> terms;
    std::string cur;
    bool neg = false;
    for (std::size_t i = 0; i < inner.size(); ++i) {
        const char c = inner[i];
        if (c == '+' || c == '-') {
            const std::string t = trim(cur);
            if (!t.empty()) terms.push_back({neg, t});
            cur.clear();
            neg = (c == '-');
            continue;
        }
        cur.push_back(c);
    }
    const std::string tail = trim(cur);
    if (!tail.empty()) terms.push_back({neg, tail});

    for (const auto& term : terms) {
        // scaled index: reg*N or N*reg
        const std::size_t star = term.text.find('*');
        if (star != std::string::npos) {
            std::string a = trim(term.text.substr(0, star));
            std::string b = trim(term.text.substr(star + 1));
            int idx = -1;
            unsigned w = 0;
            long long sc = 0;
            if (asmRegByName(lower(a), idx, w)) {
                if (!parseInt(b, sc)) {
                    err = "expected a scale after '*'";
                    return false;
                }
            } else if (asmRegByName(lower(b), idx, w)) {
                if (!parseInt(a, sc)) {
                    err = "expected a scale before '*'";
                    return false;
                }
            } else {
                err = "unrecognized scaled index '" + term.text + "'";
                return false;
            }
            if (sc != 1 && sc != 2 && sc != 4 && sc != 8) {
                err = "index scale must be 1, 2, 4, or 8";
                return false;
            }
            op.index = idx;
            op.scale = static_cast<unsigned>(sc);
            continue;
        }
        if (term.text[0] == '$') {
            const std::string name = term.text.substr(1);
            if (name.empty()) {
                err = "expected a name after '$'";
                return false;
            }
            op.kind = AsmOpKind::Slot;
            op.var = name;
            bool seen = false;
            for (const auto& v : prog.varRefs) {
                if (v == name) seen = true;
            }
            if (!seen) prog.varRefs.push_back(name);
            continue;
        }
        int idx = -1;
        unsigned w = 0;
        if (asmRegByName(lower(term.text), idx, w)) {
            if (op.reg == -1) {
                op.reg = idx;
            } else if (op.index == -1) {
                op.index = idx;
                op.scale = 1;
            } else {
                err = "too many registers in a memory operand";
                return false;
            }
            continue;
        }
        long long v = 0;
        if (parseInt(term.text, v)) {
            op.imm += term.negative ? -v : v;
            continue;
        }
        err = "unrecognized term '" + term.text + "' in a memory operand";
        return false;
    }
    if (op.kind != AsmOpKind::Slot) op.kind = AsmOpKind::Mem;
    return true;
}

bool parseOperand(const std::string& raw, AsmOperand& op, AsmProgram& prog,
                  std::string& err) {
    std::string text = trim(raw);
    if (text.empty()) {
        err = "empty operand";
        return false;
    }

    // Optional size hint: `byte`/`word`/`dword`/`qword`, with an optional `ptr`.
    unsigned hint = 0;
    for (;;) {
        const std::size_t sp = text.find_first_of(" \t");
        if (sp == std::string::npos) break;
        const std::string head = lower(text.substr(0, sp));
        unsigned h = 0;
        if (head == "byte") h = 1;
        else if (head == "word") h = 2;
        else if (head == "dword") h = 4;
        else if (head == "qword") h = 8;
        else if (head == "ptr") h = hint ? hint : 0;
        else break;
        if (head != "ptr") hint = h;
        text = trim(text.substr(sp + 1));
    }

    if (!text.empty() && text[0] == '[') {
        const std::size_t close = text.rfind(']');
        if (close == std::string::npos) {
            err = "unterminated '[' in a memory operand";
            return false;
        }
        if (!parseMem(trim(text.substr(1, close - 1)), op, prog, err)) return false;
        op.width = hint;
        return true;
    }

    if (text[0] == '$') {
        // A bare `$var` is the variable's storage, i.e. a memory operand.
        const std::string name = text.substr(1);
        if (name.empty()) {
            err = "expected a name after '$'";
            return false;
        }
        op.kind = AsmOpKind::Slot;
        op.var = name;
        op.width = hint;
        bool seen = false;
        for (const auto& v : prog.varRefs) {
            if (v == name) seen = true;
        }
        if (!seen) prog.varRefs.push_back(name);
        return true;
    }

    int idx = -1;
    unsigned w = 0;
    if (asmRegByName(lower(text), idx, w)) {
        op.kind = AsmOpKind::Reg;
        op.reg = idx;
        op.width = w;
        return true;
    }

    long long v = 0;
    if (parseInt(text, v)) {
        op.kind = AsmOpKind::Imm;
        op.imm = v;
        op.width = hint;
        return true;
    }

    err = "unrecognized operand '" + text + "'";
    return false;
}

// Fadec's FE_GP/FE_MEM macros are braced initializers over unsigned char fields,
// which C++ refuses to narrow to implicitly. These build the same structs with
// the casts written out.
FeRegGP gpr(int idx) {
    FeRegGP r;
    r.idx = static_cast<unsigned char>(idx >= 0 ? idx : 0x80);  // 0x80 == FE_NOREG
    return r;
}

FeMem memOf(const AsmOperand& op) {
    FeMem m;
    m.flags = 0;
    m.base = gpr(op.reg);
    m.scale = static_cast<unsigned char>(op.scale);
    m.idx = gpr(op.index);
    m.off = static_cast<std::int32_t>(op.imm);
    return m;
}

// A compact description of the operand shapes, e.g. "rr", "ri", "mr".
std::string signatureOf(const AsmInst& inst) {
    std::string sig;
    for (const auto& op : inst.ops) {
        switch (op.kind) {
            case AsmOpKind::Reg: sig.push_back('r'); break;
            case AsmOpKind::Imm: sig.push_back('i'); break;
            case AsmOpKind::Mem:
            case AsmOpKind::Slot: sig.push_back('m'); break;
            case AsmOpKind::Label: sig.push_back('L'); break;
            case AsmOpKind::None: sig.push_back('?'); break;
        }
    }
    return sig;
}

// The operation width: taken from a register operand when there is one, else
// from an explicit size hint. Memory-only forms need the hint because
// `mov [rbp-8], 1` is ambiguous about how many bytes it writes.
unsigned widthOf(const AsmInst& inst) {
    for (const auto& op : inst.ops) {
        if (op.kind == AsmOpKind::Reg) return op.width;
    }
    for (const auto& op : inst.ops) {
        if (op.width != 0) return op.width;
    }
    return 0;
}

}  // namespace

AsmBranch asmBranchOf(const std::string& mn) {
    if (mn == "jmp") return AsmBranch::Jmp;
    if (mn == "je" || mn == "jz") return AsmBranch::Je;
    if (mn == "jne" || mn == "jnz") return AsmBranch::Jne;
    if (mn == "jl" || mn == "jnge") return AsmBranch::Jl;
    if (mn == "jle" || mn == "jng") return AsmBranch::Jle;
    if (mn == "jg" || mn == "jnle") return AsmBranch::Jg;
    if (mn == "jge" || mn == "jnl") return AsmBranch::Jge;
    if (mn == "jb" || mn == "jnae" || mn == "jc") return AsmBranch::Jb;
    if (mn == "jbe" || mn == "jna") return AsmBranch::Jbe;
    if (mn == "ja" || mn == "jnbe") return AsmBranch::Ja;
    if (mn == "jae" || mn == "jnb" || mn == "jnc") return AsmBranch::Jae;
    return AsmBranch::None;
}

bool asmRegByName(const std::string& name, int& idx, unsigned& width) {
    for (const auto& r : kRegs) {
        if (name == r.name) {
            idx = r.idx;
            width = r.width;
            return true;
        }
    }
    return false;
}

bool parseAsmBlock(const std::string& text, AsmProgram& out, std::string& err) {
    int line = 0;
    std::size_t i = 0;
    while (i <= text.size()) {
        // Take one line.
        std::size_t nl = text.find('\n', i);
        if (nl == std::string::npos) nl = text.size();
        std::string raw = text.substr(i, nl - i);
        i = nl + 1;
        ++line;

        // Strip a `;` comment.
        const std::size_t semi = raw.find(';');
        if (semi != std::string::npos) raw = raw.substr(0, semi);
        std::string stmt = trim(raw);
        if (stmt.empty()) {
            if (nl == text.size()) break;
            continue;
        }

        AsmInst inst;
        inst.line = line;

        // A leading `name:` defines a branch target. Anything after it on the
        // same line is still an instruction.
        const std::size_t colon = stmt.find(':');
        if (colon != std::string::npos) {
            const std::string head = trim(stmt.substr(0, colon));
            bool plainName = !head.empty();
            for (const char ch : head) {
                if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' ||
                      ch == '.')) {
                    plainName = false;
                }
            }
            if (plainName) {
                inst.labelDef = head;
                stmt = trim(stmt.substr(colon + 1));
                if (stmt.empty()) {
                    out.insts.push_back(std::move(inst));
                    if (nl == text.size()) break;
                    continue;
                }
            }
        }
        const std::size_t sp = stmt.find_first_of(" \t");
        if (sp == std::string::npos) {
            inst.mnemonic = lower(stmt);
        } else {
            inst.mnemonic = lower(stmt.substr(0, sp));
            const std::string rest = trim(stmt.substr(sp + 1));
            const bool isBranch = asmBranchOf(inst.mnemonic) != AsmBranch::None;
            for (const auto& piece : splitOperands(rest)) {
                AsmOperand op;
                std::string e;
                // A branch target that is a plain name (and not a register, which
                // would be an indirect jump) refers to a label in this block.
                int dummyIdx = -1;
                unsigned dummyW = 0;
                if (isBranch && !piece.empty() && piece[0] != '[' && piece[0] != '$' &&
                    !asmRegByName(lower(piece), dummyIdx, dummyW)) {
                    long long numeric = 0;
                    if (!parseInt(piece, numeric)) {
                        op.kind = AsmOpKind::Label;
                        op.var = piece;
                        inst.ops.push_back(op);
                        continue;
                    }
                }
                if (!parseOperand(piece, op, out, e)) {
                    err = "line " + std::to_string(line) + ": " + e;
                    return false;
                }
                inst.ops.push_back(op);
            }
        }
        out.insts.push_back(std::move(inst));
        if (nl == text.size()) break;
    }
    return true;
}

// --- encoding ---------------------------------------------------------------
//
// One case per (mnemonic, width, operand shape). The macros below exist because
// Fadec's entry points are individually named functions (fe64_ADD64rr, ...)
// rather than a table that can be indexed at run time, so each combination has
// to be named in source. Adding a mnemonic is one macro use.

#define ASM_ARITH_W(NAME, W)                                                      \
    if (sig == "rr") return fe64_##NAME##W##rr(buf, 0, gpr(o0.reg), gpr(o1.reg)); \
    if (sig == "ri") return fe64_##NAME##W##ri(buf, 0, gpr(o0.reg), (int64_t)o1.imm); \
    if (sig == "rm") return fe64_##NAME##W##rm(buf, 0, gpr(o0.reg), memOf(o1));  \
    if (sig == "mr") return fe64_##NAME##W##mr(buf, 0, memOf(o0), gpr(o1.reg));  \
    if (sig == "mi") return fe64_##NAME##W##mi(buf, 0, memOf(o0), (int64_t)o1.imm);

#define ASM_ARITH(TEXT, NAME)                      \
    if (mn == TEXT) {                             \
        if (w == 8) { ASM_ARITH_W(NAME, 64) }      \
        else if (w == 4) { ASM_ARITH_W(NAME, 32) } \
        else if (w == 2) { ASM_ARITH_W(NAME, 16) } \
        else if (w == 1) { ASM_ARITH_W(NAME, 8) }  \
    }

#define ASM_UNARY_W(NAME, W)                                        \
    if (sig == "r") return fe64_##NAME##W##r(buf, 0, gpr(o0.reg)); \
    if (sig == "m") return fe64_##NAME##W##m(buf, 0, memOf(o0));

#define ASM_UNARY(TEXT, NAME)                      \
    if (mn == TEXT) {                             \
        if (w == 8) { ASM_UNARY_W(NAME, 64) }      \
        else if (w == 4) { ASM_UNARY_W(NAME, 32) } \
        else if (w == 2) { ASM_UNARY_W(NAME, 16) } \
        else if (w == 1) { ASM_UNARY_W(NAME, 8) }  \
    }

#define ASM_SHIFT_W(NAME, W)                                                      \
    if (sig == "ri") return fe64_##NAME##W##ri(buf, 0, gpr(o0.reg), (int64_t)o1.imm); \
    if (sig == "rr") return fe64_##NAME##W##rr(buf, 0, gpr(o0.reg), gpr(o1.reg)); \
    if (sig == "mi") return fe64_##NAME##W##mi(buf, 0, memOf(o0), (int64_t)o1.imm); \
    if (sig == "mr") return fe64_##NAME##W##mr(buf, 0, memOf(o0), gpr(o1.reg));

#define ASM_SHIFT(TEXT, NAME)                      \
    if (mn == TEXT) {                             \
        if (w == 8) { ASM_SHIFT_W(NAME, 64) }      \
        else if (w == 4) { ASM_SHIFT_W(NAME, 32) } \
        else if (w == 2) { ASM_SHIFT_W(NAME, 16) } \
        else if (w == 1) { ASM_SHIFT_W(NAME, 8) }  \
    }

#define ASM_NULLARY(TEXT, FN) \
    if (mn == TEXT && sig.empty()) return fe64_##FN(buf, 0);

int encodeAsmInst(const AsmInst& inst, std::uint8_t* buf, std::string& err) {
    const std::string& mn = inst.mnemonic;
    const std::string sig = signatureOf(inst);
    const unsigned w = widthOf(inst);
    static const AsmOperand kNone{};
    const AsmOperand& o0 = inst.ops.size() > 0 ? inst.ops[0] : kNone;
    const AsmOperand& o1 = inst.ops.size() > 1 ? inst.ops[1] : kNone;
    const AsmOperand& o2 = inst.ops.size() > 2 ? inst.ops[2] : kNone;

    // No-operand instructions, including the privileged ones.
    ASM_NULLARY("nop", NOP)
    ASM_NULLARY("ret", RET)
    ASM_NULLARY("syscall", SYSCALL)
    ASM_NULLARY("cli", CLI)
    ASM_NULLARY("sti", STI)
    ASM_NULLARY("hlt", HLT)
    ASM_NULLARY("pause", PAUSE)
    ASM_NULLARY("int3", INT3)
    ASM_NULLARY("ud2", UD2)
    ASM_NULLARY("cpuid", CPUID)
    ASM_NULLARY("rdtsc", RDTSC)
    ASM_NULLARY("rdmsr", RDMSR)
    ASM_NULLARY("wrmsr", WRMSR)
    ASM_NULLARY("wbinvd", WBINVD)
    ASM_NULLARY("mfence", MFENCE)
    ASM_NULLARY("lfence", LFENCE)
    ASM_NULLARY("sfence", SFENCE)
    ASM_NULLARY("iret", IRET64)
    ASM_NULLARY("iretq", IRET64)
    ASM_NULLARY("leave", LEAVE)

    ASM_ARITH("mov", MOV)
    ASM_ARITH("add", ADD)
    ASM_ARITH("sub", SUB)
    ASM_ARITH("and", AND)
    ASM_ARITH("or", OR)
    ASM_ARITH("xor", XOR)
    ASM_ARITH("cmp", CMP)
    ASM_ARITH("adc", ADC)
    ASM_ARITH("sbb", SBB)

    ASM_UNARY("inc", INC)
    ASM_UNARY("dec", DEC)
    ASM_UNARY("neg", NEG)
    ASM_UNARY("not", NOT)
    ASM_UNARY("mul", MUL)
    ASM_UNARY("imul", IMUL)
    ASM_UNARY("div", DIV)
    ASM_UNARY("idiv", IDIV)

    ASM_SHIFT("shl", SHL)
    ASM_SHIFT("shr", SHR)
    ASM_SHIFT("sar", SAR)
    ASM_SHIFT("rol", ROL)
    ASM_SHIFT("ror", ROR)

    // TEST has no `rm` form (it is symmetric; use `mr`).
    if (mn == "test") {
        if (w == 8) {
            if (sig == "rr") return fe64_TEST64rr(buf, 0, gpr(o0.reg), gpr(o1.reg));
            if (sig == "ri") return fe64_TEST64ri(buf, 0, gpr(o0.reg), (int64_t)o1.imm);
            if (sig == "mr") return fe64_TEST64mr(buf, 0, memOf(o0), gpr(o1.reg));
            if (sig == "mi") return fe64_TEST64mi(buf, 0, memOf(o0), (int64_t)o1.imm);
        } else if (w == 4) {
            if (sig == "rr") return fe64_TEST32rr(buf, 0, gpr(o0.reg), gpr(o1.reg));
            if (sig == "ri") return fe64_TEST32ri(buf, 0, gpr(o0.reg), (int64_t)o1.imm);
            if (sig == "mr") return fe64_TEST32mr(buf, 0, memOf(o0), gpr(o1.reg));
            if (sig == "mi") return fe64_TEST32mi(buf, 0, memOf(o0), (int64_t)o1.imm);
        } else if (w == 1) {
            if (sig == "rr") return fe64_TEST8rr(buf, 0, gpr(o0.reg), gpr(o1.reg));
            if (sig == "ri") return fe64_TEST8ri(buf, 0, gpr(o0.reg), (int64_t)o1.imm);
            if (sig == "mr") return fe64_TEST8mr(buf, 0, memOf(o0), gpr(o1.reg));
            if (sig == "mi") return fe64_TEST8mi(buf, 0, memOf(o0), (int64_t)o1.imm);
        }
    }

    if (mn == "lea" && sig == "rm") {
        if (w == 8) return fe64_LEA64rm(buf, 0, gpr(o0.reg), memOf(o1));
        if (w == 4) return fe64_LEA32rm(buf, 0, gpr(o0.reg), memOf(o1));
    }

    // push/pop are 64-bit only in long mode, so they carry no width suffix.
    if (mn == "push") {
        if (sig == "r") return fe64_PUSHr(buf, 0, gpr(o0.reg));
        if (sig == "m") return fe64_PUSHm(buf, 0, memOf(o0));
        if (sig == "i") return fe64_PUSHi(buf, 0, (int64_t)o0.imm);
    }
    if (mn == "pop") {
        if (sig == "r") return fe64_POPr(buf, 0, gpr(o0.reg));
        if (sig == "m") return fe64_POPm(buf, 0, memOf(o0));
    }

    if (mn == "xchg") {
        if (w == 8 && sig == "rr") return fe64_XCHG64rr(buf, 0, gpr(o0.reg), gpr(o1.reg));
        if (w == 8 && sig == "mr") return fe64_XCHG64mr(buf, 0, memOf(o0), gpr(o1.reg));
        if (w == 4 && sig == "rr") return fe64_XCHG32rr(buf, 0, gpr(o0.reg), gpr(o1.reg));
    }

    // Zero/sign extension: the destination width comes from the destination
    // register, the source width from the source operand.
    if ((mn == "movzx" || mn == "movsx") && inst.ops.size() == 2 &&
        o0.kind == AsmOpKind::Reg) {
        const bool z = (mn == "movzx");
        const unsigned dw = o0.width;
        const unsigned sw = o1.kind == AsmOpKind::Reg ? o1.width : o1.width;
        if (o1.kind == AsmOpKind::Reg) {
            if (dw == 8 && sw == 1) return z ? fe64_MOVZXr64r8(buf, 0, gpr(o0.reg), gpr(o1.reg))
                                             : fe64_MOVSXr64r8(buf, 0, gpr(o0.reg), gpr(o1.reg));
            if (dw == 8 && sw == 2) return z ? fe64_MOVZXr64r16(buf, 0, gpr(o0.reg), gpr(o1.reg))
                                             : fe64_MOVSXr64r16(buf, 0, gpr(o0.reg), gpr(o1.reg));
            if (dw == 8 && sw == 4 && !z) return fe64_MOVSXr64r32(buf, 0, gpr(o0.reg), gpr(o1.reg));
            if (dw == 4 && sw == 1) return z ? fe64_MOVZXr32r8(buf, 0, gpr(o0.reg), gpr(o1.reg))
                                             : fe64_MOVSXr32r8(buf, 0, gpr(o0.reg), gpr(o1.reg));
            if (dw == 4 && sw == 2) return z ? fe64_MOVZXr32r16(buf, 0, gpr(o0.reg), gpr(o1.reg))
                                             : fe64_MOVSXr32r16(buf, 0, gpr(o0.reg), gpr(o1.reg));
        } else {
            if (dw == 8 && sw == 1) return z ? fe64_MOVZXr64m8(buf, 0, gpr(o0.reg), memOf(o1))
                                             : fe64_MOVSXr64m8(buf, 0, gpr(o0.reg), memOf(o1));
            if (dw == 8 && sw == 2) return z ? fe64_MOVZXr64m16(buf, 0, gpr(o0.reg), memOf(o1))
                                             : fe64_MOVSXr64m16(buf, 0, gpr(o0.reg), memOf(o1));
            if (dw == 8 && sw == 4 && !z) return fe64_MOVSXr64m32(buf, 0, gpr(o0.reg), memOf(o1));
            if (dw == 4 && sw == 1) return z ? fe64_MOVZXr32m8(buf, 0, gpr(o0.reg), memOf(o1))
                                             : fe64_MOVSXr32m8(buf, 0, gpr(o0.reg), memOf(o1));
        }
        err = "line " + std::to_string(inst.line) + ": '" + mn +
              "' needs an explicit source size (e.g. `movzx rax, byte [rbp-8]`)";
        return -1;
    }

    // imul reg, reg/mem and the three-operand immediate form.
    if (mn == "imul" && w == 8) {
        if (sig == "rr") return fe64_IMUL64rr(buf, 0, gpr(o0.reg), gpr(o1.reg));
        if (sig == "rm") return fe64_IMUL64rm(buf, 0, gpr(o0.reg), memOf(o1));
        if (sig == "rri") return fe64_IMUL64rri(buf, 0, gpr(o0.reg), gpr(o1.reg), (int64_t)o2.imm);
        if (sig == "rmi") return fe64_IMUL64rmi(buf, 0, gpr(o0.reg), memOf(o1), (int64_t)o2.imm);
    }

    // Indirect control transfer only: a label target would need relocation.
    if (mn == "jmp") {
        if (sig == "r") return fe64_JMPr(buf, 0, gpr(o0.reg));
        if (sig == "m") return fe64_JMPm(buf, 0, memOf(o0));
    }
    if (mn == "call") {
        if (sig == "r") return fe64_CALLr(buf, 0, gpr(o0.reg));
        if (sig == "m") return fe64_CALLm(buf, 0, memOf(o0));
    }

    // Port I/O. NASM spells these `in al, dx` / `out dx, al`, plus an
    // 8-bit-immediate port form.
    if (mn == "in") {
        if (sig == "rr" && o1.reg == 2 /*dx*/) {
            if (o0.width == 1) return fe64_IN8(buf, 0);
            if (o0.width == 2) return fe64_IN16(buf, 0);
            if (o0.width == 4) return fe64_IN32(buf, 0);
        }
        if (sig == "ri") {
            if (o0.width == 1) return fe64_IN8ri(buf, 0, gpr(o0.reg), (int8_t)o1.imm);
            if (o0.width == 2) return fe64_IN16ri(buf, 0, gpr(o0.reg), (int8_t)o1.imm);
            if (o0.width == 4) return fe64_IN32ri(buf, 0, gpr(o0.reg), (int8_t)o1.imm);
        }
    }
    if (mn == "out") {
        if (sig == "rr" && o0.reg == 2 /*dx*/) {
            if (o1.width == 1) return fe64_OUT8(buf, 0);
            if (o1.width == 2) return fe64_OUT16(buf, 0);
            if (o1.width == 4) return fe64_OUT32(buf, 0);
        }
        // NASM writes `out imm8, al` (port first), but Fadec takes the register
        // first and the port second, so the operands are swapped here.
        if (sig == "ir") {
            if (o1.width == 1) return fe64_OUT8ri(buf, 0, gpr(o1.reg), (int8_t)o0.imm);
            if (o1.width == 2) return fe64_OUT16ri(buf, 0, gpr(o1.reg), (int8_t)o0.imm);
            if (o1.width == 4) return fe64_OUT32ri(buf, 0, gpr(o1.reg), (int8_t)o0.imm);
        }
    }

    // Descriptor-table and TLB maintenance.
    if (mn == "lgdt" && sig == "m") return fe64_LGDTm(buf, 0, memOf(o0));
    if (mn == "lidt" && sig == "m") return fe64_LIDTm(buf, 0, memOf(o0));
    if (mn == "sgdt" && sig == "m") return fe64_SGDTm(buf, 0, memOf(o0));
    if (mn == "sidt" && sig == "m") return fe64_SIDTm(buf, 0, memOf(o0));
    if (mn == "invlpg" && sig == "m") return fe64_INVLPG8m(buf, 0, memOf(o0));

    err = "line " + std::to_string(inst.line) + ": '" + inst.mnemonic +
          "' with operands (" + (sig.empty() ? std::string("none") : sig) +
          ") at width " + std::to_string(w) +
          " is not in the assembler's dispatch table";
    return -1;
}

#undef ASM_NULLARY
#undef ASM_SHIFT
#undef ASM_SHIFT_W
#undef ASM_UNARY
#undef ASM_UNARY_W
#undef ASM_ARITH
#undef ASM_ARITH_W

}  // namespace Backend



