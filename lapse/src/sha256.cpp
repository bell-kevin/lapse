// lapse — a tiny time machine for any folder
// SPDX-License-Identifier: MIT

#include "sha256.hpp"

#include <cstring>

namespace lapse {

namespace {

constexpr std::uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline std::uint32_t rotr(std::uint32_t x, unsigned n) {
    return (x >> n) | (x << (32 - n));
}

} // namespace

void Sha256::reset() {
    h_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
          0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    total_len_ = 0;
    buf_len_ = 0;
}

void Sha256::compress(const std::uint8_t* p) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (std::uint32_t(p[i * 4]) << 24) | (std::uint32_t(p[i * 4 + 1]) << 16) |
               (std::uint32_t(p[i * 4 + 2]) << 8) | std::uint32_t(p[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
    std::uint32_t e = h_[4], f = h_[5], g = h_[6], h = h_[7];

    for (int i = 0; i < 64; ++i) {
        std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        std::uint32_t ch = (e & f) ^ (~e & g);
        std::uint32_t t1 = h + S1 + ch + K[i] + w[i];
        std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        std::uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
    h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
}

void Sha256::update(const void* data, std::size_t len) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    total_len_ += len;

    if (buf_len_ > 0) {
        std::size_t take = std::min(len, sizeof(buf_) - buf_len_);
        std::memcpy(buf_ + buf_len_, p, take);
        buf_len_ += take;
        p += take;
        len -= take;
        if (buf_len_ == sizeof(buf_)) {
            compress(buf_);
            buf_len_ = 0;
        }
    }
    while (len >= 64) {
        compress(p);
        p += 64;
        len -= 64;
    }
    if (len > 0) {
        std::memcpy(buf_, p, len);
        buf_len_ = len;
    }
}

std::array<std::uint8_t, 32> Sha256::finish() {
    std::uint64_t bit_len = total_len_ * 8;

    std::uint8_t pad = 0x80;
    update(&pad, 1);
    std::uint8_t zero = 0x00;
    while (buf_len_ != 56) update(&zero, 1);

    std::uint8_t len_be[8];
    for (int i = 0; i < 8; ++i)
        len_be[i] = std::uint8_t(bit_len >> (56 - i * 8));
    // update() would also bump total_len_, but we no longer care about it.
    update(len_be, 8);

    std::array<std::uint8_t, 32> out{};
    for (int i = 0; i < 8; ++i) {
        out[i * 4 + 0] = std::uint8_t(h_[i] >> 24);
        out[i * 4 + 1] = std::uint8_t(h_[i] >> 16);
        out[i * 4 + 2] = std::uint8_t(h_[i] >> 8);
        out[i * 4 + 3] = std::uint8_t(h_[i]);
    }
    return out;
}

std::string Sha256::to_hex(const std::array<std::uint8_t, 32>& d) {
    static const char* hexc = "0123456789abcdef";
    std::string s;
    s.reserve(64);
    for (auto b : d) {
        s.push_back(hexc[b >> 4]);
        s.push_back(hexc[b & 0xf]);
    }
    return s;
}

std::string Sha256::hex_digest(const void* data, std::size_t len) {
    Sha256 h;
    h.update(data, len);
    return to_hex(h.finish());
}

} // namespace lapse
