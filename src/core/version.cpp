// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/version.hpp"

#include <format>
#include <string>

namespace helios {

#ifndef HELIOS_GIT_HASH
#define HELIOS_GIT_HASH "unknown"
#endif

HeliosVersion heliosVersion() noexcept {
    return HeliosVersion{};
}

std::string_view heliosGitHash() noexcept {
    return HELIOS_GIT_HASH;
}

std::string heliosVersionString() {
    const HeliosVersion v = heliosVersion();
    return std::format("{}.{}.{} ({})", v.major, v.minor, v.patch, heliosGitHash());
}

} // namespace helios
