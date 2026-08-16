// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Checked IO primitives: bounded reads, atomic writes, validation helpers.
#pragma once

#include "helios/core/error.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace helios::core {

// Reads a whole file as bytes; fails when size exceeds max_size (OutOfRange).
[[nodiscard]] Result<std::vector<std::uint8_t>> readBinaryFile(std::string_view path,
                                                               std::uint64_t max_size = 64U * 1024U * 1024U);

// Reads a whole file as text (UTF-8 assumed; not validated).
[[nodiscard]] Result<std::string> readTextFile(std::string_view path,
                                               std::uint64_t max_size = 64U * 1024U * 1024U);

// Atomic write: writes to a temp file in the same directory, fsyncs (best
// effort), then renames over the destination. Never leaves a partial target.
[[nodiscard]] Result<void> writeFileAtomic(std::string_view path, void const* data, std::size_t size);

[[nodiscard]] Result<void> writeTextFileAtomic(std::string_view path, std::string_view text);

[[nodiscard]] bool fileExists(std::string_view path);

} // namespace helios::core
