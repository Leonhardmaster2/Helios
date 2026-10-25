// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// The bodies of the solar-system lab: the Sun and the eight planets as
// PlanetBody descriptions, plus their J2000 positions.  This header is
// deliberately free of Diligent/GPU types so the data and the body presets
// can be unit-tested without a device.
#pragma once

#include "planet_surface.hpp"
#include "solar_system_data.hpp"

#include "helios/math/vec.hpp"

#include <cstddef>
#include <cstdint>

namespace helios::examples {

inline constexpr std::size_t kSolarSystemBodyCount = 9; // Sun + eight planets

// 0 = Sun, 1 = Mercury ... 8 = Neptune.
[[nodiscard]] PlanetBody solarSystemBody(int index, std::uint32_t seed) noexcept;
[[nodiscard]] char const* solarSystemBodyName(int index) noexcept;
// Heliocentric position in metres, ecliptic J2000.
[[nodiscard]] math::Vec3d solarSystemBodyPosition(int index, double days_since_j2000) noexcept;
// Radius used on screen: the Sun and the planets have separate display
// scales because at true scale the planets are sub-pixel dots.
[[nodiscard]] double solarSystemDisplayRadius(int index, double planet_scale,
                                              double sun_scale) noexcept;

} // namespace helios::examples
