// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// helios_universe — Milestone 0 skeleton.
// Owns time, frame graph, ephemeris/rotation backends, body registry and
// Gaia runtime catalog index (docs/ARCHITECTURE.md). The Instant /
// FrameSnapshot contract and synthetic frame providers are Milestone 1
// work and deliberately absent from this skeleton.
#pragma once

#include <string_view>

namespace helios::universe {

// Module identity (used in reports and diagnostics).
inline constexpr std::string_view kModuleName = "helios_universe";
inline constexpr std::string_view kModuleScope =
    "time, frame graph, ephemeris/rotation backends, body registry, Gaia catalog index (M1/M7/M9)";

} // namespace helios::universe
