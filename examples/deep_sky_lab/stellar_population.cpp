// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "stellar_population.hpp"

#include "helios/blackhole/blackbody.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace helios::examples {
namespace {

constexpr double kSolarTemperature = 5772.0; // K (IAU 2015 nominal)
constexpr double kSolarRadius = 6.957e8;     // m (IAU 2015 nominal)
constexpr double kMinimumMass = 0.08;        // hydrogen-burning limit, solar masses
constexpr double kMaximumMass = 120.0;
constexpr double kHottestStar = 50000.0;     // K; the O3 end of the main sequence
constexpr double kHydrogenEdge = 157802.0;   // h nu / k of 13.598 eV, in K

// Rec.709 luminance weights.
constexpr double kLumaR = 0.2126729;
constexpr double kLumaG = 0.7151522;
constexpr double kLumaB = 0.0721750;

// ---- single stars ----------------------------------------------------------

// Bolometric luminosity on the main sequence, solar units: the textbook
// piecewise power laws (e.g. Duric 2004), flattening above 20 solar masses as
// radiation pressure takes over (2.3e5 at 40 and 2.6e6 at 120 solar masses,
// as in Geneva zero-age models).  Continuous to a few per cent.
[[nodiscard]] double mainSequenceLuminosity(double mass) {
    if (mass < 0.43) {
        return 0.23 * std::pow(mass, 2.3);
    }
    if (mass < 2.0) {
        return std::pow(mass, 4.0);
    }
    if (mass < 20.0) {
        return 1.4 * std::pow(mass, 3.5);
    }
    return 5.0e4 * std::pow(mass / 20.0, 2.2);
}

[[nodiscard]] double mainSequenceRadius(double mass) {
    return std::pow(mass, mass < 1.0 ? 0.8 : 0.57);
}

// log10 of the main-sequence lifetime in years: 10 Gyr for the Sun, 20 Myr at
// 10 solar masses, 2.5 Myr at the top (a quadratic through Geneva-track
// lifetimes; flat above 100 solar masses, where the fit bottoms out).
[[nodiscard]] double logLifetimeYears(double mass) {
    double const x = std::min(std::log10(mass), 2.0);
    return 10.0 - 3.6 * x + 0.9 * x * x;
}

// Mass whose main-sequence lifetime equals `age_years`, capped at the top of
// the mass function.
[[nodiscard]] double turnoffMass(double age_years) {
    double const discriminant = 12.96 - 3.6 * (10.0 - std::log10(age_years));
    if (discriminant <= 0.0) {
        return kMaximumMass;
    }
    return std::min(std::pow(10.0, (3.6 - std::sqrt(discriminant)) / 1.8), kMaximumMass);
}

// Kroupa (2001) mass function, stars per unit mass per solar mass formed.
[[nodiscard]] double initialMassFunction(double mass) {
    // Normalised so that the integral of mass * imf over the whole range is 1.
    static const double normalisation = [] {
        double const low = (std::pow(0.5, 0.7) - std::pow(kMinimumMass, 0.7)) / 0.7;
        double const high = 0.5 * (std::pow(0.5, -0.3) - std::pow(kMaximumMass, -0.3)) / 0.3;
        return 1.0 / (low + high);
    }();
    return normalisation * (mass < 0.5 ? std::pow(mass, -1.3) : 0.5 * std::pow(mass, -2.3));
}

// Integral of x^2 / (exp(x) - 1) from `lower` to infinity.
[[nodiscard]] double photonIntegral(double lower) {
    double sum = 0.0;
    for (int n = 1; n <= 40; ++n) {
        double const k = static_cast<double>(n);
        double const term =
            std::exp(-k * lower) * (lower * lower / k + 2.0 * lower / (k * k) + 2.0 / (k * k * k));
        sum += term;
        if (term < 1.0e-12 * sum) {
            break;
        }
    }
    return sum;
}

// Hydrogen-ionising photons per second from a black-body sphere:
// 4 pi R^2 * (2 pi / c^2) (k T / h)^3 * integral.
[[nodiscard]] double ionisingRate(double radius_solar, double kelvin) {
    constexpr double kPhotonConstant = 3.8467e33; // 4 pi R_sun^2 * 2 pi (k/h)^3 / c^2
    return kPhotonConstant * radius_solar * radius_solar * kelvin * kelvin * kelvin *
           photonIntegral(kHydrogenEdge / kelvin);
}

struct SolarReference {
    double y = 0.0; // CIE Y of the solar surface, W sr^-1 m^-2
};

[[nodiscard]] SolarReference const& sun() {
    static const SolarReference reference{blackhole::blackbodyColor(kSolarTemperature).xyz[1]};
    return reference;
}

// Visible light of a black-body sphere in solar photopic luminosities.
[[nodiscard]] math::Vec3d sphereLight(double radius_solar, double kelvin) {
    return blackhole::blackbodyColor(kelvin).linearSrgb * (radius_solar * radius_solar / sun().y);
}

// Visible light per solar bolometric luminosity radiated at `kelvin`.
[[nodiscard]] math::Vec3d lightPerBolometric(double kelvin) {
    double const ratio = kSolarTemperature / kelvin;
    return blackhole::blackbodyColor(kelvin).linearSrgb * (ratio * ratio * ratio * ratio / sun().y);
}

struct Star {
    double mass = 0.0;
    double weight = 0.0; // imf * d(mass) of the trapezoid rule on the log grid
    math::Vec3d light{};
    double ionising = 0.0;
};

constexpr std::size_t kMassSamples = 1200;

struct MassGrid {
    std::array<Star, kMassSamples> stars{};
    double log_step = 0.0;
};

[[nodiscard]] Star makeStar(double mass) {
    Star star;
    star.mass = mass;
    double const luminosity = mainSequenceLuminosity(mass);
    double radius = mainSequenceRadius(mass);
    double kelvin = kSolarTemperature * std::pow(luminosity / (radius * radius), 0.25);
    if (kelvin > kHottestStar) {
        // The power laws overshoot at the top; keep the luminosity.
        kelvin = kHottestStar;
        double const ratio = kSolarTemperature / kelvin;
        radius = std::sqrt(luminosity) * ratio * ratio;
    }
    star.light = sphereLight(radius, kelvin);
    star.ionising = ionisingRate(radius, kelvin);
    return star;
}

[[nodiscard]] MassGrid const& massGrid() {
    static const MassGrid grid = [] {
        MassGrid result;
        result.log_step =
            std::log(kMaximumMass / kMinimumMass) / static_cast<double>(kMassSamples - 1);
        for (std::size_t index = 0; index < kMassSamples; ++index) {
            double const mass =
                kMinimumMass * std::exp(result.log_step * static_cast<double>(index));
            result.stars[index] = makeStar(mass);
            // d(mass) = mass d(ln mass).
            result.stars[index].weight = initialMassFunction(mass) * mass * result.log_step;
        }
        return result;
    }();
    return grid;
}

// Energy radiated after the main sequence, as a multiple of the energy
// radiated on it.  A solar-mass star burns about a tenth of its hydrogen on
// the main sequence and three to four times as much as a giant; massive stars
// burn most of their core on the main sequence.
[[nodiscard]] double evolvedEnergyRatio(double mass) {
    return std::clamp(3.5 * std::pow(mass, -1.2), 0.15, 3.5);
}

// Effective temperature at which that energy is radiated: red giants and the
// red clump, red supergiants, and above about 25 solar masses hot
// Wolf-Rayet-like stars that never turn red.
[[nodiscard]] double evolvedTemperature(double mass) {
    if (mass <= 2.0) {
        return 4400.0;
    }
    if (mass <= 8.0) {
        return 4400.0 - 400.0 * std::log(mass / 2.0) / std::log(4.0);
    }
    if (mass <= 23.0) {
        return 4000.0;
    }
    if (mass >= 27.0) {
        return 30000.0;
    }
    return 4000.0 * std::pow(30000.0 / 4000.0, (mass - 23.0) / 4.0);
}

} // namespace

