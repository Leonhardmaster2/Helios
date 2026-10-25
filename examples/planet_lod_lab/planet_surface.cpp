// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "planet_surface.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace helios::examples {
namespace {

// ---- the lattice, bit-identical to shaders/lib/planet_noise.slang ----------------

struct U3 {
    std::uint32_t x, y, z;
};

[[nodiscard]] U3 pcg3d(U3 v) noexcept {
    v.x = v.x * 1664525u + 1013904223u;
    v.y = v.y * 1664525u + 1013904223u;
    v.z = v.z * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v.x ^= v.x >> 16u;
    v.y ^= v.y >> 16u;
    v.z ^= v.z >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v;
}

[[nodiscard]] U3 latticeHash(std::int32_t x, std::int32_t y, std::int32_t z, std::uint32_t seed,
                             std::uint32_t octave) noexcept {
    U3 v{static_cast<std::uint32_t>(x) + seed,
         static_cast<std::uint32_t>(y) + seed * 0x9E3779B9u + octave * 0x85EBCA6Bu,
         static_cast<std::uint32_t>(z) + octave * 0xC2B2AE35u + 0x27D4EB2Fu};
    return pcg3d(v);
}

[[nodiscard]] double latticeValue(std::int32_t x, std::int32_t y, std::int32_t z, std::uint32_t seed,
                                  std::uint32_t octave) noexcept {
    return static_cast<double>(latticeHash(x, y, z, seed, octave).x >> 8u) * (2.0 / 16777216.0) - 1.0;
}

[[nodiscard]] double valueNoise(math::Vec3d const& p, std::uint32_t seed, std::uint32_t octave) noexcept {
    double const ix = std::floor(p[0]);
    double const iy = std::floor(p[1]);
    double const iz = std::floor(p[2]);
    auto const cx = static_cast<std::int32_t>(ix);
    auto const cy = static_cast<std::int32_t>(iy);
    auto const cz = static_cast<std::int32_t>(iz);
    auto fade = [](double f) { return f * f * f * (f * (f * 6.0 - 15.0) + 10.0); };
    double const ux = fade(p[0] - ix);
    double const uy = fade(p[1] - iy);
    double const uz = fade(p[2] - iz);
    double const a = latticeValue(cx, cy, cz, seed, octave);
    double const b = latticeValue(cx + 1, cy, cz, seed, octave);
    double const c = latticeValue(cx, cy + 1, cz, seed, octave);
    double const d = latticeValue(cx + 1, cy + 1, cz, seed, octave);
    double const e = latticeValue(cx, cy, cz + 1, seed, octave);
    double const f = latticeValue(cx + 1, cy, cz + 1, seed, octave);
    double const g = latticeValue(cx, cy + 1, cz + 1, seed, octave);
    double const h = latticeValue(cx + 1, cy + 1, cz + 1, seed, octave);
    double const k1 = b - a;
    double const k2 = c - a;
    double const k3 = e - a;
    double const k4 = a - b - c + d;
    double const k5 = a - c - e + g;
    double const k6 = a - b - e + f;
    double const k7 = -a + b + c - d + e - f - g + h;
    return a + k1 * ux + k2 * uy + k3 * uz + k4 * ux * uy + k5 * uy * uz + k6 * uz * ux +
           k7 * ux * uy * uz;
}

[[nodiscard]] double octaveNoise(PlanetBody const& body, math::Vec3d const& direction,
                                 std::uint32_t k, std::uint32_t seed) noexcept {
    double const frequency = body.base_frequency * static_cast<double>(1u << k);
    return valueNoise(direction * frequency, seed, k);
}

[[nodiscard]] double fbm(PlanetBody const& body, math::Vec3d const& direction, std::uint32_t first,
                         std::uint32_t last, std::uint32_t seed, double gain) noexcept {
    double value = 0.0;
    double amplitude = 1.0;
    for (std::uint32_t k = first; k < last; ++k) {
        value += amplitude * octaveNoise(body, direction, k, seed);
        amplitude *= gain;
    }
    return value;
}

[[nodiscard]] double ridged(PlanetBody const& body, math::Vec3d const& direction, std::uint32_t first,
                            std::uint32_t last, std::uint32_t seed) noexcept {
    double value = 0.0;
    double amplitude = 0.5;
    double weight = 1.0;
    for (std::uint32_t k = first; k < last; ++k) {
        double const n = octaveNoise(body, direction, k, seed);
        double const r = 1.0 - std::abs(n);
        double const ridge = r * r;
        value += amplitude * weight * ridge;
        weight = std::min(ridge * 1.6, 1.0);
        amplitude *= 0.5;
    }
    return value;
}

[[nodiscard]] double smoothstep(double edge0, double edge1, double x) noexcept {
    double const t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

[[nodiscard]] double unit(std::uint32_t bits) noexcept {
    return static_cast<double>(bits >> 8u) * (1.0 / 16777216.0);
}

[[nodiscard]] double craters(PlanetBody const& body, math::Vec3d const& direction, std::uint32_t k,
                             math::Vec3d const& shift, std::uint32_t seed) noexcept {
    double const frequency = body.base_frequency * static_cast<double>(1u << k);
    math::Vec3d const p = direction * frequency + shift;
    math::Vec3d const i{std::floor(p[0]), std::floor(p[1]), std::floor(p[2])};
    U3 const h = latticeHash(static_cast<std::int32_t>(i[0]), static_cast<std::int32_t>(i[1]),
                             static_cast<std::int32_t>(i[2]), seed, k);
    double const u0 = unit(h.x);
    if (u0 >= body.crater_density) {
        return 0.0;
    }
    math::Vec3d const centre{0.25 + 0.5 * unit(h.y), 0.25 + 0.5 * unit(h.z),
                             0.25 + 0.5 * unit(pcg3d(h).x)};
    double const radius =
        0.125 / std::sqrt(std::max(1.0 - 0.75 * u0 / std::max(body.crater_density, 1.0e-6), 0.25));
    math::Vec3d const offset = (p - i) - centre;
    double const rho = math::length(offset) / radius;
    if (rho >= 2.0) {
        return 0.0;
    }
    double const diameter_m = 2.0 * radius / frequency * body.radius_m;
    double const depth = body.crater_depth_ratio * diameter_m;
    double const rim = body.crater_rim_ratio * diameter_m;
    if (rho < 1.0) {
        double const rho2 = rho * rho;
        return depth * (rho2 - 1.0) + rim * rho2 * rho2;
    }
    return rim * std::exp(-3.0 * (rho - 1.0));
}

void store(float destination[4], math::Vec3d const& value, double w) noexcept {
    destination[0] = static_cast<float>(value[0]);
    destination[1] = static_cast<float>(value[1]);
    destination[2] = static_cast<float>(value[2]);
    destination[3] = static_cast<float>(w);
}

void set(float destination[4], double x, double y, double z, double w) noexcept {
    destination[0] = static_cast<float>(x);
    destination[1] = static_cast<float>(y);
    destination[2] = static_cast<float>(z);
    destination[3] = static_cast<float>(w);
}

} // namespace

bool operator==(PlanetBody const& a, PlanetBody const& b) noexcept {
    shaders::PlanetBodyParams const pa = planetBodyParams(a);
    shaders::PlanetBodyParams const pb = planetBodyParams(b);
    auto same = [](math::Vec3d const& x, math::Vec3d const& y) {
        return x[0] == y[0] && x[1] == y[1] && x[2] == y[2];
    };
    return std::memcmp(&pa, &pb, sizeof(pa)) == 0 && a.ocean == b.ocean &&
           a.gas_giant == b.gas_giant && a.star == b.star && a.max_octaves == b.max_octaves &&
           a.atmosphere == b.atmosphere && same(a.rayleigh_beta, b.rayleigh_beta) &&
           a.rayleigh_height_m == b.rayleigh_height_m && same(a.mie_beta, b.mie_beta) &&
           a.mie_height_m == b.mie_height_m && a.mie_g == b.mie_g &&
           a.atmosphere_top_m == b.atmosphere_top_m;
}

std::uint32_t planetOctavesForLevel(PlanetBody const& body, std::uint32_t level) noexcept {
    return std::min(6u + level, body.max_octaves);
}

double planetHeight(PlanetBody const& body, math::Vec3d const& direction, std::uint32_t octaves) noexcept {
    if (body.gas_giant || body.star) {
        return 0.0;
    }
    std::uint32_t const cap = std::min(octaves, body.max_octaves);
    std::uint32_t const s_continent = body.seed;
    std::uint32_t const s_belt = body.seed * 0x9E3779B9u + 1u;
    std::uint32_t const s_ridge = body.seed * 0x85EBCA6Bu + 2u;
    std::uint32_t const s_detail = body.seed * 0xC2B2AE35u + 3u;
    std::uint32_t const s_crater = body.seed * 0x27D4EB2Fu + 5u;

    constexpr double kWarp = 0.18;
    math::Vec3d const warped =
        direction + math::Vec3d{fbm(body, direction, 1, std::min(3u, cap), s_continent ^ 0x1B873593u, 0.5),
                                fbm(body, direction, 1, std::min(3u, cap), s_continent ^ 0xCC9E2D51u, 0.5),
                                fbm(body, direction, 1, std::min(3u, cap), s_continent ^ 0xE6546B64u, 0.5)} *
                        kWarp;
    double const c = fbm(body, warped, 0, std::min(7u, cap), s_continent, 0.6);
    double const land = smoothstep(-0.12, 0.12, c + body.land_bias);
    double height = body.ocean_floor_m + (body.platform_m - body.ocean_floor_m) * land +
                    body.base_roughness_m * c;

    double const b = fbm(body, direction, 1, std::min(4u, cap), s_belt, 0.5);
    double const belt =
        smoothstep(body.belt_threshold - body.belt_width, body.belt_threshold + body.belt_width, b) * land;

    if (body.mountain_amplitude_m > 0.0 && cap > body.ridge_first_octave) {
        double const r = ridged(body, direction, body.ridge_first_octave,
                                std::min(body.ridge_first_octave + 8u, cap), s_ridge);
        height += body.mountain_amplitude_m * r * r * belt;
    }
    if (body.detail_amplitude_m > 0.0 && cap > body.detail_first_octave) {
        double const d = fbm(body, direction, body.detail_first_octave, cap, s_detail, 0.62);
        double const keep = body.detail_on_plains + (1.0 - body.detail_on_plains) * belt;
        height += body.detail_amplitude_m * d * keep;
    }
    // Base level on continental crust (see planet_noise.slang).
    if (body.ocean) {
        double const crust = smoothstep(-0.05, 0.1, c + body.land_bias);
        double const excess = height - body.sea_level_m;
        constexpr double kWidth = 60.0;
        double const root = std::sqrt(excess * excess + kWidth * kWidth);
        double const floored = body.sea_level_m + 0.5 * (excess + root);
        height += (floored - height) * crust;
    }
    if (body.crater_density > 0.0 && cap > body.crater_first_octave) {
        for (std::uint32_t k = body.crater_first_octave; k < std::min(body.crater_first_octave + 6u, cap); ++k) {
            height += craters(body, direction, k, {0.0, 0.0, 0.0}, s_crater);
            height += craters(body, direction, k, {0.5, 0.5, 0.5}, s_crater ^ 0x5bd1e995u);
        }
    }
    return height;
}

PlanetLatticeAnchor planetLatticeAnchor(PlanetBody const& body,
                                        std::array<float, 3> const& anchor_direction) noexcept {
    PlanetLatticeAnchor anchor;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        double const p = static_cast<double>(anchor_direction[axis]) * body.base_frequency;
        double const cell = std::floor(p);
        anchor.cell[axis] = static_cast<std::int32_t>(cell);
        anchor.frac[axis] = static_cast<std::uint32_t>(std::min((p - cell) * 4294967296.0, 4294967295.0));
    }
    return anchor;
}

