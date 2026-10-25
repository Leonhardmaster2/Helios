// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "galaxy_explorer.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>

namespace helios::examples {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kAuPerKpc = 2.06264806e8;
constexpr double kKmPerKpc = 3.0856776e16;
constexpr double kKmPerAu = 1.495978707e8;
constexpr double kMyrPerKpcPerKmS = 977.792; // time to cross 1 kpc at 1 km/s
constexpr double kSolarMassesPerEarth = 3.0035e-6;
// The camera's distance at the galaxy view, where the speed setting applies.
constexpr double kGalaxyViewKpc = 46.0;
// Speed of the stars around a point relative to each other.
constexpr double kRelativeSpeedKmS = 30.0;

[[nodiscard]] double smoothstep(double edge0, double edge1, double x) {
    double const t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

[[nodiscard]] double blendLog(double a, double b, double s) {
    return std::exp(std::log(a) * (1.0 - s) + std::log(b) * s);
}

// The time over which what is in view changes (Myr): the stars around a
// point cross the view at tens of km/s; near a star the planets go round in
// a Kepler period at the camera's distance, near a planet its moons.
[[nodiscard]] double viewTimescale(double distance_kpc, GalaxyFocus const& focus, GalaxyScene const& scene) {
    double timescale = distance_kpc * kMyrPerKpcPerKmS / kRelativeSpeedKmS;
    StarSystem const* system = scene.system();
    if (!focus.hasStar() || system == nullptr || system->key != StarKey{focus.particle, focus.rank}) {
        return timescale;
    }
    double const au = distance_kpc * kAuPerKpc;
    double const star_mass = std::max(system->star.mass, 0.05);
    double const kepler = 2.0 * kPi * std::sqrt(au * au * au / star_mass) * 1.0e-6;
    timescale = blendLog(kepler, timescale, smoothstep(std::log(100.0), std::log(1.0e4), std::log(au)));
    if (focus.kind != GalaxyFocus::Kind::Planet && focus.kind != GalaxyFocus::Kind::Moon) {
        return timescale;
    }
    if (focus.planet >= system->planet_count) {
        return timescale;
    }
    Planet const& planet = system->planets[focus.planet];
    double const mass = planet.mass_earth * kSolarMassesPerEarth;
    double const hill = planet.orbit_au * std::cbrt(mass / (3.0 * star_mass));
    double const around = 2.0 * kPi * std::sqrt(au * au * au / mass) * 1.0e-6;
    return blendLog(around, timescale, smoothstep(std::log(0.3 * hill), std::log(3.0 * hill), std::log(au)));
}

[[nodiscard]] char const* romanNumeral(std::uint32_t index) {
    constexpr char const* kNumerals[] = {"I", "II", "III", "IV", "V", "VI"};
    return kNumerals[std::min<std::uint32_t>(index, 5U)];
}

[[nodiscard]] bool sliderDouble(char const* label, double& value, double minimum, double maximum, char const* format,
                                ImGuiSliderFlags flags = ImGuiSliderFlags_None) {
    return ImGui::SliderScalar(label, ImGuiDataType_Double, &value, &minimum, &maximum, format, flags);
}

} // namespace

std::string formatDistance(double kpc) {
    char buffer[64];
    double const pc = kpc * 1000.0;
    double const au = kpc * kAuPerKpc;
    if (kpc >= 1.0) {
        std::snprintf(buffer, sizeof(buffer), "%.2f kpc", kpc);
    } else if (pc >= 1.0) {
        std::snprintf(buffer, sizeof(buffer), "%.3g pc", pc);
    } else if (au >= 0.01) {
        std::snprintf(buffer, sizeof(buffer), "%.3g AU", au);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.4g km", kpc * kKmPerKpc);
    }
    return buffer;
}

std::string formatDuration(double myr) {
    char buffer[64];
    double const years = myr * 1.0e6;
    double const days = years * 365.25;
    if (myr >= 1.0) {
        std::snprintf(buffer, sizeof(buffer), "%.3g Myr", myr);
    } else if (years >= 1000.0) {
        std::snprintf(buffer, sizeof(buffer), "%.3g kyr", years / 1000.0);
    } else if (years >= 1.0) {
        std::snprintf(buffer, sizeof(buffer), "%.3g yr", years);
    } else if (days >= 1.0) {
        std::snprintf(buffer, sizeof(buffer), "%.3g days", days);
    } else if (days * 24.0 >= 1.0) {
        std::snprintf(buffer, sizeof(buffer), "%.3g hours", days * 24.0);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.3g minutes", days * 1440.0);
    }
    return buffer;
}

double viewingDistance(GalaxyScene const& scene, GalaxyFocus const& focus) {
    switch (focus.kind) {
    case GalaxyFocus::Kind::Centre:
        return kGalaxyViewKpc;
    case GalaxyFocus::Kind::Particle:
        return 0.3;
    case GalaxyFocus::Kind::Star:
    case GalaxyFocus::Kind::Planet:
    case GalaxyFocus::Kind::Moon:
        break;
    }
    StarSystem const* system = scene.system();
    double const radius = scene.focusRadius(focus);
    if (system == nullptr || system->key != StarKey{focus.particle, focus.rank}) {
        // Not worked out yet: the scale of a planetary system.
        return focus.kind == GalaxyFocus::Kind::Star ? 5.0 / kAuPerKpc : 1.0e-4 / kAuPerKpc;
    }
    if (focus.kind == GalaxyFocus::Kind::Star) {
        double outer_au = 0.0;
        for (std::uint32_t p = 0; p < system->planet_count; ++p) {
            outer_au = std::max(outer_au, system->planets[p].orbit_au * (1.0 + system->planets[p].eccentricity));
        }
        double const distance = outer_au > 0.0 ? 2.5 * outer_au / kAuPerKpc : 1.0 / kAuPerKpc;
        return std::max(distance, 30.0 * radius);
    }
    // A planet or a moon filling a good part of the view; backing out shows
    // the moons' orbits.
    return 5.0 * radius;
}

void flyTo(GalaxyExplorer& explorer, GalaxySettings& galaxy, GalaxyFocus const& target, double distance_kpc,
           double to_distance_kpc, double seconds) {
    galaxy.previous_focus = galaxy.focus;
    galaxy.focus = target;
    galaxy.transition = 0.0f;
    explorer.flight_elapsed = 0.0;
    explorer.flight_seconds = std::max(seconds, 0.01);
    explorer.from_distance = std::max(distance_kpc, 1.0e-15);
    explorer.to_distance = std::max(to_distance_kpc, 1.0e-15);
}

GalaxyFocus parentFocus(GalaxyFocus const& focus) {
    GalaxyFocus parent = focus;
    switch (focus.kind) {
    case GalaxyFocus::Kind::Moon:
        parent.kind = GalaxyFocus::Kind::Planet;
        break;
    case GalaxyFocus::Kind::Planet:
        parent.kind = GalaxyFocus::Kind::Star;
        break;
    case GalaxyFocus::Kind::Star:
        parent.kind = GalaxyFocus::Kind::Particle;
        break;
    case GalaxyFocus::Kind::Particle:
    case GalaxyFocus::Kind::Centre:
        parent = GalaxyFocus{};
        break;
    }
    return parent;
}

void updateExplorer(GalaxyExplorer& explorer, GalaxySettings& galaxy, GalaxyScene const& scene, double& distance_kpc,
                    double dt_seconds, bool animate) {
    if (explorer.flight_seconds > 0.0) {
        explorer.flight_elapsed += dt_seconds;
        double const t = std::min(explorer.flight_elapsed / explorer.flight_seconds, 1.0);
        galaxy.transition = static_cast<float>(t);
        // Turn towards the target first, then close in (or back out first,
        // then turn).
        bool const closing = explorer.to_distance < explorer.from_distance;
        double const zoom = closing ? smoothstep(0.2, 1.0, t) : smoothstep(0.0, 0.8, t);
        distance_kpc = blendLog(explorer.from_distance, explorer.to_distance, zoom);
        if (t >= 1.0) {
            explorer.flight_seconds = 0.0;
            galaxy.transition = 1.0f;
            galaxy.previous_focus = galaxy.focus;
        }
    }
    double const radius = scene.focusRadius(galaxy.focus);
    if (radius > 0.0) {
        distance_kpc = std::max(distance_kpc, 1.3 * radius);
    }
    distance_kpc = std::clamp(distance_kpc, 1.0e-15, 600.0);

    double const reference = kGalaxyViewKpc * kMyrPerKpcPerKmS / kRelativeSpeedKmS;
    explorer.rate_myr_per_second =
        explorer.speed_at_galaxy_myr_per_second * viewTimescale(distance_kpc, galaxy.focus, scene) / reference;
    galaxy.speed_myr_per_second = animate ? explorer.rate_myr_per_second : 0.0;
}

void drawExplorerPanel(GalaxyExplorer& explorer, GalaxySettings& galaxy, GalaxyScene const& scene,
                       double& distance_kpc) {
    GalaxyStatus const& status = scene.status();
    ImGui::SeparatorText("Explore");
    ImGui::TextWrapped("Click a star particle to go there, then a single star to visit it, then a planet or a "
                       "moon. Backspace goes one level back out.");
    ImGui::Text("Camera %s from the %s", formatDistance(distance_kpc).c_str(),
                galaxy.focus.kind == GalaxyFocus::Kind::Centre ? "galaxy's centre" : "focus");
    if (ImGui::Button("Whole galaxy")) {
        flyTo(explorer, galaxy, GalaxyFocus{}, distance_kpc, kGalaxyViewKpc);
    }
    ImGui::SameLine();
    if (ImGui::Button("Central black hole")) {
        flyTo(explorer, galaxy, GalaxyFocus{}, distance_kpc, 0.05);
    }
    if (galaxy.focus.kind != GalaxyFocus::Kind::Centre) {
        ImGui::SameLine();
        if (ImGui::Button("Back out")) {
            GalaxyFocus const parent = parentFocus(galaxy.focus);
            flyTo(explorer, galaxy, parent, distance_kpc,
                  std::max(viewingDistance(scene, parent), distance_kpc * 4.0));
        }
    }
    // Somewhere to start: the first star of a kind near the Sun's distance
    // from the centre.
    ImGui::TextUnformatted("Visit, near 8 kpc from the centre:");
    struct Search {
        char const* label;
        StarSearch kind;
    };
    constexpr Search kSearches[] = {{"Sun-like star", StarSearch::SunLike},
                                    {"ringed giant", StarSearch::RingedGiant},
                                    {"lava world", StarSearch::LavaWorld},
                                    {"red giant", StarSearch::Giant},
                                    {"white dwarf", StarSearch::WhiteDwarf}};
    for (std::size_t index = 0; index < std::size(kSearches); ++index) {
        if (index > 0) {
            ImGui::SameLine();
        }
        if (ImGui::Button(kSearches[index].label)) {
            if (auto const star = scene.findStar(8.0, kSearches[index].kind)) {
                flyTo(explorer, galaxy, *star, distance_kpc, 5.0 / kAuPerKpc, 4.0);
            }
        }
    }
    ImGui::Text("Time here: %s per second", formatDuration(explorer.rate_myr_per_second).c_str());
    (void)sliderDouble("Speed at the galaxy view (Myr/s)", explorer.speed_at_galaxy_myr_per_second, 0.0, 60.0,
                       "%.1f");
    ImGui::Checkbox("Single stars", &galaxy.resolve_stars);
    ImGui::SameLine();
    ImGui::Checkbox("Orbits", &explorer.show_orbits);
    ImGui::SameLine();
    ImGui::Checkbox("Names", &explorer.show_labels);
    if (galaxy.resolve_stars) {
        int budget = static_cast<int>(galaxy.star_budget / 1000U);
        if (ImGui::SliderInt("Single stars at most (thousands)", &budget, 100, 6000)) {
            galaxy.star_budget = static_cast<std::uint32_t>(budget) * 1000U;
        }
        ImGui::Text("%llu stars drawn one by one, from %u particles within %s",
                    static_cast<unsigned long long>(status.resolved_stars), status.near_particles,
                    formatDistance(status.near_radius_kpc).c_str());
    }

    StarSystem const* system = scene.system();
    if (galaxy.focus.kind == GalaxyFocus::Kind::Particle) {
        ImGui::TextWrapped("Star particle %u: tens of thousands of stars born together; zoom in (or click one "
                           "of its stars) to visit them.",
                           galaxy.focus.particle);
    }
    if (galaxy.focus.kind == GalaxyFocus::Kind::Centre && distance_kpc < 2.0) {
        ImGui::TextWrapped("At the centre: a black hole of %.2g solar masses, in the potential that the stars "
                           "orbit (softened over 50 pc; its own sphere of influence is a few parsecs).",
                           galaxy.recipe.black_hole_mass * 1.0e10);
    }
    if (system == nullptr || !galaxy.focus.hasStar()) {
        return;
    }
    StarState const& star = system->star;
    ImGui::SeparatorText(system->name.c_str());
    ImGui::Text("%s %s: %.3g solar masses (born %.3g), %.3g solar luminosities", spectralClass(star).c_str(),
                starPhaseName(star.phase), star.mass, star.initial_mass, star.luminosity);
    ImGui::Text("Radius %.3g solar radii, %.0f K; age %s, [Fe/H] %+.2f", star.radius, star.temperature_k,
                formatDuration(star.age_myr).c_str(), system->metallicity);
    if (system->lost_planets > 0) {
        ImGui::TextWrapped("%u planet(s) lost: %s", system->lost_planets,
                           star.phase == StarPhase::NeutronStar || star.phase == StarPhase::BlackHole
                               ? "unbound when the supernova blew off most of the star's mass"
                               : "swallowed by the star when it swelled into a giant");
    }
    if (galaxy.focus.kind != GalaxyFocus::Kind::Star && ImGui::Button("The star")) {
        GalaxyFocus target = galaxy.focus;
        target.kind = GalaxyFocus::Kind::Star;
        flyTo(explorer, galaxy, target, distance_kpc, viewingDistance(scene, target));
    }
    for (std::uint32_t p = 0; p < system->planet_count; ++p) {
        Planet const& planet = system->planets[p];
        ImGui::PushID(static_cast<int>(p));
        char label[160];
        std::snprintf(label, sizeof(label), "%c: %s, %.3g AU, %.3g Earth masses, %.3g Earth radii, %.0f K, year %s",
                      static_cast<char>('b' + p), planetKindName(planet.kind), planet.orbit_au, planet.mass_earth,
                      planet.radius_earth, planet.equilibrium_k, formatDuration(planet.period_days / 365.25e6).c_str());
        bool const focused = galaxy.focus.kind >= GalaxyFocus::Kind::Planet && galaxy.focus.planet == p;
        if (ImGui::Selectable(label, focused)) {
            GalaxyFocus target = galaxy.focus;
            target.kind = GalaxyFocus::Kind::Planet;
            target.planet = p;
            target.moon = 0;
            flyTo(explorer, galaxy, target, distance_kpc, viewingDistance(scene, target));
        }
        if (focused) {
            ImGui::Indent();
            ImGui::Text("Day %s%s%s; %u moon(s)%s%s", formatDuration(planet.day_hours / (24.0 * 365.25e6)).c_str(),
                        planet.tidally_locked ? " (tidally locked)" : "", planet.rings ? ", rings" : "",
                        planet.moon_count, planet.moon_count == 0 ? ": " : "",
                        planet.moon_count == 0 ? planet.moon_note : "");
            for (std::uint32_t m = 0; m < planet.moon_count; ++m) {
                Moon const& moon = planet.moons[m];
                ImGui::PushID(static_cast<int>(m));
                char moon_label[128];
                std::snprintf(moon_label, sizeof(moon_label), "%s: %s, radius %.0f km, %.0f km out, month %s",
                              romanNumeral(m), moon.icy ? "icy" : "rocky", moon.radius_km, moon.orbit_km,
                              formatDuration(moon.period_days / 365.25e6).c_str());
                bool const moon_focused = galaxy.focus.kind == GalaxyFocus::Kind::Moon && galaxy.focus.moon == m;
                if (ImGui::Selectable(moon_label, moon_focused)) {
                    GalaxyFocus target = galaxy.focus;
                    target.kind = GalaxyFocus::Kind::Moon;
                    target.planet = p;
                    target.moon = m;
                    flyTo(explorer, galaxy, target, distance_kpc, viewingDistance(scene, target));
                }
                ImGui::PopID();
            }
            ImGui::Unindent();
        }
        ImGui::PopID();
    }
    ImGui::TextWrapped("Disk solids %.2f times the minimum-mass nebula; snow line at %.2g AU. Planets grow by "
                       "core accretion; colours are the measured albedos of bodies of their kind and "
                       "temperature. Orbits are Kepler ellipses; time near a planet runs at hours per second.",
                       system->disk_solids, system->snow_line_au);
}

void drawExplorerOverlay(GalaxyExplorer const& explorer, GalaxySettings const& galaxy, GalaxyScene const& scene,
                         ImDrawList* list, float width, float height) {
    if (list == nullptr) {
        return;
    }
    auto toScreen = [&](math::Vec3d const& world, ImVec2& point) {
        double x = 0.0;
        double y = 0.0;
        if (!scene.project(world, x, y)) {
            return false;
        }
        point = ImVec2{static_cast<float>((x * 0.5 + 0.5) * width), static_cast<float>((0.5 - 0.5 * y) * height)};
        return std::abs(x) < 4.0 && std::abs(y) < 4.0;
    };
    ImU32 const orbit_colour = IM_COL32(120, 150, 200, 110);
    ImU32 const label_colour = IM_COL32(220, 225, 235, 210);

    if (galaxy.focus.kind == GalaxyFocus::Kind::Particle && explorer.show_labels) {
        if (auto const position = scene.focusPosition(galaxy.focus)) {
            ImVec2 point;
            if (toScreen(*position, point)) {
                list->AddCircle(point, 10.0f, label_colour, 24, 1.0f);
            }
        }
    }
    StarSystem const* system = scene.system();
    std::vector<GalaxyBody> const& bodies = scene.bodies();
    if (system == nullptr || bodies.empty() || !galaxy.focus.hasStar()) {
        return;
    }
    // Only while the system is close enough to be drawn as one.
    double const star_distance = math::length(bodies.front().world_kpc - scene.cameraWorld());
    if (star_distance > 1.0e-3) {
        return;
    }
    if (explorer.show_orbits) {
        std::vector<ImVec2> points;
        for (std::uint32_t p = 0; p < system->planet_count; ++p) {
            Planet const& planet = system->planets[p];
            constexpr int kSamples = 128;
            points.clear();
            for (int k = 0; k <= kSamples; ++k) {
                auto const world = scene.planetWorld(p, -planet.period_days * 86400.0 * k / kSamples);
                ImVec2 point;
                if (world && toScreen(*world, point)) {
                    points.push_back(point);
                } else if (points.size() > 1) {
                    list->AddPolyline(points.data(), static_cast<int>(points.size()), orbit_colour, 0, 1.0f);
                    points.clear();
                }
            }
            if (points.size() > 1) {
                list->AddPolyline(points.data(), static_cast<int>(points.size()), orbit_colour, 0, 1.0f);
            }
        }
        // The moons' orbits around the planet in focus.
        if (galaxy.focus.kind >= GalaxyFocus::Kind::Planet && galaxy.focus.planet < system->planet_count) {
            Planet const& planet = system->planets[galaxy.focus.planet];
            math::Vec3d centre{};
            for (GalaxyBody const& body : bodies) {
                if (body.focus.kind == GalaxyFocus::Kind::Planet && body.focus.planet == galaxy.focus.planet) {
                    centre = body.world_kpc;
                }
            }
            // In the planet's equator.
            auto toWorld = [&](math::Vec3d const& local) {
                return system->axes[0] * local[0] + system->axes[1] * local[1] + system->axes[2] * local[2];
            };
            math::Vec3d const east = toWorld(planet.equator[0]);
            math::Vec3d const north = toWorld(planet.equator[1]);
            for (std::uint32_t m = 0; m < planet.moon_count; ++m) {
                double const radius = planet.moons[m].orbit_km / kKmPerKpc;
                points.clear();
                for (int k = 0; k <= 96; ++k) {
                    double const angle = 2.0 * kPi * k / 96.0;
                    math::Vec3d const world = centre + east * (radius * std::cos(angle)) +
                                              north * (radius * std::sin(angle));
                    ImVec2 point;
                    if (toScreen(world, point)) {
                        points.push_back(point);
                    }
                }
                if (points.size() > 1) {
                    list->AddPolyline(points.data(), static_cast<int>(points.size()), orbit_colour, 0, 1.0f);
                }
            }
        }
    }
    if (!explorer.show_labels) {
        return;
    }
    for (GalaxyBody const& body : bodies) {
        ImVec2 point;
        if (!toScreen(body.world_kpc, point)) {
            continue;
        }
        char text[96];
        switch (body.focus.kind) {
        case GalaxyFocus::Kind::Star:
            std::snprintf(text, sizeof(text), "%s", system->name.c_str());
            break;
        case GalaxyFocus::Kind::Planet:
            std::snprintf(text, sizeof(text), "%c", static_cast<char>('b' + body.focus.planet));
            break;
        case GalaxyFocus::Kind::Moon:
            // Moons are named only around the planet in focus.
            if (galaxy.focus.kind < GalaxyFocus::Kind::Planet || body.focus.planet != galaxy.focus.planet) {
                continue;
            }
            std::snprintf(text, sizeof(text), "%c %s", static_cast<char>('b' + body.focus.planet),
                          romanNumeral(body.focus.moon));
            break;
        default:
            continue;
        }
        list->AddText(ImVec2{point.x + 6.0f, point.y - 16.0f}, label_colour, text);
    }
}

} // namespace helios::examples
