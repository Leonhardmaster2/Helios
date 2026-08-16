// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// SHA-256 per FIPS 180-4. Implementation is straightforward and
// intentionally not optimized for throughput; correctness is verified
// against the NIST vectors in tests.
#include "helios/core/hash.hpp"

#include "helios/core/io.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace helios::core {

namespace {
constexpr std::uint32_t kK[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

inline std::uint32_t rotr(std::uint32_t x, unsigned n) {
    return (x >> n) | (x << (32U - n));
}
} // namespace

Sha256::Sha256() {
    state_[0] = 0x6a09e667U;
    state_[1] = 0xbb67ae85U;
    state_[2] = 0x3c6ef372U;
    state_[3] = 0xa54ff53aU;
    state_[4] = 0x510e527fU;
    state_[5] = 0x9b05688cU;
    state_[6] = 0x1f83d9abU;
    state_[7] = 0x5be0cd19U;
}

void Sha256::transform(std::uint8_t const* block) {
    std::uint32_t w[64];
    for (unsigned i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24U) |
               (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16U) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8U) |
               static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (unsigned i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3U);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10U);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];

    for (unsigned i = 0; i < 64; ++i) {
        const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = h + s1 + ch + kK[i] + w[i];
        const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(void const* data, std::size_t size) {
    auto const* bytes = static_cast<std::uint8_t const*>(data);
    total_bits_ += static_cast<std::uint64_t>(size) * 8U;

    while (size > 0) {
        const std::size_t n = std::min(size, buffer_.size() - buffer_len_);
        std::memcpy(buffer_.data() + buffer_len_, bytes, n);
        buffer_len_ += n;
        bytes += n;
        size -= n;
        if (buffer_len_ == buffer_.size()) {
            transform(buffer_.data());
            buffer_len_ = 0;
        }
    }
}

std::array<std::uint8_t, Sha256::kDigestSize> Sha256::finalize() {
    // Pad: 0x80, zeros, 64-bit big-endian bit length.
    const std::uint64_t bits = total_bits_;
    const std::uint8_t one = 0x80;
    update(&one, 1);
    std::array<std::uint8_t, 8> len{};
    for (unsigned i = 0; i < 8; ++i) {
        len[i] = static_cast<std::uint8_t>(bits >> (56U - i * 8U));
    }
    while (buffer_len_ != 56) {
        const std::uint8_t zero = 0;
        update(&zero, 1);
    }
    update(len.data(), len.size());

    std::array<std::uint8_t, kDigestSize> digest{};
    for (unsigned i = 0; i < 8; ++i) {
        digest[i * 4] = static_cast<std::uint8_t>(state_[i] >> 24U);
        digest[i * 4 + 1] = static_cast<std::uint8_t>(state_[i] >> 16U);
        digest[i * 4 + 2] = static_cast<std::uint8_t>(state_[i] >> 8U);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(state_[i]);
    }
    finalized_ = true;
    return digest;
}

std::string sha256Hex(void const* data, std::size_t size) {
    Sha256 h;
    h.update(data, size);
    const auto d = h.finalize();
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(Sha256::kDigestSize * 2);
    for (std::uint8_t b : d) {
        out.push_back(kHex[b >> 4U]);
        out.push_back(kHex[b & 0x0fU]);
    }
    return out;
}

std::string sha256Hex(std::string_view data) {
    return sha256Hex(data.data(), data.size());
}

Result<std::string> sha256File(std::string_view path) {
    auto data = readBinaryFile(path, /*max_size=*/std::numeric_limits<std::uint64_t>::max());
    if (data.failed()) {
        return data.error();
    }
    return sha256Hex(data.value().data(), data.value().size());
}

} // namespace helios::core
