#pragma once

// Portable 128-bit integer arithmetic for compile-time use inside the
// compiler.
//
// The front end stores integer literals as exact 128-bit values (i128/u128
// constants must survive parsing, semantic checks, and constant folding
// without narrowing), and both backends split those values into 64-bit words
// when emitting code. MSVC's cl.exe has no 128-bit integer type, so these two
// structs provide the slice of 128-bit arithmetic the compiler needs, with
// bit-for-bit two's-complement semantics identical to the GCC/Clang
// `__int128` / `unsigned __int128` builtins they replace:
//
//   UInt128 -- unsigned arithmetic and bitwise ops, logical shifts, unsigned
//              comparisons, and 128/128 division/modulo (binary long
//              division; constant folding never sees enough of it to matter).
//   Int128  -- signed arithmetic over the same bit patterns, arithmetic right
//              shift, signed comparisons.
//
// Conversions between the two are explicit and always bit-preserving,
// matching `static_cast` between the builtin signed and unsigned forms. Both
// types widen implicitly from any builtin integer with the builtin's
// semantics (negative signed values sign-extend), so literal-heavy call sites
// such as `v >= -2147483648 && v <= 2147483647` read exactly as before.

#include <concepts>
#include <cstdint>

namespace Utilities {

struct Int128;

struct UInt128 {
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;

    constexpr UInt128() = default;
    constexpr UInt128(std::uint64_t loWord, std::uint64_t hiWord)
        : lo(loWord), hi(hiWord) {}

    // Implicit widening from any builtin integer, with the same semantics as
    // converting to `unsigned __int128`: negative signed values wrap modulo
    // 2^128 (i.e. sign-extend).
    template <std::integral T>
    constexpr UInt128(T v) : lo(static_cast<std::uint64_t>(v)), hi(0) {
        if constexpr (std::is_signed_v<T>) {
            if (v < 0) hi = ~std::uint64_t{0};
        }
    }

    // Bit-preserving reinterpretation of a signed value, mirroring
    // `static_cast<unsigned __int128>(i128)`. Defined after Int128 below.
    explicit constexpr UInt128(Int128 v);

    static constexpr UInt128 fromBits(std::uint64_t loWord, std::uint64_t hiWord) {
        return UInt128(loWord, hiWord);
    }

    constexpr std::uint64_t low64() const { return lo; }
    constexpr std::uint64_t high64() const { return hi; }
    constexpr bool isZero() const { return lo == 0 && hi == 0; }

    // Narrowing to the low word, mirroring `static_cast<std::uint64_t>(u128)`.
    explicit constexpr operator std::uint64_t() const { return lo; }

    constexpr bool operator==(UInt128 o) const { return lo == o.lo && hi == o.hi; }
    constexpr bool operator!=(UInt128 o) const { return !(*this == o); }
    constexpr bool operator<(UInt128 o) const {
        return hi < o.hi || (hi == o.hi && lo < o.lo);
    }
    constexpr bool operator<=(UInt128 o) const { return !(o < *this); }
    constexpr bool operator>(UInt128 o) const { return o < *this; }
    constexpr bool operator>=(UInt128 o) const { return !(*this < o); }

    constexpr UInt128 operator~() const { return UInt128(~lo, ~hi); }
    constexpr UInt128 operator&(UInt128 o) const { return UInt128(lo & o.lo, hi & o.hi); }
    constexpr UInt128 operator|(UInt128 o) const { return UInt128(lo | o.lo, hi | o.hi); }
    constexpr UInt128 operator^(UInt128 o) const { return UInt128(lo ^ o.lo, hi ^ o.hi); }

    constexpr UInt128 operator+(UInt128 o) const {
        const std::uint64_t nlo = lo + o.lo;
        return UInt128(nlo, hi + o.hi + (nlo < lo ? 1 : 0));
    }
    // Two's-complement negation, as `-x` on the unsigned builtin produces.
    constexpr UInt128 operator-() const {
        return UInt128(~lo + 1, ~hi + (lo == 0 ? 1 : 0));
    }
    constexpr UInt128 operator-(UInt128 o) const { return *this + (-o); }

    // Low 128 bits of the 128x128 product (the builtin wraps the same way).
    // 32-bit chunks keep every partial product inside 64 bits.
    constexpr UInt128 operator*(UInt128 o) const {
        const std::uint64_t al0 = lo & 0xFFFFFFFFULL;
        const std::uint64_t al1 = lo >> 32;
        const std::uint64_t bl0 = o.lo & 0xFFFFFFFFULL;
        const std::uint64_t bl1 = o.lo >> 32;
        const std::uint64_t p0 = al0 * bl0;
        const std::uint64_t p1 = al0 * bl1;
        const std::uint64_t p2 = al1 * bl0;
        const std::uint64_t p3 = al1 * bl1;
        const std::uint64_t mid =
            (p0 >> 32) + (p1 & 0xFFFFFFFFULL) + (p2 & 0xFFFFFFFFULL);
        return UInt128((p0 & 0xFFFFFFFFULL) | (mid << 32),
                       hi * o.lo + lo * o.hi + p3 + (p1 >> 32) + (p2 >> 32) +
                           (mid >> 32));
    }

