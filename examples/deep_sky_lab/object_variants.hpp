// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Named variants of the simulated objects: physically motivated sets of
// initial conditions (and, for the black hole, of disk parameters), each
// after a class of real objects.  A variant is only a starting point; every
// parameter stays editable afterwards.  Index 0 of each table is the
// object's default.
//
// The neutron star's and the pulsar's states live in neutron_star_model.hpp.
#pragma once

#include "black_hole_renderer.hpp"
#include "galaxy_simulation.hpp"
#include "gas_scene.hpp"
#include "planet_system.hpp"

#include <cstddef>
#include <span>

namespace helios::examples {

struct VariantView {
    double distance;
    double elevation_deg;
    double azimuth_deg;
    double fov_deg;
};

struct ObjectVariantInfo {
    char const* key;         // command-line name
    char const* label;       // UI name
    char const* description; // what it is, what it is modelled on, what is assumed
    VariantView view;        // a view that shows it
    // Where the run is taken to: Myr (nebula, galaxy) or years (remnant);
    // unused by the black hole.
    double run_to;
    // Display exposure that suits it, EV.
    float exposure_ev;
};

[[nodiscard]] std::span<ObjectVariantInfo const> nebulaVariants() noexcept;
[[nodiscard]] std::span<ObjectVariantInfo const> remnantVariants() noexcept;
[[nodiscard]] std::span<ObjectVariantInfo const> galaxyVariants() noexcept;
[[nodiscard]] std::span<ObjectVariantInfo const> blackHoleVariants() noexcept;
[[nodiscard]] std::span<ObjectVariantInfo const> planetVariants() noexcept;

// The recipes.  `base` supplies what a variant does not define: the seed,
// the grid (the quality preset's), the galaxy's particle counts.
[[nodiscard]] NebulaRecipe nebulaVariantRecipe(std::size_t index, NebulaRecipe base) noexcept;
[[nodiscard]] RemnantRecipe remnantVariantRecipe(std::size_t index, RemnantRecipe base) noexcept;
[[nodiscard]] GalaxyRecipe galaxyVariantRecipe(std::size_t index, GalaxyRecipe base) noexcept;
// Sets the disk of `settings`; the camera, sky and display fields are left.
void applyBlackHoleVariant(std::size_t index, BlackHoleSettings& settings) noexcept;
// The planetary system of a planet variant; `seed` varies the terrain.
[[nodiscard]] PlanetSystem planetVariantSystem(std::size_t index, std::uint32_t seed) noexcept;

// Peak effective temperature of a Page-Thorne thin disk around a
// non-rotating black hole of `mass_solar` solar masses accreting at
// `eddington_ratio` of the Eddington rate (radiative efficiency 0.057).
[[nodiscard]] double blackHoleDiskPeakTemperatureK(double mass_solar, double eddington_ratio) noexcept;

} // namespace helios::examples
