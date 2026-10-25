// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "stellar_systems.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>

namespace helios::examples {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kSolarTemperature = 5772.0;

// ---- the mass function -----------------------------------------------------------
//
// Kroupa (2001) as in stellar_population.cpp: dN/dm = m^-1.3 below 0.5 solar
// masses and 0.5 m^-2.3 above, from 0.08 to 120.  Counts and masses of the
// two segments, in the same unnormalised units.
struct Imf {
    double low_count = 0.0;
    double high_count = 0.0;
    double total_count = 0.0;
    double mean_mass = 0.0;
};

[[nodiscard]] Imf const& imf() {
    static const Imf value = [] {
        Imf result;
        result.low_count = (std::pow(kImfMinimumMass, -0.3) - std::pow(0.5, -0.3)) / 0.3;
        result.high_count = 0.5 * (std::pow(0.5, -1.3) - std::pow(kImfMaximumMass, -1.3)) / 1.3;
        result.total_count = result.low_count + result.high_count;
        double const low_mass = (std::pow(0.5, 0.7) - std::pow(kImfMinimumMass, 0.7)) / 0.7;
        double const high_mass = 0.5 * (std::pow(0.5, -0.3) - std::pow(kImfMaximumMass, -0.3)) / 0.3;
        result.mean_mass = (low_mass + high_mass) / result.total_count;
        return result;
    }();
    return value;
}

// ---- single stars -------------------------------------------------------------------
//
// The relations of stellar_population.cpp, so that a population's light is
// the sum of its stars.

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

// Mass of a main-sequence star of this luminosity (the inverse of the above).
[[nodiscard]] double mainSequenceMass(double luminosity) {
    if (luminosity < 0.23 * std::pow(0.43, 2.3)) {
        return std::pow(luminosity / 0.23, 1.0 / 2.3);
    }
    if (luminosity < 16.0) {
        return std::pow(luminosity, 0.25);
    }
    if (luminosity < 5.0e4) {
        return std::pow(luminosity / 1.4, 1.0 / 3.5);
    }
    return 20.0 * std::pow(luminosity / 5.0e4, 1.0 / 2.2);
}

[[nodiscard]] double mainSequenceRadius(double mass) {
    return std::pow(mass, mass < 1.0 ? 0.8 : 0.57);
}

// Energy radiated after the main sequence over that radiated on it, and the
// temperature it is radiated at (stellar_population.cpp).
[[nodiscard]] double giantEnergyRatio(double mass) {
    return std::clamp(3.5 * std::pow(mass, -1.2), 0.15, 3.5);
}

[[nodiscard]] double giantTemperature(double mass) {
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

// On the giant branch the time spent per ln L falls as L^-0.75 (the slope of
// the observed red-giant luminosity function, d log N / d M_bol = 0.3): the
// luminosity at a fraction `phase` of the giant time, between the end of the
// main sequence and the tip.
constexpr double kGiantSlope = 0.75;

struct GiantBranch {
    double start = 0.0; // L at the end of the main sequence
    double tip = 0.0;
    double mean = 0.0;  // time-averaged L
};

[[nodiscard]] GiantBranch giantBranch(double mass) {
    GiantBranch branch;
    // Main-sequence stars brighten by half their mean luminosity from
    // beginning to end (L = L_ms (0.75 + 0.5 f)).
    branch.start = 1.25 * mainSequenceLuminosity(mass);
    // Low- and intermediate-mass stars climb to the tip of the red-giant and
    // asymptotic giant branches (2500 solar luminosities near one solar
    // mass, up to 10^4); massive stars cross at nearly constant luminosity.
    branch.tip = mass < 8.0 ? std::max(branch.start * 1.001, 2500.0 * std::clamp(mass / 1.4, 1.0, 4.0))
                            : branch.start * 1.3;
    double const a = std::pow(branch.start, -kGiantSlope);
    double const b = std::pow(branch.tip, -kGiantSlope);
    branch.mean = kGiantSlope / (a - b) *
                  (std::pow(branch.tip, 1.0 - kGiantSlope) - std::pow(branch.start, 1.0 - kGiantSlope)) /
                  (1.0 - kGiantSlope);
    return branch;
}

[[nodiscard]] double giantLuminosity(GiantBranch const& branch, double phase) {
    double const a = std::pow(branch.start, -kGiantSlope);
    double const b = std::pow(branch.tip, -kGiantSlope);
    return std::pow(a - std::clamp(phase, 0.0, 1.0) * (a - b), -1.0 / kGiantSlope);
}

[[nodiscard]] double radiusOf(double luminosity, double kelvin) {
    double const ratio = kSolarTemperature / kelvin;
    return std::sqrt(std::max(luminosity, 0.0)) * ratio * ratio;
}

[[nodiscard]] double temperatureOf(double luminosity, double radius) {
    return kSolarTemperature * std::pow(std::max(luminosity, 0.0) / (radius * radius), 0.25);
}

[[nodiscard]] double smoothstep(double edge0, double edge1, double x) {
    double const t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// Temperature on the giant branch at `phase`, from the end of the main
// sequence (`start_k`): low-mass stars cross the subgiant branch in the first
// tenth of the time and cool as they climb; massive stars cross to red
// supergiants (or stay hot above 27 solar masses).
[[nodiscard]] double giantTemperatureAt(double mass, double phase, double luminosity, double mean,
                                        double start_k) {
    double target = giantTemperature(mass);
    double crossing = 0.3;
    if (mass < 8.0) {
        target = std::clamp(target * std::pow(luminosity / mean, -0.05), 2800.0, 1.3 * target);
        crossing = 0.1;
    }
    double const s = smoothstep(0.0, crossing, phase);
    return std::exp(std::log(start_k) * (1.0 - s) + std::log(target) * s);
}

[[nodiscard]] double gaussianFrom(float u1, float u2) {
    return std::sqrt(-2.0 * std::log(static_cast<double>(u1))) * std::cos(2.0 * kPi * u2);
}

// Total lifetime (main sequence and giant) over log mass, for inverting.
struct LifetimeTable {
    static constexpr std::size_t kSamples = 1024;
    std::array<double, kSamples> log_mass{};
    std::array<double, kSamples> log_total{}; // decreasing
};

[[nodiscard]] LifetimeTable const& lifetimeTable() {
    static const LifetimeTable table = [] {
        LifetimeTable result;
        double const low = std::log(kImfMinimumMass);
        double const high = std::log(kImfMaximumMass);
        for (std::size_t index = 0; index < LifetimeTable::kSamples; ++index) {
            double const log_mass =
                low + (high - low) * static_cast<double>(index) / (LifetimeTable::kSamples - 1U);
            double const mass = std::exp(log_mass);
            result.log_mass[index] = log_mass;
            result.log_total[index] = std::log(mainSequenceMyr(mass) + giantMyr(mass));
        }
        // Monotone, so that it can be inverted.
        for (std::size_t index = 1; index < LifetimeTable::kSamples; ++index) {
            result.log_total[index] = std::min(result.log_total[index], result.log_total[index - 1U] - 1.0e-9);
        }
        return result;
    }();
    return table;
}

[[nodiscard]] std::uint32_t pcg(std::uint32_t value) noexcept {
    std::uint32_t const state = value * 747796405U + 2891336453U;
    std::uint32_t const word = ((state >> ((state >> 28U) + 4U)) ^ state) * 277803737U;
    return (word >> 22U) ^ word;
}

// Streams of the star hash (galaxy_resolved_stars.slang uses 0 to 4).
constexpr std::uint32_t kStreamMass = 0;
constexpr std::uint32_t kStreamPlace = 1; // 1 to 4
constexpr std::uint32_t kStreamMetal = 5; // 5, 6
constexpr std::uint32_t kStreamSystem = 16;

} // namespace

// ---- the mass function -------------------------------------------------------------

double imfMeanMass() noexcept {
    return imf().mean_mass;
}

std::uint32_t starsInParticle(double particle_mass_solar) noexcept {
    double const count = std::max(particle_mass_solar, 0.0) / imf().mean_mass;
    return static_cast<std::uint32_t>(std::clamp(std::round(count), 1.0, 1.0e9));
}

double imfTailFraction(double mass) noexcept {
    Imf const& f = imf();
    mass = std::clamp(mass, kImfMinimumMass, kImfMaximumMass);
    if (mass >= 0.5) {
        return 0.5 * (std::pow(mass, -1.3) - std::pow(kImfMaximumMass, -1.3)) / 1.3 / f.total_count;
    }
    return (f.high_count + (std::pow(mass, -0.3) - std::pow(0.5, -0.3)) / 0.3) / f.total_count;
}

double imfTailMass(double fraction) noexcept {
    Imf const& f = imf();
    double const count = std::clamp(fraction, 0.0, 1.0) * f.total_count;
    if (count <= f.high_count) {
        return std::clamp(std::pow(count * 1.3 / 0.5 + std::pow(kImfMaximumMass, -1.3), -1.0 / 1.3),
                          kImfMinimumMass, kImfMaximumMass);
    }
    return std::clamp(std::pow((count - f.high_count) * 0.3 + std::pow(0.5, -0.3), -1.0 / 0.3),
                      kImfMinimumMass, kImfMaximumMass);
}

// ---- single stars ----------------------------------------------------------------------

double mainSequenceMyr(double initial_mass) noexcept {
    double const x = std::min(std::log10(std::max(initial_mass, kImfMinimumMass)), 2.0);
    return std::pow(10.0, 10.0 - 3.6 * x + 0.9 * x * x - 6.0);
}

double giantMyr(double initial_mass) noexcept {
    double const mass = std::clamp(initial_mass, kImfMinimumMass, kImfMaximumMass);
    GiantBranch const branch = giantBranch(mass);
    // Energy radiated on the main sequence: L_ms times its lifetime.
    double const energy = giantEnergyRatio(mass) * mainSequenceLuminosity(mass) * mainSequenceMyr(mass);
    return energy / branch.mean;
}

StarState evolveStar(double initial_mass, double age_myr) noexcept {
    StarState star;
    double const mass = std::clamp(initial_mass, kImfMinimumMass, kImfMaximumMass);
    star.initial_mass = mass;
    star.mass = mass;
    star.age_myr = std::max(age_myr, 0.0);
    star.main_sequence_myr = mainSequenceMyr(mass);
    star.giant_myr = giantMyr(mass);
    double const ms_luminosity = mainSequenceLuminosity(mass);
    double const ms_radius = mainSequenceRadius(mass);
    // On the main sequence the star brightens and swells slowly.
    double const f = std::min(star.age_myr / star.main_sequence_myr, 1.0);
    double luminosity = ms_luminosity * (0.75 + 0.5 * f);
    double radius = ms_radius * (0.9 + 0.3 * f);
    double kelvin = temperatureOf(luminosity, radius);
    if (kelvin > 50000.0) {
        // The power laws overshoot at the top; keep the luminosity.
        kelvin = 50000.0;
        radius = radiusOf(luminosity, kelvin);
    }
    star.luminosity = luminosity;
    star.radius = radius;
    star.temperature_k = kelvin;
    star.largest_radius = radius;
    if (star.age_myr <= star.main_sequence_myr) {
        return star;
    }

    GiantBranch const branch = giantBranch(mass);
    double const end_radius = ms_radius * 1.2;
    double const end_kelvin = std::min(temperatureOf(branch.start, end_radius), 50000.0);
    double const giant_age = star.age_myr - star.main_sequence_myr;
    if (giant_age <= star.giant_myr) {
        double const phase = giant_age / star.giant_myr;
        star.phase = StarPhase::Giant;
        star.luminosity = giantLuminosity(branch, phase);
        star.temperature_k = giantTemperatureAt(mass, phase, star.luminosity, branch.mean, end_kelvin);
        star.radius = radiusOf(star.luminosity, star.temperature_k);
        star.largest_radius = std::max(star.radius, end_radius);
        // Stars lose mass as giants, most of it near the tip.
        double const remnant = mass < 8.0 ? std::min(0.109 * mass + 0.394, 1.35) : 0.35 * mass;
        star.mass = mass - (mass - remnant) * phase * phase;
        return star;
    }

    // The largest the star became: at the tip of its giant branch.
    double const tip_kelvin = giantTemperatureAt(mass, 1.0, branch.tip, branch.mean, end_kelvin);
    star.largest_radius = radiusOf(branch.tip, tip_kelvin);
    double const cooling_myr = std::max(giant_age - star.giant_myr, 1.0e-3);
    if (mass < 8.0) {
        // White dwarf: the initial-final mass relation of Kalirai et al.
        // (2008), Mestel cooling (L proportional to t^-7/5, 10^-3 solar at
        // 1 Gyr), and the radius of a cold carbon-oxygen dwarf.
        star.phase = StarPhase::WhiteDwarf;
        star.mass = std::min(0.109 * mass + 0.394, 1.35);
        star.radius = 0.0126 * std::pow(star.mass / 0.6, -1.0 / 3.0);
        star.luminosity = std::clamp(1.0e-3 * (star.mass / 0.6) * std::pow(cooling_myr / 1000.0, -1.4), 1.0e-6, 100.0);
        star.temperature_k = temperatureOf(star.luminosity, star.radius);
    } else if (mass < 20.0) {
        // Neutron star of 12 km, cooling from 10^6 K after 10^4 years.
        star.phase = StarPhase::NeutronStar;
        star.mass = 1.4;
        star.radius = 12.0 / 695700.0;
        star.temperature_k = std::clamp(1.0e6 * std::pow(cooling_myr / 0.01, -0.2), 1.0e4, 3.0e6);
        double const ratio = star.temperature_k / kSolarTemperature;
        star.luminosity = star.radius * star.radius * ratio * ratio * ratio * ratio;
    } else {
        star.phase = StarPhase::BlackHole;
        star.mass = std::clamp(0.3 * mass, 3.0, 40.0);
        star.radius = 4.244e-6 * star.mass; // Schwarzschild radius
        star.temperature_k = 0.0;
        star.luminosity = 0.0;
    }
    return star;
}

double turnoffMassAt(double age_myr) noexcept {
    // Inverts log10 t = 10 - 3.6 x + 0.9 x^2 (years), as stellar_population.
    double const years = std::max(age_myr, 1.0e-3) * 1.0e6;
    double const discriminant = 12.96 - 3.6 * (10.0 - std::log10(years));
    if (discriminant <= 0.0) {
        return kImfMaximumMass;
    }
    return std::clamp(std::pow(10.0, (3.6 - std::sqrt(discriminant)) / 1.8), kImfMinimumMass, kImfMaximumMass);
}

double deadMassAt(double age_myr) noexcept {
    LifetimeTable const& table = lifetimeTable();
    double const log_age = std::log(std::max(age_myr, 1.0e-6));
    if (log_age >= table.log_total.front()) {
        return kImfMinimumMass;
    }
    if (log_age <= table.log_total.back()) {
        return kImfMaximumMass;
    }
    // log_total decreases with the index.
    auto const it = std::lower_bound(table.log_total.begin(), table.log_total.end(), log_age,
                                     [](double a, double b) { return a > b; });
    auto const upper = static_cast<std::size_t>(it - table.log_total.begin());
    std::size_t const lower = upper - 1U;
    double const t = (log_age - table.log_total[lower]) / (table.log_total[upper] - table.log_total[lower]);
    return std::exp(table.log_mass[lower] + (table.log_mass[upper] - table.log_mass[lower]) * t);
}

// ---- stars of a particle -------------------------------------------------------------------

std::uint32_t starHash(std::uint32_t particle, std::uint32_t rank, std::uint32_t stream) noexcept {
    std::uint32_t const h = pcg(particle * 0x9E3779B1U + stream * 0x85EBCA77U);
    return pcg(h ^ (rank * 0xC2B2AE3DU + 0x27D4EB2FU));
}

float starUnit(std::uint32_t particle, std::uint32_t rank, std::uint32_t stream) noexcept {
    return (static_cast<float>(starHash(particle, rank, stream) >> 8U) + 0.5f) * (1.0f / 16777216.0f);
}

ParticleStars particleStars(std::uint32_t particle, double particle_mass_solar, double age_myr, double birth_myr,
                            bool bulge, double spread_pc, double thickness_pc) noexcept {
    ParticleStars stars;
    stars.particle = particle;
    stars.count = starsInParticle(particle_mass_solar);
    stars.age_myr = std::max(age_myr, 0.0);
    stars.birth_myr = birth_myr;
    stars.bulge = bulge;
    stars.spread_pc = spread_pc;
    stars.thickness_pc = thickness_pc;
    double const count = static_cast<double>(stars.count);
    // Ranks below a tail count are past a mass; the star at the boundary rank
    // is, too, if its place in its stratum puts it there.
    auto boundary = [&](double mass) {
        double const tail = imfTailFraction(mass) * count;
        double rank = std::floor(tail);
        if (rank < count && rank + starUnit(particle, static_cast<std::uint32_t>(rank), 0U) < tail) {
            rank += 1.0;
        }
        return rank;
    };
    double const dead = boundary(deadMassAt(stars.age_myr));
    double const left = boundary(turnoffMassAt(stars.age_myr));
    stars.dead = static_cast<std::uint32_t>(std::clamp(dead, 0.0, count - 1.0));
    stars.giants = static_cast<std::uint32_t>(std::clamp(left - dead, 0.0, count - 1.0 - dead));
    return stars;
}

std::uint32_t rankOfSlot(ParticleStars const& stars, std::uint32_t slot) noexcept {
    std::uint32_t const living = stars.count - stars.dead;
    return slot < living ? stars.dead + slot : slot - living;
}

std::uint32_t slotOfRank(ParticleStars const& stars, std::uint32_t rank) noexcept {
    std::uint32_t const living = stars.count - stars.dead;
    return rank >= stars.dead ? rank - stars.dead : living + rank;
}

double initialMassOfRank(ParticleStars const& stars, std::uint32_t rank) noexcept {
    double const u = starUnit(stars.particle, rank, kStreamMass);
    return imfTailMass((static_cast<double>(rank) + u) / static_cast<double>(std::max(stars.count, 1U)));
}

math::Vec3d starOffsetPc(ParticleStars const& stars, std::uint32_t rank) noexcept {
    float const u1 = starUnit(stars.particle, rank, kStreamPlace);
    float const u2 = starUnit(stars.particle, rank, kStreamPlace + 1U);
    float const u3 = starUnit(stars.particle, rank, kStreamPlace + 2U);
    float const u4 = starUnit(stars.particle, rank, kStreamPlace + 3U);
    double const radial = std::sqrt(-2.0 * std::log(static_cast<double>(u1)));
    double const angle = 2.0 * kPi * u2;
    return {stars.spread_pc * radial * std::cos(angle), stars.spread_pc * radial * std::sin(angle),
            stars.thickness_pc * gaussianFrom(u3, u4)};
}

double starMetallicity(ParticleStars const& stars, std::uint32_t rank) noexcept {
    double const scatter = gaussianFrom(starUnit(stars.particle, rank, kStreamMetal),
                                        starUnit(stars.particle, rank, kStreamMetal + 1U));
    if (stars.bulge) {
        // The bulge: old but metal-rich, with a wide spread (Zoccali et al. 2008).
        return std::clamp(-0.1 + 0.35 * scatter, -1.5, 0.6);
    }
    // The disk's age-metallicity relation: the galaxy formed 13 Gyr before
    // the run began; the gas reaches solar metallicity then and rises slowly.
    double const since_formation = stars.birth_myr + 13000.0;
    double const mean = std::clamp(std::log10((std::max(since_formation, 0.0) + 500.0) / 13500.0), -1.5, 0.35);
    return std::clamp(mean + 0.15 * scatter, -2.0, 0.6);
}

std::uint32_t slotsBrighterThan(ParticleStars const& stars, double luminosity) noexcept {
    std::uint32_t const living = stars.count - stars.dead;
    double const cut = mainSequenceMass(std::max(luminosity, 1.0e-8));
    double const turnoff = turnoffMassAt(stars.age_myr);
    if (cut >= turnoff) {
        return std::min(stars.giants, living);
    }
    double const brighter = std::floor(imfTailFraction(cut) * static_cast<double>(stars.count));
    auto const slots = static_cast<std::uint32_t>(std::max(brighter - static_cast<double>(stars.dead), 0.0));
    return std::clamp(slots, std::min(stars.giants, living), living);
}

// ---- light of the resolved stars -------------------------------------------------------------

namespace {
constexpr std::size_t kTableAges = 96;
constexpr std::size_t kTableFractions = 96;
constexpr double kTableYoungest = 0.5;
constexpr double kTableOldest = 20000.0;
constexpr double kTableSmallest = 1.0e-8;
} // namespace

ResolvedLightTable::ResolvedLightTable() {
    values_.assign(kTableAges * kTableFractions, 1.0f);
    constexpr std::size_t kSteps = 3000;
    std::vector<double> position(kSteps);
    std::vector<double> cumulative(kSteps);
    for (std::size_t age_index = 0; age_index < kTableAges; ++age_index) {
        double const age = kTableYoungest * std::pow(kTableOldest / kTableYoungest,
                                                     static_cast<double>(age_index) / (kTableAges - 1U));
        double const dead = imfTailFraction(deadMassAt(age));
        double const living = 1.0 - dead;
        if (living <= kTableSmallest) {
            continue;
        }
        // Light per unit of tail fraction over a log grid in the fraction
        // counted from the brightest living star.
        double total = 0.0;
        double previous_y = 0.0;
        double previous_light = 0.0;
        for (std::size_t step = 0; step < kSteps; ++step) {
            double const y = kTableSmallest * std::pow(living / kTableSmallest,
                                                       static_cast<double>(step) / (kSteps - 1U));
            double const light = evolveStar(imfTailMass(dead + y), age).luminosity;
            total += step == 0 ? light * y : 0.5 * (light + previous_light) * (y - previous_y);
            position[step] = y;
            cumulative[step] = total;
            previous_y = y;
            previous_light = light;
        }
        for (std::size_t fraction_index = 0; fraction_index < kTableFractions; ++fraction_index) {
            double const y = kTableSmallest * std::pow(1.0 / kTableSmallest,
                                                       static_cast<double>(fraction_index) / (kTableFractions - 1U));
            double value = 1.0;
            if (y < living) {
                auto const it = std::lower_bound(position.begin(), position.end(), y);
                auto const upper = std::clamp<std::size_t>(static_cast<std::size_t>(it - position.begin()), 1U, kSteps - 1U);
                double const t = (y - position[upper - 1U]) / (position[upper] - position[upper - 1U]);
                value = (cumulative[upper - 1U] + (cumulative[upper] - cumulative[upper - 1U]) * t) / total;
            }
            values_[age_index * kTableFractions + fraction_index] = static_cast<float>(std::clamp(value, 0.0, 1.0));
        }
    }
}

double ResolvedLightTable::fraction(double age_myr, double slot_fraction) const noexcept {
    if (slot_fraction <= 0.0) {
        return 0.0;
    }
    double const a = std::clamp(std::log(std::max(age_myr, kTableYoungest) / kTableYoungest) /
                                    std::log(kTableOldest / kTableYoungest) * (kTableAges - 1U),
                                0.0, static_cast<double>(kTableAges - 1U) - 1.0e-6);
    double const f = std::clamp(std::log(std::max(slot_fraction, kTableSmallest) / kTableSmallest) /
                                    std::log(1.0 / kTableSmallest) * (kTableFractions - 1U),
                                0.0, static_cast<double>(kTableFractions - 1U) - 1.0e-6);
    auto const ai = static_cast<std::size_t>(a);
    auto const fi = static_cast<std::size_t>(f);
    double const at = a - static_cast<double>(ai);
    double const ft = f - static_cast<double>(fi);
    auto value = [&](std::size_t i, std::size_t j) {
        return static_cast<double>(values_[i * kTableFractions + j]);
    };
    double const low = value(ai, fi) * (1.0 - ft) + value(ai, fi + 1U) * ft;
    double const high = value(ai + 1U, fi) * (1.0 - ft) + value(ai + 1U, fi + 1U) * ft;
    return low * (1.0 - at) + high * at;
}

// ---- planets and moons ---------------------------------------------------------------------

namespace {

constexpr double kEarthPerAu2Gcm2 = 0.03747; // Earth masses in 1 AU^2 at 1 g/cm^2
constexpr double kAuKm = 1.495978707e8;
constexpr double kEarthRadiusKm = 6371.0;
constexpr double kEarthMassKg = 5.9722e24;
constexpr double kSolarMassKg = 1.98892e30;
constexpr double kGravitySi = 6.674e-11;
constexpr double kDay = 86400.0;

// A deterministic stream of numbers for one system.
class Dice {
  public:
    explicit Dice(StarKey key) : key_(key) {}
    [[nodiscard]] double unit() { return starUnit(key_.particle, key_.rank, stream_++); }
    [[nodiscard]] double between(double low, double high) { return low + (high - low) * unit(); }
    [[nodiscard]] double logBetween(double low, double high) {
        return std::exp(std::log(low) + (std::log(high) - std::log(low)) * unit());
    }
    [[nodiscard]] double gaussian() {
        double const u1 = unit();
        double const u2 = unit();
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2);
    }

  private:
    StarKey key_;
    std::uint32_t stream_ = kStreamSystem;
};

// Orbit radius (AU) of a period (days) around a mass (solar masses).
[[nodiscard]] double orbitOfPeriod(double days, double mass) {
    double const years = days / 365.25;
    return std::cbrt(mass * years * years);
}

[[nodiscard]] double periodOfOrbit(double au, double mass) {
    return 365.25 * std::sqrt(au * au * au / std::max(mass, 1.0e-6));
}

// Chen & Kipping (2017): radius (Earth radii) from mass (Earth masses).
[[nodiscard]] double radiusOfMass(double mass) {
    if (mass < 2.04) {
        return 1.008 * std::pow(mass, 0.279);
    }
    if (mass < 131.6) {
        return 0.808 * std::pow(mass, 0.589);
    }
    return 17.74 * std::pow(mass, -0.044);
}

// Equilibrium temperature with full heat redistribution.
[[nodiscard]] double equilibriumTemperature(double luminosity, double au, double bond_albedo) {
    return 278.6 * std::pow(std::max(luminosity, 0.0), 0.25) / std::sqrt(au) * std::pow(1.0 - bond_albedo, 0.25);
}

// What a planet is and how it reflects, from its mass, where it formed and
// its temperature.  Reflectances are the disk-averaged colours of the bodies
// of the solar system and of the giant-planet classes of Sudarsky, Burrows &
// Hubeny (2000), whose cloud decks follow from the temperature.
void classify(Planet& planet, double luminosity, bool beyond_snow_line, bool envelope) {
    double const mass = planet.mass_earth;
    double const t = equilibriumTemperature(luminosity, planet.orbit_au, 0.3);
    if (mass >= 50.0) {
        planet.kind = PlanetKind::GasGiant;
        if (t < 150.0) { // class I: ammonia clouds (Jupiter, Saturn)
            planet.albedo = {0.62, 0.55, 0.44};
            planet.bond_albedo = 0.34;
        } else if (t < 300.0) { // class II: water clouds
            planet.albedo = {0.80, 0.80, 0.78};
            planet.bond_albedo = 0.75;
        } else if (t < 850.0) { // class III: clear, Rayleigh-blue
            planet.albedo = {0.10, 0.14, 0.24};
            planet.bond_albedo = 0.12;
        } else if (t < 1400.0) { // class IV: alkali-metal absorption
            planet.albedo = {0.03, 0.03, 0.035};
            planet.bond_albedo = 0.03;
        } else { // class V: silicate clouds
            planet.albedo = {0.55, 0.50, 0.48};
            planet.bond_albedo = 0.55;
        }
    } else if (envelope && beyond_snow_line && mass >= 5.0) {
        planet.kind = PlanetKind::IceGiant;
        if (t < 150.0) { // methane removes the red (Uranus, Neptune)
            planet.albedo = {0.36, 0.60, 0.70};
        } else {
            planet.albedo = {0.45, 0.50, 0.55};
        }
        planet.bond_albedo = 0.3;
    } else if (envelope && mass >= 2.0) {
        planet.kind = PlanetKind::SubNeptune;
        planet.albedo = {0.36, 0.38, 0.42};
        planet.bond_albedo = 0.3;
    } else if (t > 1100.0) {
        planet.kind = PlanetKind::Lava;
        planet.albedo = {0.10, 0.09, 0.08};
        planet.bond_albedo = 0.1;
    } else if (t > 290.0 && mass > 0.5) {
        planet.kind = PlanetKind::Cloudy; // a runaway greenhouse under sulphuric clouds (Venus)
        planet.albedo = {0.82, 0.76, 0.60};
        planet.bond_albedo = 0.76;
    } else if (t > 200.0 && mass > 0.3 && mass < 6.0) {
        planet.kind = PlanetKind::Temperate; // oceans and clouds (Earth)
        planet.albedo = {0.30, 0.33, 0.40};
        planet.bond_albedo = 0.30;
    } else if (t < 200.0 && beyond_snow_line) {
        planet.kind = PlanetKind::Icy;
        planet.albedo = {0.65, 0.67, 0.70};
        planet.bond_albedo = 0.6;
    } else {
        planet.kind = PlanetKind::Rocky;
        planet.albedo = t > 350.0 ? math::Vec3d{0.14, 0.13, 0.12}  // Mercury
                                  : math::Vec3d{0.30, 0.20, 0.13}; // oxidised regolith (Mars)
        planet.bond_albedo = t > 350.0 ? 0.09 : 0.25;
    }
    planet.equilibrium_k = equilibriumTemperature(luminosity, planet.orbit_au, planet.bond_albedo);
}

void addMoons(Planet& planet, double star_mass, double star_luminosity, Dice& dice) {
    planet.moon_count = 0;
    double const radius_km = planet.radius_earth * kEarthRadiusKm;
    double const planet_density = planet.mass_earth * kEarthMassKg /
                                  (4.0 / 3.0 * kPi * std::pow(radius_km * 1000.0, 3.0)) / 1000.0; // g/cm^3
    // Prograde moons stay bound inside about 0.4 Hill radii (Domingos, Winter
    // & Yokoyama 2006), and form outside the Roche limit.
    double const hill_km = planet.orbit_au * (1.0 - planet.eccentricity) * kAuKm *
                           std::cbrt(planet.mass_earth * kEarthMassKg / (3.0 * star_mass * kSolarMassKg));
    double const outer_km = 0.4 * hill_km;
    bool const cold = equilibriumTemperature(star_luminosity, planet.orbit_au, 0.3) < 200.0;
    double const moon_density = cold ? 1.9 : 3.3;
    double const roche_km = 2.44 * radius_km * std::cbrt(planet_density / moon_density);
    double const inner_km = std::max(1.5 * roche_km, 2.5 * radius_km);
    if (outer_km < 1.5 * inner_km) {
        planet.moon_note = "too close to its star: its Hill sphere ends inside the Roche limit";
        return;
    }
    bool const giant = planet.kind == PlanetKind::GasGiant || planet.kind == PlanetKind::IceGiant;
    std::uint32_t wanted = 0;
    double first_km = 0.0;
    double spacing = 1.0;
    double total_ratio = 0.0;
    if (giant) {
        // Regular satellites from the circumplanetary disk: together about
        // 10^-4 of the planet's mass (Canup & Ward 2006), in near-resonant
        // chains like the Galilean moons.
        wanted = 2U + static_cast<std::uint32_t>(dice.unit() * 3.0);
        first_km = std::max(inner_km * 1.2, radius_km * dice.between(4.0, 7.0));
        spacing = dice.between(1.5, 1.9);
        total_ratio = 1.0e-4 * dice.logBetween(0.3, 3.0);
    } else {
        // Moons of rocky planets and sub-Neptunes: debris of a giant impact
        // (the Moon is 1.2 % of the Earth), tidally pushed out to tens of
        // planet radii.
        wanted = dice.unit() < 0.35 ? 2U : 1U;
        first_km = radius_km * dice.between(15.0, 60.0);
        spacing = dice.between(1.8, 3.0);
        total_ratio = dice.logBetween(0.002, 0.03);
    }
    first_km = std::clamp(first_km, inner_km, outer_km / 1.2);
    double const planet_mass_kg = planet.mass_earth * kEarthMassKg;
    double remaining = total_ratio;
    for (std::uint32_t index = 0; index < wanted && planet.moon_count < kMaxMoons; ++index) {
        double const orbit_km = first_km * std::pow(spacing, static_cast<double>(index));
        if (orbit_km > outer_km) {
            planet.moon_note = "no room for more inside its Hill sphere";
            break;
        }
        double const share = index + 1U == wanted ? remaining : remaining * dice.between(0.3, 0.7);
        remaining -= share;
        Moon& moon = planet.moons[planet.moon_count++];
        moon.icy = cold;
        moon.mass_earth = planet.mass_earth * share;
        double const mass_kg = moon.mass_earth * kEarthMassKg;
        moon.radius_km = std::cbrt(mass_kg / (4.0 / 3.0 * kPi * moon_density * 1000.0)) / 1000.0;
        moon.orbit_km = orbit_km;
        double const mu = kGravitySi * (planet_mass_kg + mass_kg);
        moon.period_days = 2.0 * kPi * std::sqrt(std::pow(orbit_km * 1000.0, 3.0) / mu) / kDay;
        moon.phase_rad = dice.between(0.0, 2.0 * kPi);
        moon.albedo = cold ? math::Vec3d{0.62, 0.62, 0.64} : math::Vec3d{0.12, 0.115, 0.11};
    }
}

} // namespace

StarSystem generateStarSystem(ParticleStars const& stars, std::uint32_t rank) {
    StarSystem system;
    system.key = {stars.particle, rank};
    system.name = starName(system.key);
    double const initial_mass = initialMassOfRank(stars, rank);
    system.star = evolveStar(initial_mass, stars.age_myr);
    system.metallicity = starMetallicity(stars, rank);
    Dice dice{system.key};
    // The orbits' reference plane, oriented at random.
    {
        double const z = dice.between(-1.0, 1.0);
        double const angle = dice.between(0.0, 2.0 * kPi);
        double const s = std::sqrt(std::max(1.0 - z * z, 0.0));
        math::Vec3d const normal{s * std::cos(angle), s * std::sin(angle), z};
        math::Vec3d const helper = std::abs(normal[2]) < 0.9 ? math::Vec3d{0.0, 0.0, 1.0} : math::Vec3d{1.0, 0.0, 0.0};
        math::Vec3d const x = math::normalized(math::cross(helper, normal));
        system.axes = {x, math::cross(normal, x), normal};
    }

    // The disk the planets formed in: solids following the minimum-mass
    // extrasolar nebula of Chiang & Laughlin (2013), 50 g/cm^2 (a / AU)^-1.6
    // around a solar-mass star, scaled with the star's mass and metal
    // content and a log-normal spread of disk masses (0.35 dex); twice as
    // much beyond the snow line, where water condenses, at 2.7 AU (M / Msun)^2
    // (Ida & Lin 2004).
    double const mass = initial_mass;
    double const metals = std::pow(10.0, system.metallicity);
    system.disk_solids = std::pow(10.0, 0.35 * dice.gaussian()) * metals;
    double const surface = 50.0 * system.disk_solids * mass;
    system.snow_line_au = std::clamp(2.7 * mass * mass, 0.03, 40.0);
    double const disk_life_myr = dice.logBetween(1.0, 10.0);

    // Three or four planets: the innermost at an orbital period of 2 to 80
    // days, the next at period ratios of 1.5 to 4.5 (the spread of the
    // Kepler multi-planet systems and the solar system).
    std::uint32_t const count = dice.unit() < 0.5 ? 3U : 4U;
    std::array<double, kMaxPlanets> orbit{};
    orbit[0] = orbitOfPeriod(dice.logBetween(2.0, 80.0), mass);
    for (std::uint32_t index = 1; index < count; ++index) {
        orbit[index] = orbit[index - 1U] * std::pow(dice.logBetween(1.5, 4.5), 2.0 / 3.0);
    }
    // Solids within each planet's annulus (halfway, geometrically, to its
    // neighbours), of which a fraction ends up in the planet.
    auto solids = [&](double inner, double outer) {
        auto integral = [&](double a, double b) { // of 2 pi a Sigma da, Sigma = surface a^-1.6
            return 2.0 * kPi * surface * (std::pow(b, 0.4) - std::pow(a, 0.4)) / 0.4;
        };
        double const snow = system.snow_line_au;
        double total = 0.0;
        if (inner < snow) {
            total += integral(inner, std::min(outer, snow));
        }
        if (outer > snow) {
            total += 2.0 * integral(std::max(inner, snow), outer);
        }
        return total * kEarthPerAu2Gcm2;
    };
    std::uint32_t planets = 0;
    for (std::uint32_t index = 0; index < count; ++index) {
        double const a = orbit[index];
        double const inner = index == 0 ? a * a / std::sqrt(orbit[1] * a) : std::sqrt(orbit[index - 1U] * a);
        double const outer = index + 1U == count ? a * std::sqrt(a / orbit[index - 1U]) : std::sqrt(a * orbit[index + 1U]);
        Planet& planet = system.planets[planets];
        planet = {};
        planet.orbit_au = a;
        planet.core_earth = std::max(solids(inner, outer) * dice.between(0.3, 0.9), 0.01);
        bool const beyond = a > system.snow_line_au;
        // Core accretion: a core of 10 Earth masses captures the disk's gas
        // in a runaway (Pollack et al. 1996), if it grows before the gas is
        // gone; cores take longer the further out they are.
        double const growth_myr = 1.0 * std::pow(a / 5.0, 2.5) * (10.0 / planet.core_earth) * std::sqrt(mass);
        bool envelope = false;
        if (planet.core_earth >= 10.0 && growth_myr < disk_life_myr) {
            double const gas = std::pow(10.0, std::log10(300.0) + 0.45 * dice.gaussian());
            planet.mass_earth = planet.core_earth + std::clamp(gas, 40.0, 4000.0);
            envelope = true;
        } else if (planet.core_earth >= 2.0 && dice.unit() < (beyond ? 0.95 : 0.5)) {
            // A few per cent of hydrogen survives on cores beyond the snow
            // line, and on half of those inside it (photo-evaporation strips
            // the rest: the radius valley of Fulton et al. 2017).
            planet.mass_earth = planet.core_earth * (1.0 + dice.between(beyond ? 0.08 : 0.01, beyond ? 0.2 : 0.04));
            envelope = true;
        } else {
            planet.mass_earth = planet.core_earth;
        }
        planet.radius_earth = radiusOfMass(planet.mass_earth);
        if (!envelope && planet.mass_earth >= 2.04) {
            // A bare rocky core keeps the rocky mass-radius relation.
            planet.radius_earth = 1.008 * std::pow(planet.mass_earth, 0.279);
        }
        planet.eccentricity = std::min(std::abs(dice.gaussian()) * (envelope && planet.mass_earth > 50.0 ? 0.08 : 0.03), 0.4);
        planet.inclination_rad = std::abs(dice.gaussian()) * 1.5 * kPi / 180.0;
        planet.node_rad = dice.between(0.0, 2.0 * kPi);
        planet.periapsis_rad = dice.between(0.0, 2.0 * kPi);
        planet.phase_rad = dice.between(0.0, 2.0 * kPi);
        classify(planet, system.star.luminosity, beyond, envelope);
        // Hot Jupiters are inflated by the heat of their star.
        if (planet.kind == PlanetKind::GasGiant && planet.equilibrium_k > 1000.0) {
            planet.radius_earth *= 1.0 + 0.5 * std::min((planet.equilibrium_k - 1000.0) / 1000.0, 1.0);
        }
        // Icy rings, where ice survives (Saturn's are at 80 K).
        if (planet.kind == PlanetKind::GasGiant && dice.unit() < 0.35 && planet.equilibrium_k < 200.0) {
            planet.rings = true;
            planet.ring_inner = dice.between(1.2, 1.5);
            planet.ring_outer = dice.between(planet.ring_inner + 0.3, 2.3);
        }
        ++planets;
    }

    // Spacing: neighbours closer than 10 mutual Hill radii are unstable
    // within the system's lifetime (Chambers et al. 1996); the outer one is
    // moved out to that distance.
    // With h = (m1 + m2) / 3M)^(1/3), ten mutual Hill radii apart is
    // a2 - a1 = 5 h (a1 + a2), so a2 = a1 (1 + 5h) / (1 - 5h).
    for (std::uint32_t index = 1; index < planets; ++index) {
        Planet& previous = system.planets[index - 1U];
        Planet& planet = system.planets[index];
        double const h = std::cbrt((previous.mass_earth + planet.mass_earth) * kEarthMassKg /
                                   (3.0 * mass * kSolarMassKg));
        double const closest = previous.orbit_au * (1.0 + 5.0 * h) / std::max(1.0 - 5.0 * h, 0.05);
        planet.orbit_au = std::max(planet.orbit_au, closest);
    }

    // What the star's evolution has done to them: giants swallow planets
    // inside about twice their largest radius (tides drag in the rest), and
    // the mass the star loses widens the orbits that survive (a M constant
    // under slow mass loss).  A supernova blows off most of the star's mass
    // at once and unbinds its planets.
    StarState const& star = system.star;
    double const engulfed_au = 2.0 * star.largest_radius * 0.00465047;
    double const widening = star.initial_mass / std::max(star.mass, 1.0e-3);
    std::uint32_t kept = 0;
    for (std::uint32_t index = 0; index < planets; ++index) {
        Planet planet = system.planets[index];
        bool const lost = star.phase == StarPhase::NeutronStar || star.phase == StarPhase::BlackHole ||
                          (star.phase != StarPhase::MainSequence && planet.orbit_au < engulfed_au);
        if (lost) {
            ++system.lost_planets;
            continue;
        }
        planet.orbit_au *= widening;
        system.planets[kept++] = planet;
    }
    system.planet_count = kept;

    double const day_lock_au = 0.15 * std::cbrt(mass) * std::pow(std::max(star.age_myr, 1.0) / 1000.0, 1.0 / 6.0);
    for (std::uint32_t p = 0; p < system.planet_count; ++p) {
        Planet& planet = system.planets[p];
        // Re-classified at the star's present luminosity and the orbit it
        // has now (a giant star heats its planets; a white dwarf freezes them).
        bool const envelope = planet.mass_earth > planet.core_earth * 1.005;
        classify(planet, star.luminosity, planet.orbit_au > system.snow_line_au * widening, envelope);
        planet.period_days = periodOfOrbit(planet.orbit_au, star.mass + planet.mass_earth * 3.0035e-6);
        planet.tidally_locked = planet.orbit_au < day_lock_au;
        bool const giant = planet.kind == PlanetKind::GasGiant || planet.kind == PlanetKind::IceGiant;
        planet.day_hours = planet.tidally_locked ? planet.period_days * 24.0
                                                 : (giant ? dice.between(9.0, 18.0) : dice.logBetween(10.0, 60.0));
        // Axial tilt: tides erase it on locked planets; otherwise collisions
        // late in their growth leave tens of degrees (the solar system's run
        // from 3 to 98).  From streams of their own, so that nothing else
        // about the system depends on them.
        if (!planet.tidally_locked) {
            auto const unit = [&](std::uint32_t k) {
                return static_cast<double>(starUnit(stars.particle, rank, 900U + 4U * p + k));
            };
            double const spread = (giant ? 25.0 : 20.0) * kPi / 180.0;
            double const tilt = std::sqrt(-2.0 * std::log(unit(0))) * std::cos(2.0 * kPi * unit(1)) * spread;
            planet.obliquity_rad = std::min(std::abs(tilt), 85.0 * kPi / 180.0);
            double const azimuth = 2.0 * kPi * unit(2);
            double const s = std::sin(planet.obliquity_rad);
            math::Vec3d const pole{s * std::cos(azimuth), s * std::sin(azimuth), std::cos(planet.obliquity_rad)};
            math::Vec3d const helper = std::abs(pole[2]) < 0.9 ? math::Vec3d{0.0, 0.0, 1.0} : math::Vec3d{1.0, 0.0, 0.0};
            math::Vec3d const x = math::normalized(math::cross(helper, pole));
            planet.equator = {x, math::cross(pole, x), pole};
        }
        addMoons(planet, star.mass, star.luminosity, dice);
    }
    return system;
}

namespace {

// Orbital angle of a period at a time given as a coarse part and a fine
// part (seconds): fmod is exact, so the angle keeps its precision however
// long the run.
[[nodiscard]] double orbitalAngle(double phase, double period_s, double coarse_s, double fine_s) {
    if (period_s <= 0.0) {
        return phase;
    }
    double const turns = std::fmod(coarse_s, period_s) / period_s + std::fmod(fine_s, period_s) / period_s;
    return phase + 2.0 * kPi * (turns - std::floor(turns));
}

} // namespace

math::Vec3d planetPosition(Planet const& planet, double coarse_s, double fine_s) noexcept {
    double const mean = orbitalAngle(planet.phase_rad, planet.period_days * kDay, coarse_s, fine_s);
    double const e = planet.eccentricity;
    double anomaly = mean;
    for (int iteration = 0; iteration < 6; ++iteration) {
        anomaly -= (anomaly - e * std::sin(anomaly) - mean) / (1.0 - e * std::cos(anomaly));
    }
    double const x = planet.orbit_au * (std::cos(anomaly) - e);
    double const y = planet.orbit_au * std::sqrt(1.0 - e * e) * std::sin(anomaly);
    // Periapsis in the orbital plane, then the inclination about the node
    // line, then the node's longitude.
    double const cw = std::cos(planet.periapsis_rad);
    double const sw = std::sin(planet.periapsis_rad);
    double const px = x * cw - y * sw;
    double const py = x * sw + y * cw;
    double const ci = std::cos(planet.inclination_rad);
    double const si = std::sin(planet.inclination_rad);
    double const qy = py * ci;
    double const qz = py * si;
    double const cn = std::cos(planet.node_rad);
    double const sn = std::sin(planet.node_rad);
    return {px * cn - qy * sn, px * sn + qy * cn, qz};
}

math::Vec3d moonPosition(Moon const& moon, double coarse_s, double fine_s) noexcept {
    double const angle = orbitalAngle(moon.phase_rad, moon.period_days * kDay, coarse_s, fine_s);
    return {moon.orbit_km * std::cos(angle), moon.orbit_km * std::sin(angle), 0.0};
}

std::string starName(StarKey const& key) {
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "HGC %07u-%u", key.particle, key.rank);
    return buffer;
}

