#pragma once

#include <cstdint>
#include <string>

// Compile-time string hashing used by the backend to fold string-literal hashes
// (e.g. @hash("literal")). The result MUST stay bit-for-bit identical to the
// runtime hash emitted in src/compiler/runtime_core.cpp (__ins_hash_bytes) so a
// folded literal hash equals the runtime hash of the same bytes.
//
// The algorithm is a word-wise multiply / xor-shift mix that reads 8-byte
// little-endian words. Words are assembled from bytes explicitly here so the
// compile-time value is independent of the build host's endianness; the runtime
// reads native u64 words, which matches on little-endian targets (all currently
// supported targets are little-endian).

namespace Hashing {

inline constexpr std::uint64_t kPortableMul = 0x9E3779B97F4A7C15ULL;   // golden ratio
inline constexpr std::uint64_t kPortableSeed = 0xCBF29CE484222325ULL;  // FNV offset basis

inline std::uint64_t portableStringHash(const char* data, std::uint64_t len) {
    std::uint64_t h = kPortableSeed;
    h ^= len * kPortableMul;

    std::uint64_t i = 0;
    for (; i + 8 <= len; i += 8) {
        std::uint64_t k = 0;
        for (int b = 0; b < 8; ++b) {
            k |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[i + b]))
                 << (8 * b);
        }
        h ^= k;
        h *= kPortableMul;
        h ^= h >> 47;
    }

    std::uint64_t tail = 0;
    for (std::uint64_t s = 0; i < len; ++i, s += 8) {
        tail |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[i])) << s;
    }
    h ^= tail;
    h *= kPortableMul;
    h ^= h >> 47;

    // Final avalanche.
    h ^= h >> 32;
    h *= kPortableMul;
    h ^= h >> 29;
    return h;
}

inline std::uint64_t portableStringHash(const std::string& s) {
    return portableStringHash(s.data(), static_cast<std::uint64_t>(s.size()));
}

// --- AES fast-path hash (x86-64 hosted) ------------------------------------
// A GxHash-style hash built on the AES round function. The compile-time software
// implementation below MUST produce the same value as the hand-built runtime
// routine (__ins_hash_bytes_aes in module_emit.cpp), which uses the hardware
// AESENC instruction. Because AESENC computes exactly one FIPS-197 AES round
// (ShiftRows, SubBytes, MixColumns, then XOR round key), the software round here
// matches it bit-for-bit given the same byte layout (xmm byte i == state[i],
// little-endian; AES state is column-major state[row + 4*col]).
//
// Algorithm (must match the MIR routine exactly):
//   state = SEED ^ (len in the low 64 bits, high 64 bits zero)
//   for each full 16-byte block b:  state = aesenc(state, b)     // block as round key
//   tail (< 16 bytes, zero-padded): state = aesenc(state, tail)
//   finalize:                       state = aesenc(state, SEED) x2
//   result = low 64 bits of state (little-endian)

// 16-byte seed (pi fractional digits; low 64 then high 64, little-endian bytes).
inline constexpr std::uint8_t kAesSeed[16] = {
    0xD3, 0x08, 0xA3, 0x85, 0x88, 0x6A, 0x3F, 0x24,   // 0x243F6A8885A308D3
    0x44, 0x73, 0x70, 0x03, 0x2E, 0x8A, 0x19, 0x13,   // 0x13198A2E03707344
};

// Standard Rijndael (FIPS-197) S-box.
inline constexpr std::uint8_t kAesSBox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

inline std::uint8_t aesXtime(std::uint8_t x) {
    return static_cast<std::uint8_t>((x << 1) ^ ((x >> 7) * 0x1b));
}

// One AES round matching `aesenc state, key`.
inline void aesRound(std::uint8_t s[16], const std::uint8_t key[16]) {
    std::uint8_t t[16];
    for (int i = 0; i < 16; ++i) t[i] = kAesSBox[s[i]];        // SubBytes
    std::uint8_t r[16];                                        // ShiftRows
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
            r[row + 4 * c] = t[row + 4 * ((c + row) & 3)];
    for (int c = 0; c < 4; ++c) {                              // MixColumns
        std::uint8_t a0 = r[4 * c], a1 = r[4 * c + 1], a2 = r[4 * c + 2], a3 = r[4 * c + 3];
        s[4 * c + 0] = static_cast<std::uint8_t>(aesXtime(a0) ^ (aesXtime(a1) ^ a1) ^ a2 ^ a3);
        s[4 * c + 1] = static_cast<std::uint8_t>(a0 ^ aesXtime(a1) ^ (aesXtime(a2) ^ a2) ^ a3);
        s[4 * c + 2] = static_cast<std::uint8_t>(a0 ^ a1 ^ aesXtime(a2) ^ (aesXtime(a3) ^ a3));
        s[4 * c + 3] = static_cast<std::uint8_t>((aesXtime(a0) ^ a0) ^ a1 ^ a2 ^ aesXtime(a3));
    }
    for (int i = 0; i < 16; ++i) s[i] ^= key[i];              // AddRoundKey
}

inline std::uint64_t aesStringHash(const char* data, std::uint64_t len) {
    std::uint8_t state[16];
    for (int b = 0; b < 8; ++b)
        state[b] = static_cast<std::uint8_t>((len >> (8 * b)) & 0xff);   // len, low 64, LE
    for (int b = 8; b < 16; ++b) state[b] = 0;
    for (int i = 0; i < 16; ++i) state[i] ^= kAesSeed[i];                // ^ SEED

    std::uint64_t off = 0;
    std::uint8_t block[16];
    for (; off + 16 <= len; off += 16) {
        for (int i = 0; i < 16; ++i)
            block[i] = static_cast<std::uint8_t>(data[off + i]);
        aesRound(state, block);
    }
    std::uint8_t tail[16] = {0};
    for (std::uint64_t j = 0; off + j < len; ++j)
        tail[j] = static_cast<std::uint8_t>(data[off + j]);
    aesRound(state, tail);

    aesRound(state, kAesSeed);
    aesRound(state, kAesSeed);

    std::uint64_t h = 0;
    for (int b = 0; b < 8; ++b) h |= static_cast<std::uint64_t>(state[b]) << (8 * b);
    return h;
}

inline std::uint64_t aesStringHash(const std::string& s) {
    return aesStringHash(s.data(), static_cast<std::uint64_t>(s.size()));
}

}  // namespace Hashing
