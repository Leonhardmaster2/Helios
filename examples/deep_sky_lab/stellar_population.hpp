// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Light of a stellar population as it ages.
//
// The galaxy simulation tracks star particles, each standing for tens of
// thousands of solar masses of stars born together.  What such a particle
// looks like is not chosen: it is the sum over a Kroupa (2001) initial mass
// function of the stars still alive at that age,
//
//   - main-sequence stars as black bodies, with textbook mass-luminosity and
//     mass-radius relations and lifetimes fitted to stellar-evolution tracks;
//   - evolved stars through the fuel-consumption theorem (Renzini & Buzzoni
//     1986): stars leaving the main sequence at a known rate, each radiating
//     a known amount of energy as a giant or supergiant.
//
// Young populations come out blue and hundreds of times brighter per unit
// mass than old ones, which are yellow-orange; nothing else is needed to make
// spiral arms blue and bulges yellow.  Ionising photons, which light the H II
// regions, come from the same sum.
//
// Simplifications: solar composition, no binaries, black-body spectra, and
// one temperature per evolved phase.  The mass-to-light ratios land within
// about 30 % of population-synthesis codes over 3 Myr - 13 Gyr.
#pragma once

#include "helios/math/vec.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace helios::examples {

struct PopulationLight {
    // Linear Rec.709/sRGB light per solar mass of stars formed.  Its Rec.709
    // luminance is the photopic luminosity in units of the Sun's.
    math::Vec3d rgb{};
    // Hydrogen-ionising photons per second per solar mass formed.
    double ionising_rate = 0.0;
};

// `age_myr` is clamped to [0.01, 20000].
[[nodiscard]] PopulationLight populationLight(double age_myr);

// Photopic luminosity (solar units per solar mass formed) of `light`.
[[nodiscard]] double populationLuminosity(PopulationLight const& light) noexcept;

// Log-spaced lookup table for the GPU: rgb = light per solar mass formed,
// a = ionising photons per second per solar mass formed, in units of
// kIonisingRateUnit.
inline constexpr double kIonisingRateUnit = 1.0e46;

struct PopulationTable {
    double min_age_myr = 0.0;
    double max_age_myr = 0.0;
    std::uint32_t count = 0;
    std::vector<float> rgba{}; // count * 4

    [[nodiscard]] bool valid() const noexcept {
        return count >= 2 && rgba.size() == static_cast<std::size_t>(count) * 4U &&
               min_age_myr > 0.0 && max_age_myr > min_age_myr;
    }
};

[[nodiscard]] PopulationTable makePopulationTable(double min_age_myr, double max_age_myr,
                                                  std::uint32_t count);

// A single star on the main sequence, from the same relations.
struct StarLight {
    double temperature_k = 0.0;
    double luminosity = 0.0; // photopic, solar units
};
[[nodiscard]] StarLight mainSequenceStar(double mass_solar);

// Visible line emission of photo-ionised gas (an H II region) per
// kIonisingRateUnit of absorbed ionising photons, in the same light unit as
// PopulationLight::rgb.  Balmer lines follow from case B recombination at
// 10^4 K; the forbidden lines use ratios typical of H II regions in spiral
// disks.
[[nodiscard]] math::Vec3d ionisedGasLight() noexcept;

} // namespace helios::examples
