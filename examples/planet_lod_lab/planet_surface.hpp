// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// The surface of a planet or moon: the parameters of its terrain, climate
// and colours, the same height function the GPU evaluates
// (shaders/lib/planet_noise.slang), in double precision for the camera and
// the level of detail, and named bodies after classes of real worlds.
#pragma once

#include "helios/math/vec.hpp"
#include "shaders/abi.hpp"

#include <array>
#include <cstdint>

namespace helios::examples {

// Everything that describes a body's surface.  Lengths in metres,
// temperatures in kelvin, colours as linear sRGB albedo.
struct PlanetBody {
    double radius_m = 6'371'000.0;
    double sea_level_m = 0.0;
    bool ocean = true;
    // Lattice frequency of octave 0 (cells per radian) and the highest
    // octave evaluated anywhere (cells of octave 22 are 6 m on Earth).
    double base_frequency = 1.5;
    std::uint32_t max_octaves = 23;

    double ocean_floor_m = -4200.0;
    double platform_m = 350.0;
    double land_bias = -0.08; // -1 all ocean .. +1 all land; -0.08 gives about 30 % land
    double base_roughness_m = 900.0;

    double mountain_amplitude_m = 5500.0;
    double belt_threshold = 0.18;
    double belt_width = 0.22;
    std::uint32_t ridge_first_octave = 3;

    // Amplitude of the first detail octave (130 km cells on Earth); the
    // octaves below fall by 0.62 each, the roughness of real terrain.
    double detail_amplitude_m = 1200.0;
    std::uint32_t detail_first_octave = 5;
    double detail_on_plains = 0.3;

    double crater_density = 0.0; // fraction of lattice cells with a crater
    double crater_depth_ratio = 0.2;
    double crater_rim_ratio = 0.04;
    std::uint32_t crater_first_octave = 3;

    double equator_temperature_k = 300.0;
    double pole_drop_k = 50.0;
    double lapse_rate_k_per_m = 0.0065;
    double humidity_bias = 0.1;
    std::uint32_t humidity_octave = 1;

    std::uint32_t seed = 7;

    math::Vec3d color_vegetation{0.045, 0.085, 0.030};
    math::Vec3d color_desert{0.36, 0.26, 0.15};
    double beach_height_m = 25.0;
    math::Vec3d color_rock{0.20, 0.17, 0.15};
    math::Vec3d color_snow{0.82, 0.84, 0.86};
    math::Vec3d color_ocean_deep{0.004, 0.012, 0.035};
    math::Vec3d color_ocean_shallow{0.02, 0.10, 0.12};
    double ocean_depth_scale_m = 60.0;
    double ocean_specular = 1.0;

    // Atmosphere: scattering coefficients at the surface (1/m) and scale
    // heights (m) of molecules (Rayleigh, with its 1/lambda^4 colour) and
    // aerosols (Mie), the aerosols' forward-scattering asymmetry, and where
    // the atmosphere is taken to end.  Earth's values by default (Bucholtz
    // 1995 for Rayleigh; the aerosol values of Bruneton & Neyret 2008).
    bool atmosphere = true;
    math::Vec3d rayleigh_beta{5.8e-6, 13.5e-6, 33.1e-6};
    double rayleigh_height_m = 8000.0;
    math::Vec3d mie_beta{2.1e-5, 2.1e-5, 2.1e-5};
    double mie_height_m = 1200.0;
    double mie_g = 0.76;
    double atmosphere_top_m = 100'000.0;

    // Gas giant: no terrain, a banded cloud deck instead.
    bool gas_giant = false;
    double bands_per_hemisphere = 7.0;
    double band_turbulence = 0.8;
    double storm_density = 0.3;
    math::Vec3d band_color_a{0.70, 0.60, 0.45};
    math::Vec3d band_color_b{0.86, 0.80, 0.68};
    math::Vec3d band_color_c{0.45, 0.33, 0.25};
};

// Equal when they render the same (every field reaches the GPU block).
[[nodiscard]] bool operator==(PlanetBody const& a, PlanetBody const& b) noexcept;

// GPU constant block of the body (shaders/lib/planet_noise.slang).
[[nodiscard]] shaders::PlanetBodyParams planetBodyParams(PlanetBody const& body) noexcept;

// Octaves a tile of the given quadtree level evaluates: six above the level,
// capped by the body's maximum.
[[nodiscard]] std::uint32_t planetOctavesForLevel(PlanetBody const& body, std::uint32_t level) noexcept;

// Height (m above the radius) at a direction, in double precision, with
// the first `octaves` octaves.  Matches the GPU to the rounding of its
// float32 arithmetic (millimetres).
[[nodiscard]] double planetHeight(PlanetBody const& body, math::Vec3d const& direction,
                                  std::uint32_t octaves) noexcept;

// The lattice anchor of a patch: cell and 32-bit fraction of octave 0 at a
// direction (the float32-rounded anchor the shader subtracts).
struct PlanetLatticeAnchor {
    std::array<std::int32_t, 3> cell{};
    std::array<std::uint32_t, 3> frac{};
};
[[nodiscard]] PlanetLatticeAnchor planetLatticeAnchor(PlanetBody const& body,
                                                      std::array<float, 3> const& anchor_direction) noexcept;

// Named bodies.
enum class PlanetBodyKind : std::uint8_t {
    EarthLike = 0,
    DesertWorld = 1,
    IceWorld = 2,
    GasGiant = 3,
    RockyMoon = 4, // airless, cratered (the Moon)
    IcyMoon = 5,   // bright ice with craters (Enceladus-like)
};
[[nodiscard]] PlanetBody planetBodyPreset(PlanetBodyKind kind, std::uint32_t seed) noexcept;

} // namespace helios::examples
