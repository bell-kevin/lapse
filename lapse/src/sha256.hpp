// lapse — a tiny time machine for any folder
// SPDX-License-Identifier: MIT
//
// Minimal, dependency-free SHA-256 (FIPS 180-4). Used to content-address
// file objects and snapshot manifests.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace lapse {

class Sha256 {
public:
    Sha256() { reset(); }

    void reset();
    void update(const void* data, std::size_t len);
    std::array<std::uint8_t, 32> finish();

    // Convenience: hash a whole buffer / string in one call, return lowercase hex.
    static std::string hex_digest(const void* data, std::size_t len);
    static std::string hex_digest(const std::string& s) {
        return hex_digest(s.data(), s.size());
    }
    static std::string to_hex(const std::array<std::uint8_t, 32>& d);

private:
    void compress(const std::uint8_t* block);

    std::array<std::uint32_t, 8> h_{};
    std::uint64_t total_len_ = 0;     // bytes processed
    std::uint8_t buf_[64];            // partial block
    std::size_t buf_len_ = 0;
};

} // namespace lapse
