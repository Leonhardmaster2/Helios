// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace helios {

// Version of the Helios engine (semantic). Milestone-0 skeleton: 0.1.0.
struct HeliosVersion {
    std::uint32_t major = 0;
    std::uint32_t minor = 1;
    std::uint32_t patch = 0;
};

// Current engine version.
[[nodiscard]] HeliosVersion heliosVersion() noexcept;

// Git commit the binaries were built from (HELIOS_GIT_HASH) or "unknown".
[[nodiscard]] std::string_view heliosGitHash() noexcept;

// Human-readable "major.minor.patch (hash)".
[[nodiscard]] std::string heliosVersionString();

} // namespace helios
