#include <compiler/runtime_core.hpp>

#include <parser/parser.hpp>
#include <sema/sema.hpp>
#include <utilities/errors.hpp>

#include <memory>
#include <string>
#include <unordered_map>


namespace {

const char* kCoreSource = R"Insty(
module core

fun [name(__ins_memcpy)] memcpy(u8* dest, u8* src, u64 size) -> void {
    unsafe {
        u64 i = 0
        while i < size {
            dest[i] = src[i]
            i = i + 1
        }
    }
}

fun [name(__ins_memset)] memset(u8* ptr, u8 value, u64 size) -> void {
    unsafe {
        u64 i = 0
        while i < size {
            ptr[i] = value
            i = i + 1
        }
    }
}

// --- string interpolation formatters -------------------------------------
// Cursor convention: append into `buf` starting at `off`, never writing at or
// past `cap`, returning the new offset (may equal cap once full).

// Copy up to (cap-off) of the first `n` bytes of `src` into buf at off.
fun [name(__ins_fmt_raw)] fmt_raw(u8* buf, i64 cap, i64 off, u8* src, i64 n) -> i64 {
    unsafe {
        i64 i = 0
        i64 o = off
        while i < n {
            if o >= cap {
                return o
            }
            buf[o] = src[i]
            i = i + 1
            o = o + 1
        }
        return o
    }
}

// Append the decimal representation of `n` (signed when is_signed is true).
fun [name(__ins_fmt_int)] fmt_int(u8* buf, i64 cap, i64 off, i64 n, bool is_signed) -> i64 {
    unsafe {
        u8[32] scratch
        u8* digits = cast<u8*>(&scratch)
        i64 v = n
        bool neg = false
        if is_signed {
            if v < 0 {
                neg = true
                v = 0 - v
            }
        }
        // Generate digits (reversed). At least one digit (handles 0).
        i64 di = 0
        bool more = true
        while more {
            i64 q = cast<i64>(cast<u64>(v) / 10)
            i64 r = cast<i64>(cast<u64>(v) - cast<u64>(q) * 10)
            digits[di] = cast<u8>(48 + r)
            di = di + 1
            v = q
            if v == 0 {
                more = false
            }
        }
        i64 o = off
        if neg {
            if o < cap {
                buf[o] = 45
                o = o + 1
            }
        }
        // Emit digits in forward order (reverse of scratch).
        i64 k = di
        while k > 0 {
            if o >= cap {
                return o
            }
            k = k - 1
            buf[o] = digits[k]
            o = o + 1
        }
        return o
    }
}

// Append `v` with a fixed 6 fractional decimal places.
fun [name(__ins_fmt_float)] fmt_float(u8* buf, i64 cap, i64 off, f64 v) -> i64 {
    unsafe {
        f64 a = v
        i64 o = off
        if a < 0.0 {
            a = 0.0 - a
            if o < cap {
                buf[o] = 45
                o = o + 1
            }
        }
        i64 ipart = cast<i64>(a)
        o = fmt_int(buf, cap, o, ipart, false)
        if o < cap {
            buf[o] = 46
            o = o + 1
        }
        f64 frac = a - cast<f64>(ipart)
        i64 scaled = cast<i64>(frac * 1000000.0 + 0.5)
        // Emit exactly 6 fractional digits with leading zeros.
        i64 divisor = 100000
        while divisor > 0 {
            if o >= cap {
                return o
            }
            i64 dgt = cast<i64>(cast<u64>(scaled) / cast<u64>(divisor))
            i64 rem = cast<i64>(cast<u64>(scaled) - cast<u64>(dgt) * cast<u64>(divisor))
            buf[o] = cast<u8>(48 + dgt)
            o = o + 1
            scaled = rem
            divisor = cast<i64>(cast<u64>(divisor) / 10)
        }
        return o
    }
}

// Finalize: on overflow stamp a ".." marker, then NUL-terminate within bounds.
fun [name(__ins_fmt_finish)] fmt_finish(u8* buf, i64 cap, i64 off) -> void {
    unsafe {
        if off >= cap {
            buf[cap - 1] = 46
            buf[cap - 2] = 46
            buf[cap] = 0
            return
        }
        buf[off] = 0
    }
}

// --- half-precision (f16) conversion --------------------------------------
// Software IEEE-754 binary16 <-> binary32 conversion, operating on raw bit
// patterns. x86-64 has these in hardware (F16C: vcvtph2ps / vcvtps2ph), so the
// native backends never call these; WebAssembly has no f16 at all, and its
// backend lowers CvtF16ToF32 / CvtF32ToF16 to them.
//
// f32 -> f16 rounds to nearest, ties to even -- the same mode vcvtps2ph uses
// with its immediate set to 0 -- so both backends agree bit for bit. Subnormals,
// infinities and NaNs are all handled; overflow becomes infinity and underflow
// becomes a signed zero.

// The half is in the low 16 bits of `h`; the result is an f32 bit pattern.
fun [name(__ins_f16_to_f32)] f16_to_f32(u32 h) -> u32 {
    u32 sign = (h & 0x8000) << 16
    u32 exp = (h >> 10) & 0x1F
    u32 mant = h & 0x3FF

    if exp == 0 {
        if mant == 0 {
            return sign
        }
        // Subnormal: shift left until the implicit bit surfaces, then remove it.
        u32 e = 0
        u32 m = mant
        while (m & 0x400) == 0 {
            m = m << 1
            e = e + 1
        }
        m = m & 0x3FF
        return sign | ((127 - 14 - e) << 23) | (m << 13)
    }
    if exp == 31 {
        // Infinity (mant == 0) or NaN, preserving the payload.
        return sign | 0x7F800000 | (mant << 13)
    }
    // exp - 15 + 127
    return sign | ((exp + 112) << 23) | (mant << 13)
}

// `f` is an f32 bit pattern; the result is a half in the low 16 bits.
fun [name(__ins_f32_to_f16)] f32_to_f16(u32 f) -> u32 {
    u32 sign = (f >> 16) & 0x8000
    u32 exp = (f >> 23) & 0xFF
    u32 mant = f & 0x7FFFFF

    if exp == 255 {
        if mant != 0 {
            return sign | 0x7E00
        }
        return sign | 0x7C00
    }

    // Re-bias for half: exp - 127 + 15.
    i32 e = cast<i32>(exp) - 112

    if e >= 31 {
        return sign | 0x7C00
    }

    if e <= 0 {
        if e < 0 - 10 {
            return sign
        }
        // Subnormal result: restore the implicit 1 and shift it down into place.
        u32 m = mant | 0x800000
        u32 shift = cast<u32>(14 - e)
        u32 h = m >> shift
        u32 roundBit = (m >> (shift - 1)) & 1
        u32 lowMask = (cast<u32>(1) << (shift - 1)) - 1
        u32 sticky = 0
        if (m & lowMask) != 0 {
            sticky = 1
        }
        if roundBit == 1 {
            if sticky == 1 || (h & 1) == 1 {
                h = h + 1
            }
        }
        return sign | h
    }

    // Normal result. A mantissa carry propagates into the exponent, and an
    // exponent carry to 31 yields infinity, which is the correct saturation.
    u32 h = (cast<u32>(e) << 10) | (mant >> 13)
    u32 roundBit = (mant >> 12) & 1
    u32 sticky = 0
    if (mant & 0xFFF) != 0 {
        sticky = 1
    }
    if roundBit == 1 {
        if sticky == 1 || (h & 1) == 1 {
            h = h + 1
        }
    }
    return sign | h
}

// --- 128-bit integer division/modulo helpers ------------------------------
// Each public helper takes pointers to three 16-byte little-endian operands:
//   result (out), a (dividend), b (divisor); low word at [+0], high at [+8].
// The custom backend lowers i128 `/` and `%` to calls to these symbols using a
// fixed by-pointer convention (result,a,b in the first three int arg regs),
// which is identical on System V and Win64. The bodies use only u64-half
// arithmetic (binary long division) so they never recurse into i128 div/mod.

// Unsigned worker: quot = a / b, rem = a % b. Any of quot_*/rem_* may be null.
fun [name(__ins_u128_divmod)] u128_divmod(u8* a, u8* b, u8* quot, u8* rem) -> void {
    unsafe {
        u64* ap = cast<u64*>(a)
        u64* bp = cast<u64*>(b)
        u64 a_lo = ap[0]
        u64 a_hi = ap[1]
        u64 b_lo = bp[0]
        u64 b_hi = bp[1]

        // Running remainder (r_hi:r_lo) and quotient (q_hi:q_lo).
        u64 r_lo = 0
        u64 r_hi = 0
        u64 q_lo = 0
        u64 q_hi = 0

        // Process bits from most-significant (127) down to 0.
        i64 i = 127
        while i >= 0 {
            // remainder <<= 1
            u64 carry_r = r_lo >> 63
            r_hi = (r_hi << 1) | carry_r
            r_lo = r_lo << 1
            // bring in bit i of the dividend as the new low bit of remainder
            u64 bit = 0
            if i >= 64 {
                bit = (a_hi >> cast<u64>(i - 64)) & 1
            } else {
                bit = (a_lo >> cast<u64>(i)) & 1
            }
            r_lo = r_lo | bit

            // if remainder >= divisor: remainder -= divisor; set quotient bit i
            bool ge = false
            if r_hi > b_hi {
                ge = true
            } else {
                if r_hi == b_hi {
                    if r_lo >= b_lo {
                        ge = true
                    }
                }
            }
            if ge {
                // remainder -= divisor (128-bit subtract with borrow)
                u64 new_lo = r_lo - b_lo
                u64 borrow = 0
                if r_lo < b_lo {
                    borrow = 1
                }
                r_hi = r_hi - b_hi - borrow
                r_lo = new_lo
                // set bit i of the quotient
                if i >= 64 {
                    q_hi = q_hi | (cast<u64>(1) << cast<u64>(i - 64))
                } else {
                    q_lo = q_lo | (cast<u64>(1) << cast<u64>(i))
                }
            }
            i = i - 1
        }

        if cast<u64>(quot) != 0 {
            u64* qp = cast<u64*>(quot)
            qp[0] = q_lo
            qp[1] = q_hi
        }
        if cast<u64>(rem) != 0 {
            u64* rp = cast<u64*>(rem)
            rp[0] = r_lo
            rp[1] = r_hi
        }
    }
}

// Negate a 16-byte value in place: x = -x (two's complement).
fun [name(__ins_i128_neg)] i128_neg(u8* x) -> void {
    unsafe {
        u64* p = cast<u64*>(x)
        u64 lo = p[0]
        u64 hi = p[1]
        u64 nlo = (cast<u64>(0) - lo)
        u64 nhi = (cast<u64>(0) - hi)
        if lo != 0 {
            nhi = nhi - 1
        }
        p[0] = nlo
        p[1] = nhi
    }
}

// Copy 16 bytes a -> dst.
fun [name(__ins_i128_copy)] i128_copy(u8* dst, u8* a) -> void {
    unsafe {
        u64* d = cast<u64*>(dst)
        u64* s = cast<u64*>(a)
        d[0] = s[0]
        d[1] = s[1]
    }
}

// Sign bit of a 16-byte value (true if negative).
fun [name(__ins_i128_neg_p)] i128_neg_p(u8* a) -> bool {
    unsafe {
        u64* p = cast<u64*>(a)
        return (p[1] >> 63) != 0
    }
}

// result = a / b  (unsigned).
fun [name(__ins_udivti3)] udivti3(u8* result, u8* a, u8* b) -> void {
    unsafe {
        u128_divmod(a, b, result, cast<u8*>(0))
    }
}

// result = a % b  (unsigned).
fun [name(__ins_umodti3)] umodti3(u8* result, u8* a, u8* b) -> void {
    unsafe {
        u128_divmod(a, b, cast<u8*>(0), result)
    }
}

// result = a / b  (signed): divide magnitudes, apply sign = sign(a) ^ sign(b).
fun [name(__ins_divti3)] divti3(u8* result, u8* a, u8* b) -> void {
    unsafe {
        u8[16] ua_s
        u8[16] ub_s
        u8* ua = cast<u8*>(&ua_s)
        u8* ub = cast<u8*>(&ub_s)
        i128_copy(ua, a)
        i128_copy(ub, b)
        bool na = i128_neg_p(ua)
        bool nb = i128_neg_p(ub)
        if na {
            i128_neg(ua)
        }
        if nb {
            i128_neg(ub)
        }
        u128_divmod(ua, ub, result, cast<u8*>(0))
        if na != nb {
            i128_neg(result)
        }
    }
}

// result = a % b  (signed): remainder takes the sign of the dividend.
fun [name(__ins_modti3)] modti3(u8* result, u8* a, u8* b) -> void {
    unsafe {
        u8[16] ua_s
        u8[16] ub_s
        u8* ua = cast<u8*>(&ua_s)
        u8* ub = cast<u8*>(&ub_s)
        i128_copy(ua, a)
        i128_copy(ub, b)
        bool na = i128_neg_p(ua)
        bool nb = i128_neg_p(ub)
        if na {
            i128_neg(ua)
        }
        if nb {
            i128_neg(ub)
        }
        u128_divmod(ua, ub, cast<u8*>(0), result)
        if na {
            i128_neg(result)
        }
    }
}

// --- allocator ------------------------------------------------------------
// The custom backend lowers these builtin calls to the platform allocator:
//   Win64       HeapAlloc / HeapFree
//   SysV/Linux  mmap / munmap
//
// `realloc` takes the old allocation size so mmap-backed targets can release
// the previous mapping. The legacy @realloc(ptr, new_size[, align]) builtin
// remains accepted by sema, but cannot free on every target.

fun [name(__ins_alloc)] alloc(u64 size, u64 align) -> u8* {
    unsafe {
        return @malloc(size, align)
    }
}

// Linux brk bump allocator. brk syscall is number 12. asm<i64> performs the
// syscall and yields rax; the standard x86_64 convention is rax=num, rdi=arg0.
fun [name(core_alloc_brk)] alloc_brk(u64 size, u64 align) -> u8* {
    unsafe {
        u64 eff_size = size
        if eff_size == 0 {
            eff_size = 1
        }
        u64 eff_align = align
        if eff_align < 16 {
            eff_align = 16
        }
        // Round eff_size up to a multiple of eff_align without bitwise-not.
        u64 aligned = ((eff_size + eff_align - 1) / eff_align) * eff_align

        // Query the current program break: brk(0).
        i64 old_break = asm<i64>("syscall", "={rax},{rax},{rdi},~{rcx},~{r11},~{memory}", 12, 0)
        u64 ub = cast<u64>(old_break)
        u64 new_break = ub + aligned
        if new_break < ub {
            return cast<u8*>(0)
        }
        i64 ret = asm<i64>("syscall", "={rax},{rax},{rdi},~{rcx},~{r11},~{memory}", 12, cast<i64>(new_break))
        if cast<u64>(ret) == new_break {
            return cast<u8*>(ub)
        }
        return cast<u8*>(0)
    }
}

// InstantOS mmap allocator. Raw syscall with the InstantOS register convention
// (rax=num, rbx=addr, r10=len, rdx=prot); syscall number 12.
fun [name(core_alloc_mmap)] alloc_mmap(u64 size, u64 align) -> u8* {
    unsafe {
        u64 eff_size = size
        if eff_size == 0 {
            eff_size = 1
        }
        i64 r = asm<i64>("syscall", "={rax},{rax},{rbx},{r10},{rdx},~{rcx},~{r11},~{memory}", 12, 0, cast<i64>(eff_size), 0)
        if cast<u64>(r) == cast<u64>(0 - 1) {
            return cast<u8*>(0)
        }
        return cast<u8*>(r)
    }
}

fun [name(__ins_free)] free(u8* ptr, u64 size, u64 align) -> void {
    unsafe {
        @free(ptr, size, align)
    }
    return
}

// Reallocate: allocate a new block, copy min(old_size, new_size), free the old.
fun [name(__ins_realloc)] realloc(u8* ptr, u64 old_size, u64 new_size, u64 align) -> u8* {
    unsafe {
        return @realloc(ptr, old_size, new_size, align)
    }
}

// --- string hashing ------------------------------------------------------
// Portable word-wise multiply/xor-shift hash. MUST stay bit-for-bit identical
// to Hashing::portableStringHash in include/utilities/string_hash.hpp so a
// compile-time-folded literal hash equals the runtime hash of the same bytes.
// Reads 8-byte native (little-endian) words over [0, len); the trailing partial
// word is assembled byte-wise. len is scanned within the string, so no read
// occurs at or past the NUL terminator.
fun [name(__ins_hash_bytes)] hash_bytes(u8* ptr, u64 len) -> u64 {
    u64 mul = 11400714819323198485      // 0x9E3779B97F4A7C15 (golden ratio)
    u64 h = 14695981039346656037        // 0xCBF29CE484222325 (FNV offset basis)
    h = h ^ (len * mul)
    unsafe {
        u64 base = cast<u64>(ptr)
        u64 i = 0
        while i + 8 <= len {
            u64 k = ~cast<u64*>(base + i)
            h = h ^ k
            h = h * mul
            h = h ^ (h >> 47)
            i = i + 8
        }
        u64 tail = 0
        u64 s = 0
        while i < len {
            tail = tail | (cast<u64>(ptr[i]) << s)
            s = s + 8
            i = i + 1
        }
        h = h ^ tail
        h = h * mul
        h = h ^ (h >> 47)
    }
    h = h ^ (h >> 32)
    h = h * mul
    h = h ^ (h >> 29)
    return h
}

// Hash a NUL-terminated string: find its length, then hash those bytes.
fun [name(__ins_hash)] hash(text s) -> u64 {
    unsafe {
        u8* p = cast<u8*>(s)
        u64 len = 0
        while p[len] != 0 {
            len = len + 1
        }
        return hash_bytes(p, len)
    }
}

// Null-safe content equality for `text` (`==`/`!=` lower to this). Returns 1 when
// equal, 0 otherwise. If either side is null, falls back to pointer equality so a
// `text == cast<text>(0)` null check stays correct (no dereference of null).
fun [name(__ins_streq)] streq(text a, text b) -> i64 {
    unsafe {
        u64 ua = cast<u64>(a)
        u64 ub = cast<u64>(b)
        if ua == 0 {
            if ub == 0 {
                return 1
            }
            return 0
        }
        if ub == 0 {
            return 0
        }
        u8* pa = cast<u8*>(a)
        u8* pb = cast<u8*>(b)
        u64 i = 0
        while true {
            u8 ca = pa[i]
            u8 cb = pb[i]
            if ca != cb {
                return 0
            }
            if ca == 0 {
                return 1
            }
            i = i + 1
        }
    }
    return 1
}
)Insty";

