// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Debug-name generation for Vulkan objects and assets. Names are stable,
// bounded (Vulkan object names have implementation-defined limits) and
// unique per category via a per-category counter.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace helios::core {

// Returns "category[index]" — e.g. makeDebugName("image", 7) -> "image[7]".
[[nodiscard]] std::string makeDebugName(std::string_view category, std::uint32_t index);

// Returns "category[index]" with an automatic per-category counter.
// Thread-safe. Used for ad-hoc objects that do not carry their own index.
[[nodiscard]] std::string makeAutoDebugName(std::string_view category);

// Sanitizes a name for Vulkan (must not start with a digit or contain
// characters outside [a-zA-Z0-9_\-.]); returns "unnamed" when empty.
[[nodiscard]] std::string sanitizeDebugName(std::string_view name);

// Maximum recommended object-name length (Vulkan does not mandate a limit,
// but tools and validation behave better with bounded names).
inline constexpr std::size_t kMaxDebugNameLength = 128;

} // namespace helios::core
