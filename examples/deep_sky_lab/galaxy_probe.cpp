// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// CPU-only probe of the galaxy simulation: runs it without a GPU or window,
// prints how the disk evolves, and optionally writes surface-density maps.
//
// The numbers answer the questions an image cannot: does the initial disk sit
// in equilibrium, how strong are the spiral (m = 2, 3, 4) and bar modes that
// grow out of it, how fast is gas turning into stars, and does the run give
// the same bytes with a different thread count.

#include "galaxy_simulation.hpp"
#include "stellar_population.hpp"
#include "stellar_systems.hpp"

#include "helios/render/png_writer.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <numbers>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace helios::examples;

struct Options {
    GalaxyRecipe recipe{};
    double time_myr = 300.0;
    double every_myr = 50.0;
    std::uint32_t threads = std::max(1U, std::thread::hardware_concurrency());
    std::string map_prefix{};
    bool population = false;
    bool systems = false;
    bool checksum = false;
};

template <typename T> bool parse(std::string_view text, T& value) {
    auto const [end, code] = std::from_chars(text.data(), text.data() + text.size(), value);
    return code == std::errc{} && end == text.data() + text.size();
}

// FNV-1a over the particle positions: equal for equal runs.
[[nodiscard]] std::uint64_t checksum(GalaxySnapshot const& snapshot) {
    std::uint64_t hash = 0xCBF29CE484222325ULL;
    auto const* bytes = reinterpret_cast<unsigned char const*>(snapshot.particles.data());
    for (std::size_t index = 0;
         index < snapshot.particles.size() * sizeof(helios::shaders::GalaxyParticle); ++index) {
        hash = (hash ^ bytes[index]) * 0x100000001B3ULL;
    }
    return hash;
}