char const* starPhaseName(StarPhase phase) noexcept {
    switch (phase) {
    case StarPhase::MainSequence:
        return "main-sequence star";
    case StarPhase::Giant:
        return "giant";
    case StarPhase::WhiteDwarf:
        return "white dwarf";
    case StarPhase::NeutronStar:
        return "neutron star";
    case StarPhase::BlackHole:
        return "black hole";
    }
    return "";
}

char const* planetKindName(PlanetKind kind) noexcept {
    switch (kind) {
    case PlanetKind::Rocky:
        return "rocky, thin atmosphere";
    case PlanetKind::Temperate:
        return "rocky, temperate";
    case PlanetKind::Cloudy:
        return "rocky, under thick clouds";
    case PlanetKind::Lava:
        return "lava world";
    case PlanetKind::Icy:
        return "icy";
    case PlanetKind::SubNeptune:
        return "sub-Neptune";
    case PlanetKind::IceGiant:
        return "ice giant";
    case PlanetKind::GasGiant:
        return "gas giant";
    }
    return "";
}

std::string spectralClass(StarState const& star) {
    switch (star.phase) {
    case StarPhase::WhiteDwarf:
        return "white dwarf";
    case StarPhase::NeutronStar:
        return "neutron star";
    case StarPhase::BlackHole:
        return "black hole";
    case StarPhase::MainSequence:
    case StarPhase::Giant:
        break;
    }
    struct Band {
        char letter;
        double hot;
        double cool;
    };
    constexpr Band kBands[] = {{'O', 50000.0, 30000.0}, {'B', 30000.0, 10000.0}, {'A', 10000.0, 7500.0},
                               {'F', 7500.0, 6000.0},   {'G', 6000.0, 5200.0},   {'K', 5200.0, 3700.0},
                               {'M', 3700.0, 2300.0}};
    double const t = star.temperature_k;
    Band band = kBands[6];
    for (Band const& candidate : kBands) {
        if (t >= candidate.cool) {
            band = candidate;
            break;
        }
    }
    double const sub = std::clamp(std::log(band.hot / std::max(t, 1.0)) / std::log(band.hot / band.cool), 0.0, 0.95);
    char const* luminosity = star.phase == StarPhase::MainSequence ? "V" : (star.initial_mass >= 8.0 ? "I" : "III");
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%c%d %s", band.letter, static_cast<int>(sub * 10.0), luminosity);
    return buffer;
}

} // namespace helios::examples
