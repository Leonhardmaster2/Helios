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
#ifndef HELIOS_VERSION_MAJOR
#define HELIOS_VERSION_MAJOR 0
#endif
#ifndef HELIOS_VERSION_MINOR
#define HELIOS_VERSION_MINOR 1
#endif
#ifndef HELIOS_VERSION_PATCH
#define HELIOS_VERSION_PATCH 0
#endif
    HeliosVersion v;
    v.major = HELIOS_VERSION_MAJOR;
    v.minor = HELIOS_VERSION_MINOR;
    v.patch = HELIOS_VERSION_PATCH;
    return v;
}

std::string_view heliosGitHash() noexcept {
    return HELIOS_GIT_HASH;
}

std::string heliosVersionString() {
    const HeliosVersion v = heliosVersion();
    return std::format("{}.{}.{} ({})", v.major, v.minor, v.patch, heliosGitHash());
}

} // namespace helios