shaders::PlanetBodyParams planetBodyParams(PlanetBody const& body) noexcept {
    shaders::PlanetBodyParams g{};
    set(g.shape, body.radius_m, body.sea_level_m, body.base_frequency, body.max_octaves);
    set(g.continent, body.ocean_floor_m, body.platform_m, body.land_bias, body.base_roughness_m);
    set(g.mountain, body.mountain_amplitude_m, body.belt_threshold, body.belt_width,
        body.ridge_first_octave);
    set(g.detail, body.detail_amplitude_m, body.detail_first_octave, body.detail_on_plains,
        body.humidity_octave);
    set(g.crater, body.crater_density, body.crater_depth_ratio, body.crater_rim_ratio,
        body.crater_first_octave);
    set(g.climate, body.equator_temperature_k, body.pole_drop_k, body.lapse_rate_k_per_m,
        body.humidity_bias);
    g.seeds[0] = body.seed;
    g.seeds[1] = body.seed * 0x9E3779B9u + 1u;
    g.seeds[2] = body.seed * 0x85EBCA6Bu + 2u;
    g.seeds[3] = body.seed * 0xC2B2AE35u + 3u;
    g.seeds2[0] = body.seed * 0x165667B1u + 4u;
    g.seeds2[1] = body.seed * 0x27D4EB2Fu + 5u;
    g.seeds2[2] = body.seed * 0xD3A2646Cu + 6u;
    g.seeds2[3] = 0u;
    store(g.color_vegetation, body.color_vegetation, 0.0);
    store(g.color_desert, body.color_desert, body.beach_height_m);
    store(g.color_rock, body.color_rock, 0.0);
    store(g.color_snow, body.color_snow, body.ocean_depth_scale_m);
    store(g.color_ocean_deep, body.color_ocean_deep, body.ocean ? 1.0 : 0.0);
    store(g.color_ocean_shallow, body.color_ocean_shallow, body.ocean_specular);
    // bands.x is the surface kind: 0 terrain, 1 gas giant, 2 star.  The
    // shader branches on it before it reads the band parameters.
    set(g.bands, body.star ? 2.0 : (body.gas_giant ? 1.0 : 0.0), body.bands_per_hemisphere,
        body.band_turbulence, body.storm_density);
    store(g.band_color_a, body.band_color_a, 0.0);
    store(g.band_color_b, body.band_color_b, 0.0);
    store(g.band_color_c, body.band_color_c, 0.0);
    return g;
}

