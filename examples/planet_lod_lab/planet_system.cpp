// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "planet_system.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace helios::examples {
namespace {

constexpr double kGravity = 6.674e-11; // m^3 kg^-1 s^-2

[[nodiscard]] std::uint32_t hash(std::uint32_t x) noexcept {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

// Value noise in [-1, 1] on a line.
[[nodiscard]] double lineNoise(double x, std::uint32_t seed) noexcept {
    double const i = std::floor(x);
    double const f = x - i;
    double const t = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    auto value = [&](double cell) {
        return static_cast<double>(hash(static_cast<std::uint32_t>(static_cast<std::int64_t>(cell)) ^ seed) >> 8) *
                   (2.0 / 16777216.0) -
               1.0;
    };
    return value(i) + (value(i + 1.0) - value(i)) * t;
}

[[nodiscard]] double smoothstep(double a, double b, double x) noexcept {
    double const t = std::clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

} // namespace

double planetGravitationalParameter(PlanetSystem const& system) noexcept {
    double const r = system.body.radius_m;
    return kGravity * system.mean_density_kg_m3 * (4.0 / 3.0) * std::numbers::pi * r * r * r;
}

double moonMeanMotion(PlanetSystem const& system, PlanetMoon const& moon) noexcept {
    double const a = std::max(moon.orbit_radius_m, 1.0);
    return std::sqrt(planetGravitationalParameter(system) * (1.0 + moon.mass_ratio) / (a * a * a));
}

double moonOrbitalAngle(PlanetSystem const& system, PlanetMoon const& moon, double time_s) noexcept {
    return std::remainder(moon.phase_rad + moonMeanMotion(system, moon) * time_s,
                          2.0 * std::numbers::pi);
}

double planetRocheLimit(PlanetSystem const& system, double moon_density_kg_m3) noexcept {
    return 2.44 * system.body.radius_m *
           std::cbrt(system.mean_density_kg_m3 / std::max(moon_density_kg_m3, 1.0));
}

double moonHillRadius(PlanetSystem const& /*system*/, PlanetMoon const& moon) noexcept {
    return moon.orbit_radius_m * std::cbrt(moon.mass_ratio / 3.0);
}

std::vector<float> planetRingOpticalDepth(PlanetSystem const& system, std::size_t samples) {
    std::vector<float> profile(std::max<std::size_t>(samples, 2), 0.0f);
    PlanetRings const& rings = system.rings;
    if (!rings.enabled || rings.outer_m <= rings.inner_m) {
        return profile;
    }
    double const span = rings.outer_m - rings.inner_m;
    for (std::size_t index = 0; index < profile.size(); ++index) {
        double const x = (static_cast<double>(index) + 0.5) / static_cast<double>(profile.size());
        double const r = rings.inner_m + x * span;
        // Three zones like Saturn's C, B and A rings: a thin inner zone, the
        // dense middle, a moderate outer zone, with soft edges.
        double tau = rings.peak_optical_depth *
                     (0.06 + 0.94 * smoothstep(0.22, 0.34, x) * (1.0 - 0.72 * smoothstep(0.55, 0.62, x)));
        tau *= smoothstep(0.0, 0.03, x) * (1.0 - smoothstep(0.96, 1.0, x));
        // Fine radial structure: ringlets at several scales.
        double texture = 1.0;
        double amplitude = rings.ringlet_contrast;
        for (int octave = 0; octave < 5; ++octave) {
            double const frequency = 24.0 * std::pow(2.0, octave);
            texture += amplitude * lineNoise(x * frequency, rings.seed + static_cast<std::uint32_t>(octave) * 977u);
            amplitude *= 0.55;
        }
        tau *= std::max(texture, 0.05);
        // Gaps: the moons' 2:1 and 3:2 inner Lindblad resonances, and the
        // orbit of any moon embedded in the rings.
        for (std::uint32_t m = 0; m < system.moon_count; ++m) {
            PlanetMoon const& moon = system.moons[m];
            if (moon.mass_ratio <= 0.0) {
                continue;
            }
            double const scale = std::cbrt(moon.mass_ratio * moon.mass_ratio);
            // Calibrated so that a Mimas-mass moon (m/M = 6.6e-8) opens a gap
            // of about 2 % of the resonance radius, the Cassini division.
            // The (m/M)^(2/3) scaling is the gap-opening criterion for small
            // satellites; for a large moon it would exceed the whole ring, so
            // a gap is capped at 4 % of its resonance radius.
            constexpr double kResonanceWidth = 1430.0;
            for (double const ratio : {std::cbrt(0.25), std::cbrt(4.0 / 9.0)}) {
                double const resonance = moon.orbit_radius_m * ratio;
                double const width = std::min(kResonanceWidth * scale, 0.04) * resonance * (ratio > 0.65 ? 0.3 : 1.0);
                double const distance = std::abs(r - resonance);
                tau *= 1.0 - 0.97 * (1.0 - smoothstep(0.7 * width, 1.0 * width, distance));
            }
            double const hill = moonHillRadius(system, moon);
            double const distance = std::abs(r - moon.orbit_radius_m);
            tau *= 1.0 - 0.995 * (1.0 - smoothstep(2.5 * hill, 3.5 * hill, distance));
        }
        profile[index] = static_cast<float>(std::max(tau, 0.0));
    }
    return profile;
}

PlanetSystem planetSystemPreset(PlanetSystemKind kind, std::uint32_t seed) noexcept {
    PlanetSystem s;
    switch (kind) {
    case PlanetSystemKind::EarthLike: {
        s.body = planetBodyPreset(PlanetBodyKind::EarthLike, seed);
        s.mean_density_kg_m3 = 5514.0;
        s.day_length_s = 86164.0;
        s.clouds.enabled = true;
        s.clouds.cover = 0.5;
        // The Moon: a tenth of a per cent of the planet's mass... 1.23 % in
        // fact, sixty radii out.
        PlanetMoon moon;
        moon.body = planetBodyPreset(PlanetBodyKind::RockyMoon, seed + 1);
        moon.orbit_radius_m = 384'400'000.0;
        moon.mass_ratio = 0.0123;
        moon.phase_rad = 0.8;
        s.moons[0] = moon;
        s.moon_count = 1;
        // Rings off; switch them on for a ringed Earth.
        s.rings.enabled = false;
        s.rings.inner_m = 1.4 * s.body.radius_m;
        s.rings.outer_m = 2.2 * s.body.radius_m;
        s.rings.peak_optical_depth = 0.8;
        break;
    }
    case PlanetSystemKind::DesertWorld: {
        s.body = planetBodyPreset(PlanetBodyKind::DesertWorld, seed);
        s.mean_density_kg_m3 = 3934.0;
        s.day_length_s = 88775.0;
        // Thin, high water-ice clouds.
        s.clouds.enabled = true;
        s.clouds.cover = 0.12;
        s.clouds.base_m = 15'000.0;
        s.clouds.top_m = 22'000.0;
        s.clouds.extinction_per_m = 0.004;
        // Two small captured bodies, close in.
        PlanetMoon inner;
        inner.body = planetBodyPreset(PlanetBodyKind::RockyMoon, seed + 2);
        inner.body.radius_m = 11'000.0;
        inner.body.crater_first_octave = 1;
        inner.body.mountain_amplitude_m = 400.0;
        inner.body.detail_amplitude_m = 60.0;
        inner.body.base_roughness_m = 150.0;
        inner.body.platform_m = 300.0;
        inner.body.ocean_floor_m = -300.0;
        inner.orbit_radius_m = 9'376'000.0;
        inner.mass_ratio = 1.65e-8;
        inner.phase_rad = 2.1;
        PlanetMoon outer = inner;
        outer.body.seed = seed + 3;
        outer.body.radius_m = 6'200.0;
        outer.orbit_radius_m = 23'463'000.0;
        outer.mass_ratio = 2.3e-9;
        outer.phase_rad = 4.0;
        s.moons[0] = inner;
        s.moons[1] = outer;
        s.moon_count = 2;
        s.rings.enabled = false;
        s.rings.inner_m = 1.3 * s.body.radius_m;
        s.rings.outer_m = 2.0 * s.body.radius_m;
        s.rings.peak_optical_depth = 0.3;
        break;
    }
    case PlanetSystemKind::IceWorld: {
        s.body = planetBodyPreset(PlanetBodyKind::IceWorld, seed);
        s.mean_density_kg_m3 = 2000.0;
        s.day_length_s = 5.0 * 86400.0;
        s.clouds.enabled = false;
        PlanetMoon moon;
        moon.body = planetBodyPreset(PlanetBodyKind::IcyMoon, seed + 4);
        moon.orbit_radius_m = 12.0 * s.body.radius_m;
        moon.mass_ratio = 1.0e-3;
        moon.phase_rad = 1.5;
        s.moons[0] = moon;
        s.moon_count = 1;
        s.rings.enabled = false;
        s.rings.inner_m = 1.3 * s.body.radius_m;
        s.rings.outer_m = 2.1 * s.body.radius_m;
        s.rings.peak_optical_depth = 0.4;
        break;
    }
    case PlanetSystemKind::RingedGasGiant: {
        s.body = planetBodyPreset(PlanetBodyKind::GasGiant, seed);
        s.mean_density_kg_m3 = 687.0;    // Saturn's
        s.day_length_s = 38'018.0;
        s.clouds.enabled = false;        // the bands are the clouds
        double const radius = s.body.radius_m;
        s.rings.enabled = true;
        s.rings.inner_m = 1.24 * radius; // the inner edge of the C ring
        s.rings.outer_m = 2.27 * radius; // the outer edge of the A ring
        s.rings.peak_optical_depth = 1.8;
        s.rings.albedo = 0.55;
        // Four icy moons: the first two shape the rings (their 2:1 resonances
        // fall in them), the last is a large one far out.
        struct Spec {
            double orbit_radii;
            double mass_ratio;
            double radius_m;
            double phase;
        };
        constexpr std::array<Spec, 4> kMoons{{{3.19, 6.6e-8, 198'000.0, 0.3},
                                              {4.09, 1.9e-7, 252'000.0, 2.4},
                                              {5.06, 1.09e-6, 531'000.0, 4.1},
                                              {21.0, 2.37e-4, 2'575'000.0, 1.1}}};
        for (std::size_t m = 0; m < kMoons.size(); ++m) {
            PlanetMoon moon;
            moon.body = planetBodyPreset(PlanetBodyKind::IcyMoon, seed + 10 + static_cast<std::uint32_t>(m));
            moon.body.radius_m = kMoons[m].radius_m;
            moon.orbit_radius_m = kMoons[m].orbit_radii * radius;
            moon.mass_ratio = kMoons[m].mass_ratio;
            moon.phase_rad = kMoons[m].phase;
            s.moons[m] = moon;
        }
        // The far moon is a world of its own: dark, hazy orange.
        s.moons[3].body.color_desert = {0.55, 0.38, 0.18};
        s.moons[3].body.color_rock = {0.40, 0.28, 0.14};
        s.moons[3].body.color_snow = {0.55, 0.38, 0.18};
        s.moons[3].body.crater_density = 0.05;
        s.moon_count = 4;
        break;
    }
    }
    return s;
}

} // namespace helios::examples
