// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// SHA-256 (FIPS 180-4) — self-contained implementation used for asset
// manifests and cache keys. Validated against NIST test vectors.
#pragma once

#include "helios/core/error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace helios::core {

class Sha256 {
public:
    static constexpr std::size_t kDigestSize = 32;

    Sha256();
    void update(void const* data, std::size_t size);
    // Finalizes and returns the digest. The object must not be reused.
    [[nodiscard]] std::array<std::uint8_t, kDigestSize> finalize();

private:
    void transform(std::uint8_t const* block);

    std::uint32_t state_[8] = {};
    std::uint64_t total_bits_ = 0;
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffer_len_ = 0;
    bool finalized_ = false;
};

// Hex digest of a byte range.
[[nodiscard]] std::string sha256Hex(void const* data, std::size_t size);
[[nodiscard]] std::string sha256Hex(std::string_view data);
// Hex digest of a file's contents (error when the file cannot be read).
[[nodiscard]] Result<std::string> sha256File(std::string_view path);

} // namespace helios::core