PlanetBody planetBodyPreset(PlanetBodyKind kind, std::uint32_t seed) noexcept {
    PlanetBody b;
    b.seed = seed;
    switch (kind) {
    case PlanetBodyKind::EarthLike:
        break; // the defaults: 6371 km, 30 % land, 300 K at the equator
    case PlanetBodyKind::DesertWorld:
        // Mars-sized and Mars-cold: no ocean (the sea level is far below
        // the lowest ground), a thin crust that carries higher mountains
        // for its gravity, dust-coloured plains, polar frost.
        b.radius_m = 3'389'500.0;
        b.ocean = false;
        b.sea_level_m = -20'000.0;
        b.ocean_floor_m = -3000.0;
        b.platform_m = 1500.0;
        b.land_bias = 0.05;
        b.base_roughness_m = 1500.0;
        b.mountain_amplitude_m = 9000.0;
        b.belt_threshold = 0.25;
        b.detail_amplitude_m = 500.0;
        b.detail_on_plains = 0.45;
        b.crater_density = 0.35;
        b.crater_first_octave = 4;
        b.equator_temperature_k = 240.0;
        b.pole_drop_k = 70.0;
        b.lapse_rate_k_per_m = 0.0025;
        b.humidity_bias = -1.0; // nothing grows
        b.color_desert = {0.42, 0.22, 0.11};
        b.color_rock = {0.28, 0.16, 0.10};
        b.color_snow = {0.85, 0.84, 0.82};
        // A thin carbon-dioxide atmosphere (a hundredth of Earth's surface
        // pressure) carrying dust that scatters reddish light and is mixed
        // through a deep layer: the butterscotch sky of Mars.
        b.rayleigh_beta = {0.9e-7, 2.0e-7, 4.6e-7};
        b.rayleigh_height_m = 11'000.0;
        b.mie_beta = {1.6e-5, 1.1e-5, 0.7e-5};
        b.mie_height_m = 11'000.0;
        b.mie_g = 0.7;
        b.atmosphere_top_m = 90'000.0;
        break;
    case PlanetBodyKind::IceWorld:
        // A frozen ocean world: the surface is ice over a buried ocean,
        // bright and nearly flat, scored by ridges (the cracks of a shifting
        // shell) and lightly cratered.
        b.radius_m = 2'600'000.0;
        b.ocean = false;
        b.sea_level_m = -20'000.0;
        b.ocean_floor_m = -300.0;
        b.platform_m = 200.0;
        b.land_bias = 0.0;
        b.base_roughness_m = 150.0;
        b.mountain_amplitude_m = 900.0;
        b.belt_threshold = 0.0;
        b.belt_width = 0.4;
        b.ridge_first_octave = 4;
        b.detail_amplitude_m = 60.0;
        b.detail_on_plains = 0.6;
        b.crater_density = 0.12;
        b.crater_first_octave = 5;
        b.equator_temperature_k = 110.0;
        b.pole_drop_k = 40.0;
        b.lapse_rate_k_per_m = 0.0;
        b.humidity_bias = -1.0;
        b.color_desert = {0.62, 0.66, 0.70};
        b.color_rock = {0.45, 0.50, 0.56};
        b.color_snow = {0.88, 0.90, 0.92};
        b.atmosphere = false; // a tenuous nitrogen atmosphere scatters too little to show
        break;
    case PlanetBodyKind::GasGiant:
        b.radius_m = 58'232'000.0; // Saturn's equatorial radius
        b.ocean = false;
        b.gas_giant = true;
        b.mountain_amplitude_m = 0.0;
        b.detail_amplitude_m = 0.0;
        b.base_roughness_m = 0.0;
        b.equator_temperature_k = 134.0;
        b.pole_drop_k = 10.0;
        b.lapse_rate_k_per_m = 0.0;
        // Hydrogen above the cloud deck: a scale height of 60 km and a few
        // hundred kilometres of haze-free gas make a thin blue limb.
        b.rayleigh_beta = {1.7e-6, 4.0e-6, 9.9e-6};
        b.rayleigh_height_m = 60'000.0;
        b.mie_beta = {2.0e-6, 2.0e-6, 2.0e-6};
        b.mie_height_m = 30'000.0;
        b.mie_g = 0.6;
        b.atmosphere_top_m = 600'000.0;
        break;
    case PlanetBodyKind::RockyMoon:
        // The Moon: airless, dark, saturated with craters, a few highland
        // massifs; no ocean, no climate zones.
        b.radius_m = 1'737'400.0;
        b.ocean = false;
        b.sea_level_m = -20'000.0;
        b.ocean_floor_m = -1500.0; // the maria: smoother, darker lowlands
        b.platform_m = 1500.0;
        b.land_bias = 0.3;
        b.base_roughness_m = 800.0;
        b.mountain_amplitude_m = 2500.0;
        b.belt_threshold = 0.3;
        b.detail_amplitude_m = 250.0;
        b.detail_on_plains = 0.35;
        b.crater_density = 0.55;
        b.crater_first_octave = 3;
        b.equator_temperature_k = 250.0;
        b.pole_drop_k = 60.0;
        b.lapse_rate_k_per_m = 0.0;
        b.humidity_bias = -1.0;
        b.color_desert = {0.12, 0.115, 0.11}; // highlands
        b.color_rock = {0.07, 0.068, 0.066};  // maria
        b.color_snow = {0.12, 0.115, 0.11};   // no frost: the same as the highlands
        b.atmosphere = false;
        break;
    case PlanetBodyKind::IcyMoon:
        // A small bright ice moon (Enceladus-like): high albedo, craters,
        // ridged terrain where the shell is young.
        b.radius_m = 252'000.0;
        b.ocean = false;
        b.sea_level_m = -20'000.0;
        b.ocean_floor_m = -200.0;
        b.platform_m = 200.0;
        b.land_bias = 0.0;
        b.base_roughness_m = 120.0;
        b.mountain_amplitude_m = 600.0;
        b.belt_threshold = 0.1;
        b.belt_width = 0.3;
        b.ridge_first_octave = 4;
        b.detail_amplitude_m = 40.0;
        b.detail_on_plains = 0.5;
        b.crater_density = 0.3;
        b.crater_first_octave = 4;
        b.equator_temperature_k = 75.0;
        b.pole_drop_k = 20.0;
        b.lapse_rate_k_per_m = 0.0;
        b.humidity_bias = -1.0;
        b.color_desert = {0.80, 0.84, 0.88};
        b.color_rock = {0.60, 0.66, 0.72};
        b.color_snow = {0.92, 0.94, 0.96};
        b.atmosphere = false;
        break;
    case PlanetBodyKind::Sun:
        // The Sun as an emissive body: no terrain, no atmosphere, no ocean.
        // The shader supplies the limb darkening and granulation; the preset
        // fixes the radius the quadtree and the camera use.
        b.radius_m = 695'700'000.0;
        b.ocean = false;
        b.star = true;
        b.gas_giant = false;
        b.atmosphere = false;
        b.mountain_amplitude_m = 0.0;
        b.detail_amplitude_m = 0.0;
        b.base_roughness_m = 0.0;
        b.crater_density = 0.0;
        b.equator_temperature_k = 5772.0;
        b.pole_drop_k = 0.0;
        b.lapse_rate_k_per_m = 0.0;
        b.humidity_bias = 0.0;
        break;
    }
    return b;
}

} // namespace helios::examples
