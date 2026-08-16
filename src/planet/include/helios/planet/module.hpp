// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// helios_planet — Milestone 0 skeleton.
// Owns body-local cube-sphere quadtree, screen-space-error selection, tile
// request policy, cache/domain state and generator descriptions
// (docs/ARCHITECTURE.md). Topology/LOD/residency are Milestones 2-4.
#pragma once

#include <string_view>

namespace helios::planet {

// Module identity (used in reports and diagnostics).
inline constexpr std::string_view kModuleName = "helios_planet";
inline constexpr std::string_view kModuleScope =
    "cube-sphere quadtree, SSE selection, tile requests, cache/domain state (M2-M4)";

} // namespace helios::planet
