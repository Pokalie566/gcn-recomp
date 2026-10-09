// SHA-1, for checking that a disc's main.dol is the one a game was built for (FIPS 180-4).
// Not for anything that needs a secure hash: a known-good file is all it identifies.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

class Sha1 {
public:
    void update(const void* data, size_t len) {
        const uint8_t* p = (const uint8_t*)data;
        total_ += len;
        while (len) {
            const size_t n = std::min(len, (size_t)64 - fill_);
            memcpy(buf_ + fill_, p, n);
            fill_ += n; p += n; len -= n;
            if (fill_ == 64) { block(buf_); fill_ = 0; }
        }
    }
    // The digest as 40 lowercase hex digits. Ends the hash.
    std::string hex() {
        const uint64_t bits = total_ * 8;
        const uint8_t pad = 0x80;
        update(&pad, 1);
        const uint8_t zero = 0;
        while (fill_ != 56) update(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
        update(len, 8);
        char out[41];
        for (int i = 0; i < 5; i++) snprintf(out + 8 * i, 9, "%08x", h_[i]);
        return out;
    }

private:
    static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
    void block(const uint8_t* b) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)b[4 * i] << 24 | (uint32_t)b[4 * i + 1] << 16 | (uint32_t)b[4 * i + 2] << 8 | b[4 * i + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h_[0], bb = h_[1], c = h_[2], d = h_[3], e = h_[4];
        for (int i = 0; i < 80; i++) {
            const uint32_t f = i < 20 ? (bb & c) | (~bb & d) : i < 40 ? bb ^ c ^ d : i < 60 ? (bb & c) | (bb & d) | (c & d) : bb ^ c ^ d;
            const uint32_t k = i < 20 ? 0x5A827999u : i < 40 ? 0x6ED9EBA1u : i < 60 ? 0x8F1BBCDCu : 0xCA62C1D6u;
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(bb, 30); bb = a; a = t;
        }
        h_[0] += a; h_[1] += bb; h_[2] += c; h_[3] += d; h_[4] += e;
    }
    uint32_t h_[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    uint8_t buf_[64];
    size_t fill_ = 0;
    uint64_t total_ = 0;
};