struct CoreModule {
    std::shared_ptr<AST::ProgramRoot> ast;
    Sema::SemaResult sema;
    bool ok = false;
};

}

namespace {

// Cache of the analyzed core module, keyed by the owning TypeContext's
// process-unique id (not its address). The CoreModule's SemaResult holds
// TypeRefs (raw const Type*) owned by that TypeContext; keying on the raw
// pointer is unsafe because a destroyed context can be recreated at the same
// address, which would return a stale CoreModule full of dangling TypeRefs.
std::unordered_map<uint64_t, std::unique_ptr<CoreModule>> g_coreCache;

CoreModule* getCoreModule(Types::TypeContext& types) {
    auto it = g_coreCache.find(types.id());
    if (it != g_coreCache.end()) {
        return it->second.get();
    }
    auto cm = std::make_unique<CoreModule>();

    std::string source = kCoreSource;
    ErrorReporting::ErrorReporter scratch(source, "<core>");
    Parser parser;
    std::string mutableSource = source;
    cm->ast = parser.produceAST(mutableSource);
    if (cm->ast && !scratch.hasError()) {
        Sema::Analyzer analyzer(types, &scratch);
        cm->sema = analyzer.analyze(cm->ast, {});
        cm->ok = cm->sema.ok && !scratch.hasError();
    }

    CoreModule* raw = cm.get();
    g_coreCache[types.id()] = std::move(cm);
    return raw;
}

}

const Sema::SemaResult* getCoreRuntimeModule(Types::TypeContext& types) {
    CoreModule* core = getCoreModule(types);
    if (!core || !core->ok) return nullptr;
    return &core->sema;
}