PopulationLight populationLight(double age_myr) {
    PopulationLight result;
    double const age_years = std::clamp(age_myr, 0.01, 20000.0) * 1.0e6;
    double const turnoff = turnoffMass(age_years);
    MassGrid const& grid = massGrid();

    // Main sequence: every star below the turn-off, the last interval cut at
    // the turn-off itself.
    double const position = std::log(turnoff / kMinimumMass) / grid.log_step;
    auto const last = std::min(static_cast<std::size_t>(position), kMassSamples - 1);
    double const fraction = position - static_cast<double>(last);
    for (std::size_t index = 0; index <= last; ++index) {
        Star const& star = grid.stars[index];
        // Trapezoid end weights: half at the first sample, and the cut
        // interval beyond the last full sample.
        double weight = star.weight;
        if (index == 0) {
            weight *= 0.5;
        }
        if (index == last) {
            weight *= 0.5 + (last + 1 < kMassSamples ? fraction : 0.0);
        }
        result.rgb += star.light * weight;
        result.ionising_rate += star.ionising * weight;
    }

    // Evolved stars, if any star has left the main sequence yet.
    if (turnoff < kMaximumMass) {
        double const x = std::log10(turnoff);
        // |d(mass)/d(age)| at the turn-off, per year.
        double const leaving = turnoff / (age_years * std::max(3.6 - 1.8 * x, 0.4));
        double const lifetime = std::pow(10.0, logLifetimeYears(turnoff));
        double const energy =
            evolvedEnergyRatio(turnoff) * mainSequenceLuminosity(turnoff) * lifetime;
        double const bolometric = initialMassFunction(turnoff) * leaving * energy;
        double const kelvin = evolvedTemperature(turnoff);
        result.rgb += lightPerBolometric(kelvin) * bolometric;
        // Photons per second per solar luminosity radiated at `kelvin`.
        double const ratio = kSolarTemperature / kelvin;
        result.ionising_rate += bolometric * ionisingRate(1.0, kelvin) * ratio * ratio * ratio * ratio;
    }

    // A population is a sum of black bodies and therefore inside the sRGB
    // gamut except for the dimmest red dwarfs; clip without changing luminance.
    double const luminance = populationLuminosity(result);
    math::Vec3d clipped{std::max(result.rgb[0], 0.0), std::max(result.rgb[1], 0.0),
                        std::max(result.rgb[2], 0.0)};
    double const clipped_luminance = kLumaR * clipped[0] + kLumaG * clipped[1] + kLumaB * clipped[2];
    result.rgb = clipped_luminance > 0.0 ? clipped * (luminance / clipped_luminance) : clipped;
    return result;
}

