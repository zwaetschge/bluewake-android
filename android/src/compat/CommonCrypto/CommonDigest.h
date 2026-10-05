// Stand-in for Apple's CommonCrypto digest header on Android: the BlueWake disc
// importer (apple/ios/src/disc_import.c) needs only CC_SHA1 to verify
// main.dol's revision-0 hash. Self-contained SHA-1 (RFC 3174), no dependency.
#ifndef BW_LINUX_COMMONCRYPTO_SHIM_H
#define BW_LINUX_COMMONCRYPTO_SHIM_H

#include <stdint.h>
#include <string.h>

#define CC_SHA1_DIGEST_LENGTH 20
typedef uint32_t CC_LONG;

static inline void bw_sha1_block(uint32_t state[5], const uint8_t block[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i)
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    for (int i = 16; i < 80; ++i) {
        const uint32_t x = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
        w[i] = (x << 1) | (x >> 31);
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | ((~b) & d);            k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                       k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);     k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                       k = 0xCA62C1D6; }
        const uint32_t t = ((a << 5) | (a >> 27)) + f + e + k + w[i];
        e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = t;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
    memset(w, 0, sizeof w);
}

static inline unsigned char* CC_SHA1(const void* data, CC_LONG len, unsigned char* out) {
    uint32_t state[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint64_t total = (uint64_t)len << 3;
    const uint8_t* p = (const uint8_t*)data;
    while (len >= 64) {
        bw_sha1_block(state, p);
        p += 64;
        len -= 64;
    }
    uint8_t last[128];
    memcpy(last, p, len);
    last[len] = 0x80;
    const size_t padded = len + 1 <= 56 ? 64 : 128;
    memset(last + len + 1, 0, padded - len - 1 - 8);
    for (int i = 0; i < 8; ++i)
        last[padded - 1 - i] = (uint8_t)(total >> (8 * i));
    bw_sha1_block(state, last);
    if (padded == 128)
        bw_sha1_block(state, last + 64);
    for (int i = 0; i < 5; ++i) {
        out[i * 4] = (uint8_t)(state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)state[i];
    }
    return out;
}

#endif