    // 128/128 binary long division. Division by zero is meaningless, exactly
    // as with the builtin (callers guard against it); it returns zero here so
    // a stray call yields a defined value instead of a crash or garbage.
    static constexpr UInt128 divMod(UInt128 num, UInt128 den, UInt128& rem) {
        if (den.isZero()) {
            rem = UInt128(0, 0);
            return UInt128(0, 0);
        }
        UInt128 q(0, 0);
        UInt128 r(0, 0);
        for (int i = 0; i < 128; ++i) {
            const std::uint64_t top = (num.hi >> 63) & 1;
            num = num << 1;
            q = q << 1;
            r = (r << 1) | UInt128(top, 0);
            if (r >= den) {
                r = r - den;
                q = q | UInt128(1, 0);
            }
        }
        rem = r;
        return q;
    }
    constexpr UInt128 operator/(UInt128 o) const {
        UInt128 rem;
        return divMod(*this, o, rem);
    }
    constexpr UInt128 operator%(UInt128 o) const {
        UInt128 rem;
        divMod(*this, o, rem);
        return rem;
    }

    // Logical shifts. A count of 128 or more yields zero, matching the
    // guarded call sites (the builtin leaves that case undefined).
    constexpr UInt128 operator<<(unsigned n) const {
        if (n == 0) return *this;
        if (n < 64) return UInt128(lo << n, (hi << n) | (lo >> (64 - n)));
        if (n < 128) return UInt128(0, lo << (n - 64));
        return UInt128(0, 0);
    }
    constexpr UInt128 operator>>(unsigned n) const {
        if (n == 0) return *this;
        if (n < 64) return UInt128((lo >> n) | (hi << (64 - n)), hi >> n);
        if (n < 128) return UInt128(hi >> (n - 64), 0);
        return UInt128(0, 0);
    }
};

struct Int128 {
    std::uint64_t lo = 0;
    std::int64_t hi = 0;

    constexpr Int128() = default;
    constexpr Int128(std::uint64_t loWord, std::int64_t hiWord)
        : lo(loWord), hi(hiWord) {}

    // Implicit widening from any builtin integer with sign-extension, exactly
    // as converting to `__int128`.
    template <std::integral T>
    constexpr Int128(T v) : lo(static_cast<std::uint64_t>(v)), hi(0) {
        if constexpr (std::is_signed_v<T>) {
            if (v < 0) hi = -1;
        }
    }

    // Bit-preserving reinterpretation, mirroring `static_cast<__int128>(u128)`.
    explicit constexpr Int128(UInt128 v)
        : lo(v.lo), hi(static_cast<std::int64_t>(v.hi)) {}

    static constexpr Int128 fromBits(std::uint64_t loWord, std::int64_t hiWord) {
        return Int128(loWord, hiWord);
    }

    // The raw bit pattern as an unsigned value.
    constexpr UInt128 bits() const {
        return UInt128(lo, static_cast<std::uint64_t>(hi));
    }

    constexpr std::uint64_t low64() const { return lo; }
    constexpr std::int64_t high64() const { return hi; }
    constexpr bool isZero() const { return lo == 0 && hi == 0; }

    // Narrowing to the low word, mirroring `static_cast<std::int64_t>(i128)`.
    explicit constexpr operator std::int64_t() const {
        return static_cast<std::int64_t>(lo);
    }

    constexpr bool operator==(Int128 o) const { return lo == o.lo && hi == o.hi; }
    constexpr bool operator!=(Int128 o) const { return !(*this == o); }
    constexpr bool operator<(Int128 o) const {
        return hi < o.hi || (hi == o.hi && lo < o.lo);
    }
    constexpr bool operator<=(Int128 o) const { return !(o < *this); }
    constexpr bool operator>(Int128 o) const { return o < *this; }
    constexpr bool operator>=(Int128 o) const { return !(*this < o); }

    constexpr Int128 operator~() const { return Int128(~lo, ~hi); }
    constexpr Int128 operator&(Int128 o) const { return Int128(lo & o.lo, hi & o.hi); }
    constexpr Int128 operator|(Int128 o) const { return Int128(lo | o.lo, hi | o.hi); }
    constexpr Int128 operator^(Int128 o) const { return Int128(lo ^ o.lo, hi ^ o.hi); }

    // Add/subtract/multiply are the same bit operations as unsigned; only the
    // interpretation of the result differs.
    constexpr Int128 operator+(Int128 o) const { return Int128(bits() + o.bits()); }
    constexpr Int128 operator-() const { return Int128(UInt128(0, 0) - bits()); }
    constexpr Int128 operator-(Int128 o) const { return Int128(bits() - o.bits()); }
    constexpr Int128 operator*(Int128 o) const { return Int128(bits() * o.bits()); }

    constexpr Int128 operator<<(unsigned n) const { return Int128(bits() << n); }
    // Arithmetic right shift: vacated bits replicate the sign.
    constexpr Int128 operator>>(unsigned n) const {
        if (n == 0) return *this;
        if (n < 64) {
            return Int128((lo >> n) | (static_cast<std::uint64_t>(hi) << (64 - n)),
                          hi >> n);
        }
        if (n < 128) {
            return Int128(static_cast<std::uint64_t>(hi >> (n - 64)), hi >> 63);
        }
        const std::int64_t sign = hi >> 63;
        return Int128(static_cast<std::uint64_t>(sign), sign);
    }
};

constexpr UInt128::UInt128(Int128 v)
    : lo(v.lo), hi(static_cast<std::uint64_t>(v.hi)) {}

}  // namespace Utilities