StarLight mainSequenceStar(double mass_solar) {
    double const mass = std::clamp(mass_solar, kMinimumMass, kMaximumMass);
    double const luminosity = mainSequenceLuminosity(mass);
    double const radius = mainSequenceRadius(mass);
    StarLight star;
    star.temperature_k =
        std::min(kSolarTemperature * std::pow(luminosity / (radius * radius), 0.25), kHottestStar);
    math::Vec3d const light = makeStar(mass).light;
    star.luminosity = kLumaR * light[0] + kLumaG * light[1] + kLumaB * light[2];
    return star;
}

double populationLuminosity(PopulationLight const& light) noexcept {
    return kLumaR * light.rgb[0] + kLumaG * light.rgb[1] + kLumaB * light.rgb[2];
}

PopulationTable makePopulationTable(double min_age_myr, double max_age_myr, std::uint32_t count) {
    PopulationTable table;
    if (!std::isfinite(min_age_myr) || !std::isfinite(max_age_myr) || min_age_myr <= 0.0 ||
        max_age_myr <= min_age_myr || count < 2U || count > 65536U) {
        return table;
    }
    table.min_age_myr = min_age_myr;
    table.max_age_myr = max_age_myr;
    table.count = count;
    table.rgba.resize(static_cast<std::size_t>(count) * 4U);
    double const log_step = std::log(max_age_myr / min_age_myr) / static_cast<double>(count - 1U);
    for (std::uint32_t index = 0; index < count; ++index) {
        PopulationLight const light =
            populationLight(min_age_myr * std::exp(log_step * static_cast<double>(index)));
        std::size_t const offset = static_cast<std::size_t>(index) * 4U;
        table.rgba[offset + 0U] = static_cast<float>(light.rgb[0]);
        table.rgba[offset + 1U] = static_cast<float>(light.rgb[1]);
        table.rgba[offset + 2U] = static_cast<float>(light.rgb[2]);
        table.rgba[offset + 3U] = static_cast<float>(light.ionising_rate / kIonisingRateUnit);
    }
    return table;
}

math::Vec3d ionisedGasLight() noexcept {
    // Energy radiated per ionising photon absorbed, in watts per (photon/s).
    // Case B at 10^4 K: 0.45 H-alpha photons per recombination.
    constexpr double kHAlpha = 1.37e-19;
    struct Line {
        double wavelength_nm;
        double power; // relative to H-alpha
    };
    constexpr std::array<Line, 5> kLines{{
        {656.28, 1.0},        // H-alpha
        {486.13, 1.0 / 2.86}, // H-beta, the case B Balmer decrement
        {500.7, 0.45},        // [O III] 4959 + 5007
        {658.35, 0.40},       // [N II] 6548 + 6583
        {672.4, 0.20},        // [S II] 6716 + 6731
    }};
    math::Vec3d sum{};
    for (Line const& line : kLines) {
        sum += blackhole::monochromaticLinearSrgb(line.wavelength_nm) * line.power;
    }
    // The Sun on the same scale: its surface radiance over its whole disk.
    double const solar = 4.0 * std::numbers::pi * std::numbers::pi * kSolarRadius * kSolarRadius * sun().y;
    math::Vec3d const light = sum * (kHAlpha * kIonisingRateUnit / solar);
    // Saturated red lies outside sRGB; clip the sum, as for the stars.
    return {std::max(light[0], 0.0), std::max(light[1], 0.0), std::max(light[2], 0.0)};
}

} // namespace helios::examples