void report(GalaxySnapshot const& snapshot, Options const& options) {
    // Fourier amplitudes of the stellar surface density between 2 and 10 kpc,
    // relative to the axisymmetric part, and the disk's thickness and extent.
    std::complex<double> modes[5]{};
    double count = 0.0;
    double z2 = 0.0;
    double r_sum = 0.0;
    // Thickness of the stellar disk inside 3 kpc, from 3 to 8 kpc, and beyond.
    double zone_z2[3]{};
    double zone_count[3]{};
    // Thickness by population (original disk stars, stars formed from gas,
    // bulge) and finer zones: inside 0.5 kpc, 0.5 to 3, 3 to 8, beyond.
    double pop_z2[3][4]{};
    double pop_count[3][4]{};
    double pop_zmax[3][4]{};
    std::size_t gas = 0;
    std::size_t const first_cloud = static_cast<std::size_t>(snapshot.disk_stars) + snapshot.bulge_stars;
    std::size_t const total = snapshot.particles.size();
    for (std::size_t i = 0; i < total; ++i) {
        GalaxyParticleState const state = unpackParticle(snapshot.particles[i]);
        float const p[4]{state.position_kpc[0], state.position_kpc[1], state.position_kpc[2],
                         state.birth_myr};
        bool const is_bulge = i >= snapshot.disk_stars && i < first_cloud;
        if (i >= first_cloud && p[3] == kGasBirth) {
            ++gas;
            continue;
        }
        {
            double const r = std::hypot(p[0], p[1]);
            int const fine = r < 0.5 ? 0 : (r < 3.0 ? 1 : (r < 8.0 ? 2 : 3));
            int const population = i < snapshot.disk_stars ? 0 : (is_bulge ? 2 : 1);
            pop_z2[population][fine] += static_cast<double>(p[2]) * p[2];
            pop_count[population][fine] += 1.0;
            pop_zmax[population][fine] = std::max(pop_zmax[population][fine], std::abs(static_cast<double>(p[2])));
        }
        if (is_bulge) {
            continue;
        }
        double const radius = std::hypot(p[0], p[1]);
        if (i < snapshot.disk_stars) {
            z2 += static_cast<double>(p[2]) * p[2];
            r_sum += radius;
            int const zone = radius < 3.0 ? 0 : (radius < 8.0 ? 1 : 2);
            zone_z2[zone] += static_cast<double>(p[2]) * p[2];
            zone_count[zone] += 1.0;
        }
        if (radius < 2.0 || radius > 10.0) {
            continue;
        }
        double const angle = std::atan2(p[1], p[0]);
        for (int m = 1; m <= 4; ++m) {
            modes[m] += std::polar(1.0, m * angle);
        }
        count += 1.0;
    }
    std::printf("t = %7.1f Myr  step %6llu  %6.2f ms/step (deposit %.1f, field %.1f, kick %.1f) | A1 %.3f A2 %.3f A3 %.3f A4 %.3f | "
                "z_rms %.3f kpc <R> %.2f kpc | gas left %5.1f %%  formed %u  SFR %.2f Msun/yr",
                snapshot.time_myr, static_cast<unsigned long long>(snapshot.step), snapshot.step_ms,
                snapshot.deposit_ms, snapshot.mesh_ms, snapshot.kick_ms,
                std::abs(modes[1]) / count, std::abs(modes[2]) / count, std::abs(modes[3]) / count,
                std::abs(modes[4]) / count, std::sqrt(z2 / snapshot.disk_stars),
                r_sum / snapshot.disk_stars,
                100.0 * static_cast<double>(gas) / std::max<double>(snapshot.clouds, 1.0),
                snapshot.stars_formed, snapshot.star_formation_rate);
    std::printf(" | z_rms by zone %.3f %.3f %.3f", std::sqrt(zone_z2[0] / zone_count[0]),
                std::sqrt(zone_z2[1] / zone_count[1]), std::sqrt(zone_z2[2] / zone_count[2]));
    static char const* const kPopulation[3]{"disk", "formed", "bulge"};
    for (int population = 0; population < 3; ++population) {
        std::printf("\n    %-6s z_rms / max |z| (kpc) r<0.5: %.3f/%.2f  0.5-3: %.3f/%.2f  3-8: %.3f/%.2f  >8: %.3f/%.2f  (n %.0f %.0f %.0f %.0f)",
                    kPopulation[population],
                    std::sqrt(pop_z2[population][0] / std::max(pop_count[population][0], 1.0)), pop_zmax[population][0],
                    std::sqrt(pop_z2[population][1] / std::max(pop_count[population][1], 1.0)), pop_zmax[population][1],
                    std::sqrt(pop_z2[population][2] / std::max(pop_count[population][2], 1.0)), pop_zmax[population][2],
                    std::sqrt(pop_z2[population][3] / std::max(pop_count[population][3], 1.0)), pop_zmax[population][3],
                    pop_count[population][0], pop_count[population][1], pop_count[population][2], pop_count[population][3]);
    }
    if (options.checksum) {
        std::printf("  checksum %016llx", static_cast<unsigned long long>(checksum(snapshot)));
    }
    std::printf("\n");
    std::fflush(stdout);

    if (options.map_prefix.empty()) {
        return;
    }
    // Stars on the left, gas on the right, logarithmic grey scale.
    std::uint32_t const n = snapshot.grid;
    helios::render::RgbaImage image;
    image.width = n * 2U;
    image.height = n;
    image.pixels.assign(static_cast<std::size_t>(image.width) * image.height * 4U, 255);
    auto shade = [](float surface, float low, float high) {
        float const value = std::log(std::max(surface, low) / low) / std::log(high / low);
        return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    for (std::uint32_t y = 0; y < n; ++y) {
        for (std::uint32_t x = 0; x < n; ++x) {
            float const* const texel =
                snapshot.maps.data() + (static_cast<std::size_t>(n - 1U - y) * n + x) * 4U;
            std::uint8_t const stars = shade(texel[2], 1.0f, 3000.0f);
            std::uint8_t const gas_value = shade(texel[0], 0.5f, 200.0f);
            std::uint8_t* const left = image.pixels.data() + (static_cast<std::size_t>(y) * image.width + x) * 4U;
            std::uint8_t* const right = left + static_cast<std::size_t>(n) * 4U;
            left[0] = left[1] = left[2] = stars;
            right[0] = right[1] = right[2] = gas_value;
        }
    }
    char name[64];
    std::snprintf(name, sizeof(name), "_%05d.png", static_cast<int>(std::lround(snapshot.time_myr)));
    auto const written = helios::render::writePngRgba(options.map_prefix + name, image);
    if (written.failed()) {
        std::fprintf(stderr, "GalaxyProbe: %s\n", written.error().to_string().c_str());
    }
}

void printPopulation() {
    std::printf("Simple stellar population (Kroupa IMF), per solar mass formed:\n"
                "   age (Myr)   L_V/M (solar)   M/L_V    linear sRGB (unit luminance)   "
                "ionising photons /s\n");
    for (double age : {1.0, 3.0, 5.0, 10.0, 30.0, 100.0, 300.0, 1000.0, 3000.0, 10000.0, 13000.0}) {
        PopulationLight const light = populationLight(age);
        double const luminosity = populationLuminosity(light);
        std::printf("  %9.0f   %12.4f   %7.3f   %.3f %.3f %.3f              %.3e\n", age, luminosity,
                    1.0 / luminosity, light.rgb[0] / luminosity, light.rgb[1] / luminosity,
                    light.rgb[2] / luminosity, light.ionising_rate);
    }
    helios::math::Vec3d const gas = ionisedGasLight();
    std::printf("Ionised gas per 1e46 photons/s absorbed: linear sRGB %.4f %.4f %.4f (solar "
                "luminosities)\n",
                gas[0], gas[1], gas[2]);
}

// The stars of particles and their planetary systems: checks of the mass
// function, the stellar states and the planets against what is known.
void printSystems() {
    std::printf("Mass function: mean stellar mass %.3f Msun; %u stars in a 37500 Msun particle, %u in a 10000 "
                "Msun one\n",
                imfMeanMass(), starsInParticle(37500.0), starsInParticle(10000.0));
    std::printf("   age (Myr)  turn-off  dead above   remnants  giants  | light: total (Lsun/Msun)  in giants  "
                "brightest 64 stars (sum / table)\n");
    ResolvedLightTable const table;
    for (double age : {3.0, 10.0, 100.0, 1000.0, 5000.0, 10000.0, 13000.0}) {
        ParticleStars const stars = particleStars(1, 37500.0, age, -age, false, 30.0, 30.0);
        double total = 0.0;
        double giants = 0.0;
        double brightest = 0.0;
        for (std::uint32_t slot = 0; slot < stars.count; ++slot) {
            std::uint32_t const rank = rankOfSlot(stars, slot);
            StarState const star = evolveStar(initialMassOfRank(stars, rank), age);
            total += star.luminosity;
            if (star.phase == StarPhase::Giant) {
                giants += star.luminosity;
            }
            if (slot < 64) {
                brightest += star.luminosity;
            }
        }
        std::printf("  %9.0f  %8.3f  %10.3f  %9u  %6u  | %12.4f  %22.1f %%  %8.1f %% / %.1f %%\n", age,
                    turnoffMassAt(age), deadMassAt(age), stars.dead, stars.giants, total / 37500.0,
                    100.0 * giants / std::max(total, 1.0e-30), 100.0 * brightest / std::max(total, 1.0e-30),
                    100.0 * table.fraction(age, 64.0 / stars.count));
    }
    std::printf("The Sun at 4600 Myr: ");
    {
        StarState const sun = evolveStar(1.0, 4600.0);
        std::printf("L %.2f Lsun, R %.2f Rsun, T %.0f K, %s; main sequence %.0f Myr, giant %.0f Myr, largest "
                    "radius after it %.0f Rsun\n",
                    sun.luminosity, sun.radius, sun.temperature_k, spectralClass(sun).c_str(), sun.main_sequence_myr,
                    sun.giant_myr, evolveStar(1.0, 1.0e5).largest_radius);
    }

    // Statistics over many systems of a 5 Gyr old particle's stars.
    std::uint32_t kinds[8]{};
    std::uint32_t planets = 0;
    std::uint32_t moons = 0;
    std::uint32_t with_moons = 0;
    std::uint32_t lost = 0;
    std::uint32_t systems_with_giant = 0;
    constexpr std::uint32_t kSystems = 20000;
    for (std::uint32_t index = 0; index < kSystems; ++index) {
        ParticleStars const stars = particleStars(1000U + index / 50U, 37500.0, 5000.0, -5000.0, false, 30.0, 30.0);
        std::uint32_t const rank = stars.dead + stars.giants + (index * 7919U) % (stars.count - stars.dead - stars.giants);
        StarSystem const system = generateStarSystem(stars, rank);
        bool giant = false;
        lost += system.lost_planets;
        for (std::uint32_t p = 0; p < system.planet_count; ++p) {
            Planet const& planet = system.planets[p];
            ++kinds[static_cast<int>(planet.kind)];
            ++planets;
            moons += planet.moon_count;
            with_moons += planet.moon_count > 0 ? 1U : 0U;
            giant = giant || planet.kind == PlanetKind::GasGiant;
        }
        systems_with_giant += giant ? 1U : 0U;
    }
    std::printf("%u main-sequence systems at 5 Gyr: %.2f planets each, %.1f %% with a gas giant; %.2f moons per "
                "planet, %.1f %% of planets have moons\n",
                kSystems, static_cast<double>(planets) / kSystems, 100.0 * systems_with_giant / kSystems,
                static_cast<double>(moons) / std::max(planets, 1U), 100.0 * with_moons / std::max(planets, 1U));
    std::printf("  kinds:");
    for (int kind = 0; kind < 8; ++kind) {
        std::printf(" %s %.1f %%,", planetKindName(static_cast<PlanetKind>(kind)), 100.0 * kinds[kind] / std::max(planets, 1U));
    }
    std::printf("\n");

    // A few systems in full.
    for (std::uint32_t example = 0; example < 6; ++example) {
        double const age = example < 4 ? 4600.0 : (example == 4 ? 12000.0 : 30.0);
        ParticleStars const stars = particleStars(4242U + example, 37500.0, age, -age, false, 30.0, 30.0);
        // A star near the Sun's mass, or the brightest living one.
        std::uint32_t rank = static_cast<std::uint32_t>(imfTailFraction(example == 3 ? 0.3 : 1.0) * stars.count) + example;
        if (example == 4) {
            rank = stars.dead - 3U; // a remnant
        } else if (example == 5) {
            rank = stars.dead + 2U;
        }
        StarSystem const system = generateStarSystem(stars, rank);
        StarState const& star = system.star;
        std::printf("\n%s: %s (%s), %.3f Msun (born %.3f), L %.3g Lsun, R %.3g Rsun, %.0f K, age %.0f Myr, "
                    "[Fe/H] %+.2f, snow line %.2f AU, disk solids x%.2f, %u planet(s) lost\n",
                    system.name.c_str(), spectralClass(star).c_str(), starPhaseName(star.phase), star.mass,
                    star.initial_mass, star.luminosity, star.radius, star.temperature_k, age, system.metallicity,
                    system.snow_line_au, system.disk_solids, system.lost_planets);
        for (std::uint32_t p = 0; p < system.planet_count; ++p) {
            Planet const& planet = system.planets[p];
            std::printf("  %c: %-26s %8.3f AU  P %9.2f d  e %.3f  %8.2f M_E (core %.2f)  %6.2f R_E  T_eq %5.0f K  "
                        "day %.1f h%s%s, %u moon(s)%s%s\n",
                        static_cast<char>('b' + p), planetKindName(planet.kind), planet.orbit_au, planet.period_days,
                        planet.eccentricity, planet.mass_earth, planet.core_earth, planet.radius_earth,
                        planet.equilibrium_k, planet.day_hours, planet.tidally_locked ? " (locked)" : "",
                        planet.rings ? ", rings" : "", planet.moon_count, planet.moon_count == 0 ? ": " : "",
                        planet.moon_count == 0 ? planet.moon_note : "");
            for (std::uint32_t m = 0; m < planet.moon_count; ++m) {
                Moon const& moon = planet.moons[m];
                std::printf("      moon %u: r %.0f km, orbit %.0f km, P %.2f d, %s\n", m + 1U, moon.radius_km,
                            moon.orbit_km, moon.period_days, moon.icy ? "icy" : "rocky");
            }
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        std::string_view const argument = argv[index];
        auto next = [&]() -> std::string_view { return index + 1 < argc ? argv[++index] : ""; };
        bool ok = true;
        if (argument == "--time") {
            ok = parse(next(), options.time_myr);
        } else if (argument == "--every") {
            ok = parse(next(), options.every_myr);
        } else if (argument == "--grid") {
            ok = parse(next(), options.recipe.grid);
        } else if (argument == "--stars") {
            ok = parse(next(), options.recipe.disk_stars);
        } else if (argument == "--gas") {
            ok = parse(next(), options.recipe.gas_clouds);
        } else if (argument == "--bulge") {
            ok = parse(next(), options.recipe.bulge_stars);
        } else if (argument == "--seed") {
            ok = parse(next(), options.recipe.seed);
        } else if (argument == "--toomre-q") {
            ok = parse(next(), options.recipe.toomre_q);
        } else if (argument == "--time-step") {
            ok = parse(next(), options.recipe.time_step_myr);
        } else if (argument == "--gas-mass") {
            ok = parse(next(), options.recipe.gas_mass);
        } else if (argument == "--feedback") {
            ok = parse(next(), options.recipe.feedback_momentum_km_s);
        } else if (argument == "--sound-speed") {
            ok = parse(next(), options.recipe.gas_sound_speed_km_s);
        } else if (argument == "--threads") {
            ok = parse(next(), options.threads);
        } else if (argument == "--maps") {
            options.map_prefix = std::string{next()};
        } else if (argument == "--population") {
            options.population = true;
        } else if (argument == "--systems") {
            options.systems = true;
        } else if (argument == "--checksum") {
            options.checksum = true;
        } else {
            std::fprintf(stderr,
                         "Usage: helios_galaxy_probe [--time MYR] [--every MYR] [--grid N] "
                         "[--stars N] [--gas N] [--bulge N] [--seed N] [--toomre-q Q] "
                         "[--time-step MYR] [--gas-mass M] [--feedback KM_S] [--sound-speed KM_S] "
                         "[--threads N] [--maps PREFIX] [--population] [--systems] [--checksum]\n");
            return argument == "--help" ? 0 : 2;
        }
        if (!ok) {
            std::fprintf(stderr, "GalaxyProbe: invalid value for %s\n", std::string{argument}.c_str());
            return 2;
        }
    }
    if (options.population) {
        printPopulation();
        return 0;
    }
    if (options.systems) {
        printSystems();
        return 0;
    }
    if (options.recipe.grid < 32 || (options.recipe.grid & (options.recipe.grid - 1U)) != 0) {
        std::fprintf(stderr, "GalaxyProbe: --grid must be a power of two, at least 32\n");
        return 2;
    }

    WorkerPool pool{options.threads};
    GalaxySimulation simulation{options.recipe, pool};
    std::printf("Galaxy: %u disk stars of %.0f Msun, %u gas clouds of %.0f Msun, %u bulge stars; "
                "mesh %u^2, cell %.0f pc, %u thread(s)\n",
                options.recipe.disk_stars, simulation.diskParticleMass(), options.recipe.gas_clouds,
                simulation.cloudParticleMass(), options.recipe.bulge_stars, options.recipe.grid,
                2000.0 * options.recipe.half_extent_kpc / options.recipe.grid, pool.threadCount());
    std::printf("Rotation curve (km/s):");
    for (double radius : {0.5, 1.0, 2.0, 4.0, 6.0, 8.0, 12.0, 16.0}) {
        std::printf("  %.1f kpc: %.0f", radius, simulation.circularSpeed(radius));
    }
    std::printf("\n");

    GalaxySnapshot snapshot;
    simulation.snapshot(snapshot);
    report(snapshot, options);
    double next_report = options.every_myr;
    while (simulation.timeMyr() < options.time_myr - 1.0e-6) {
        simulation.step();
        if (simulation.timeMyr() >= next_report - 1.0e-6 ||
            simulation.timeMyr() >= options.time_myr - 1.0e-6) {
            simulation.snapshot(snapshot);
            report(snapshot, options);
            next_report += options.every_myr;
        }
    }
    return 0;
}
