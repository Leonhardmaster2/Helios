// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// A planet with what surrounds it: its rings, its cloud layer and its moons,
// and the physics that ties them together (orbits from the planet's mass,
// ring gaps from the moons' resonances, the Roche limit).  Named systems
// after classes of real worlds.
#pragma once

#include "planet_surface.hpp"

#include "helios/math/vec.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace helios::examples {

struct PlanetClouds {
    bool enabled = true;
    double cover = 0.5;          // fraction of the sky covered
    // Earth's lowest cloud bases are a kilometre or two above the local
    // ground; referenced to the sea level, a 3 km base clears most of the
    // land (the mean land elevation is 800 m) while high plateaus sit in it.
    double base_m = 3000.0;
    double top_m = 7500.0;
    double extinction_per_m = 0.03; // of dense cloud (visibility of tens of metres)
    // Zonal drift, radians of longitude per second of planet time (20 m/s
    // at Earth's radius).
    double wind_rad_per_s = 3.1e-6;
    std::uint32_t seed = 11;
};

struct PlanetRings {
    bool enabled = false;
    double inner_m = 0.0;
    double outer_m = 0.0;
    // Normal optical depth of the densest zone (Saturn's B ring: 1.5 to 2.5).
    double peak_optical_depth = 1.8;
    double albedo = 0.55;                   // single-scattering albedo of the particles
    math::Vec3d tint{0.95, 0.90, 0.82};      // water ice with a little reddening
    double ringlet_contrast = 0.45;          // fine radial structure, 0..1
    std::uint32_t seed = 5;
};

struct PlanetMoon {
    PlanetBody body{};
    double orbit_radius_m = 0.0;
    double mass_ratio = 0.0;   // moon / planet
    double phase_rad = 0.0;    // orbital position at time zero
};

inline constexpr std::size_t kMaxPlanetMoons = 4;

struct PlanetSystem {
    PlanetBody body{};
    double mean_density_kg_m3 = 5514.0; // Earth's; sets the mass, hence the orbits
    double day_length_s = 86400.0;
    PlanetRings rings{};
    PlanetClouds clouds{};
    std::array<PlanetMoon, kMaxPlanetMoons> moons{};
    std::uint32_t moon_count = 0;
};

// G M of the planet, m^3 / s^2.
[[nodiscard]] double planetGravitationalParameter(PlanetSystem const& system) noexcept;

// Mean motion of a moon, rad/s, and its orbital angle at a time.
[[nodiscard]] double moonMeanMotion(PlanetSystem const& system, PlanetMoon const& moon) noexcept;
[[nodiscard]] double moonOrbitalAngle(PlanetSystem const& system, PlanetMoon const& moon,
                                      double time_s) noexcept;

// Roche limit for a fluid satellite of the given density (Roche 1849):
// 2.44 R (rho_planet / rho_moon)^(1/3).  Rings live inside it.
[[nodiscard]] double planetRocheLimit(PlanetSystem const& system, double moon_density_kg_m3) noexcept;

// Hill radius of a moon: a (m / 3M)^(1/3).
[[nodiscard]] double moonHillRadius(PlanetSystem const& system, PlanetMoon const& moon) noexcept;

// Normal optical depth of the rings over radius, `samples` values from the
// inner to the outer edge: the zones of the preset, gaps opened at the moons'
// inner Lindblad resonances (Goldreich & Tremaine 1978, 1980; width scaled
// as (m/M)^(2/3) a and calibrated to Mimas and the Cassini division) and
// along any moon's orbit inside the rings (a few Hill radii), and fine
// radial structure (a stand-in for the viscous overstability of dense rings,
// Schmit & Tscharnuter 1995).
[[nodiscard]] std::vector<float> planetRingOpticalDepth(PlanetSystem const& system, std::size_t samples);

// Named systems.
enum class PlanetSystemKind : std::uint8_t {
    EarthLike = 0,
    DesertWorld = 1,
    IceWorld = 2,
    RingedGasGiant = 3,
};
inline constexpr std::size_t kPlanetSystemKindCount = 4;
[[nodiscard]] PlanetSystem planetSystemPreset(PlanetSystemKind kind, std::uint32_t seed) noexcept;

} // namespace helios::examples
