// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// helios_solar_system: the Sun and the eight planets on the planet LOD lab's
// cube-sphere patch renderer.  The Sun is a real body of this scene (a
// PlanetBody with `star = true`, drawn by shaders/planet_patch.slang), not a
// light or a background sprite.  Planet positions come from J2000 mean
// orbital elements (solar_system_data.hpp); the camera can orbit the Sun or
// follow any planet.
//
// Routes:
//   - interactive  an SDL window with an orbit camera and an ImGui panel;
//   - --capture    a windowless offscreen render written as a PNG.
#include "diligent_headless.hpp"
#include "diligent_sdl_app.hpp"
#include "rolling_stats.hpp"
#include "solar_system_stage.hpp"

#include "helios/math/vec.hpp"
#include "helios/render/png_writer.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifndef HELIOS_GIT_HASH
#define HELIOS_GIT_HASH "unknown"
#endif
#ifndef HELIOS_BUILD_TYPE
#define HELIOS_BUILD_TYPE "unknown"
#endif

namespace helios::examples {
namespace {

using Clock = std::chrono::steady_clock;
using Json = nlohmann::ordered_json;

constexpr double kAu = kAstronomicalUnitM;
constexpr double kDaysPerSecondDefault = 2.0;
constexpr double kSecondsPerDay = 86400.0;

struct LabState {
    SolarSystemSettings settings{};
    bool playing = false;
    double days_per_second = kDaysPerSecondDefault;
    bool v_sync = true;
};

[[nodiscard]] SolarSystemSettings defaultSettings() {
    SolarSystemSettings settings;
    settings.days_since_j2000 = 0.0; // J2000.0
    settings.focus = 0;              // the Sun
    settings.camera.distance_m = 2.0 * kAu;
    settings.camera.elevation_deg = 50.0;
    settings.camera.azimuth_deg = -25.0;
    settings.camera.vertical_fov_deg = 55.0;
    settings.planet_radius_scale = 500.0;
    settings.sun_radius_scale = 12.0;
    settings.lod_error_px = 3.0f;
    settings.exposure_ev = 0.0f;
    settings.glare = 0.18f;
    return settings;
}

[[nodiscard]] double daysSinceJ2000Now() {
    // J2000.0 is 2000-01-01 12:00 UTC = Unix 946728000 s.
    double const seconds =
        std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    return (seconds - 946'728'000.0) / 86'400.0;
}

[[nodiscard]] int bodyIndex(std::string_view name) noexcept {
    for (int index = 0; index < static_cast<int>(kSolarSystemBodyCount); ++index) {
        std::string_view const candidate{solarSystemBodyName(index)};
        if (candidate.size() == name.size()) {
            bool same = true;
            for (std::size_t i = 0; i < name.size(); ++i) {
                char const a = candidate[i];
                char const b = name[i];
                char const lower_a = (a >= 'A' && a <= 'Z') ? static_cast<char>(a - 'A' + 'a') : a;
                char const lower_b = (b >= 'A' && b <= 'Z') ? static_cast<char>(b - 'A' + 'a') : b;
                if (lower_a != lower_b) {
                    same = false;
                    break;
                }
            }
            if (same) {
                return index;
            }
        }
    }
    return -1;
}

[[nodiscard]] ImU32 orbitColor(int index) noexcept {
    static constexpr std::array<std::uint32_t, 9> colors{
        0xff8fb4ffu, 0xff9a8f86u, 0xff6fb8d9u, 0xff7fd18au, 0xff6a7fe0u,
        0xff5fb7d9u, 0xff8fd0e8u, 0xffd9b56fu, 0xffe08f6fu,
    };
    return colors[static_cast<std::size_t>(std::clamp(index, 0, 8))];
}

void drawOrbitOverlay(ImDrawList* draw, SolarSystemView const& view, SolarSystemSettings const& state,
                      std::uint32_t width, std::uint32_t height) {
    if (draw == nullptr || !state.show_orbits) {
        return;
    }
    double const orbit_limit = state.camera.distance_m * 1.8;
    for (int index = 1; index < static_cast<int>(kSolarSystemBodyCount); ++index) {
        SolarSystemBody const& body =
            kSolarSystemPlanets[static_cast<std::size_t>(index - 1)];
        // An orbit much larger than the view projects to a few nearly
        // straight lines across the frame; leave it out until the camera is
        // far enough away to see it as an orbit.
        if (body.semi_major_axis_m > orbit_limit) {
            continue;
        }
        constexpr int kSamples = 192;
        ImVec2 previous{};
        bool have_previous = false;
        for (int sample = 0; sample <= kSamples; ++sample) {
            double const offset_days =
                body.orbital_period_days * static_cast<double>(sample) / kSamples;
            math::Vec3d const point =
                solarSystemBodyPosition(index, state.days_since_j2000 + offset_days);
            double x = 0.0;
            double y = 0.0;
            bool const visible = projectSolarSystemPoint(view, point, width, height, x, y);
            ImVec2 const current{static_cast<float>(x), static_cast<float>(y)};
            if (visible && have_previous) {
                draw->AddLine(previous, current, orbitColor(index), 1.0f);
            }
            previous = current;
            have_previous = visible;
        }
    }
}

// The headless capture has no ImGui pass, so the orbits and labels are drawn
// into the RGBA image with the PNG writer's bitmap font.  The interactive
// route uses the ImGui overlay below instead.
void blendPixel(render::RgbaImage& image, int x, int y, std::uint8_t r, std::uint8_t g,
                std::uint8_t b, float alpha) {
    if (x < 0 || y < 0 || x >= static_cast<int>(image.width) ||
        y >= static_cast<int>(image.height)) {
        return;
    }
    std::size_t const offset =
        (static_cast<std::size_t>(y) * image.width + static_cast<std::size_t>(x)) * 4U;
    auto& dst = image.pixels;
    dst[offset + 0] = static_cast<std::uint8_t>(dst[offset + 0] * (1.0f - alpha) + r * alpha);
    dst[offset + 1] = static_cast<std::uint8_t>(dst[offset + 1] * (1.0f - alpha) + g * alpha);
    dst[offset + 2] = static_cast<std::uint8_t>(dst[offset + 2] * (1.0f - alpha) + b * alpha);
}

void drawCpuLine(render::RgbaImage& image, int x0, int y0, int x1, int y1, std::uint8_t r,
                 std::uint8_t g, std::uint8_t b, float alpha) {
    int const dx = std::abs(x1 - x0);
    int const dy = -std::abs(y1 - y0);
    int const sx = x0 < x1 ? 1 : -1;
    int const sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    while (true) {
        blendPixel(image, x0, y0, r, g, b, alpha);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int const twice = 2 * error;
        if (twice >= dy) {
            error += dy;
            x0 += sx;
        }
        if (twice <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

void drawCpuOverlay(render::RgbaImage& image, SolarSystemView const& view,
                    SolarSystemSettings const& state) {
    if (!state.show_orbits) {
        return;
    }
    double const orbit_limit = state.camera.distance_m * 1.8;
    for (int index = 1; index < static_cast<int>(kSolarSystemBodyCount); ++index) {
        SolarSystemBody const& body = kSolarSystemPlanets[static_cast<std::size_t>(index - 1)];
        if (body.semi_major_axis_m > orbit_limit) {
            continue;
        }
        // ImU32 is IM_COL32(r, g, b, a) = a<<24 | b<<16 | g<<8 | r.
        std::uint32_t const color = orbitColor(index);
        std::uint8_t const r = static_cast<std::uint8_t>(color & 0xffU);
        std::uint8_t const g = static_cast<std::uint8_t>((color >> 8) & 0xffU);
        std::uint8_t const b = static_cast<std::uint8_t>((color >> 16) & 0xffU);
        constexpr int kSamples = 192;
        bool have_previous = false;
        int previous_x = 0;
        int previous_y = 0;
        for (int sample = 0; sample <= kSamples; ++sample) {
            double const offset_days =
                body.orbital_period_days * static_cast<double>(sample) / kSamples;
            math::Vec3d const point =
                solarSystemBodyPosition(index, state.days_since_j2000 + offset_days);
            double x = 0.0;
            double y = 0.0;
            bool const visible =
                projectSolarSystemPoint(view, point, image.width, image.height, x, y);
            if (visible) {
                int const px = static_cast<int>(std::lround(x));
                int const py = static_cast<int>(std::lround(y));
                if (have_previous) {
                    drawCpuLine(image, previous_x, previous_y, px, py, r, g, b, 0.55f);
                }
                previous_x = px;
                previous_y = py;
            }
            have_previous = visible;
        }
    }
    for (int index = 0; index < static_cast<int>(kSolarSystemBodyCount); ++index) {
        math::Vec3d const point = solarSystemBodyPosition(index, state.days_since_j2000);
        double x = 0.0;
        double y = 0.0;
        if (!projectSolarSystemPoint(view, point, image.width, image.height, x, y)) {
            continue;
        }
        std::string name = solarSystemBodyName(index);
        for (char& c : name) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        int const label_x = static_cast<int>(std::lround(x)) + (index == 0 ? 24 : 6);
        int const label_y = static_cast<int>(std::lround(y)) + (index == 0 ? 10 : -6);
        render::drawOverlayText(image, label_x, label_y, name, 225, 232, 255);
    }
    char title[96];
    std::snprintf(title, sizeof(title), "HELIOS SOLAR SYSTEM  J2000 %+.1f D", state.days_since_j2000);
    render::drawOverlayText(image, 14, 14, title, 220, 230, 255);
    std::snprintf(title, sizeof(title), "PLANET RADII X%.0f  SUN X%.1f",
                  state.planet_radius_scale, state.sun_radius_scale);
    render::drawOverlayText(image, 14, 24, title, 150, 175, 215);
}

void drawLabels(ImDrawList* draw, SolarSystemView const& view, SolarSystemSettings const& state,
                std::uint32_t width, std::uint32_t height) {
    if (draw == nullptr || !state.show_orbits) {
        return;
    }
    for (int index = 0; index < static_cast<int>(kSolarSystemBodyCount); ++index) {
        math::Vec3d const point = solarSystemBodyPosition(index, state.days_since_j2000);
        double x = 0.0;
        double y = 0.0;
        if (!projectSolarSystemPoint(view, point, width, height, x, y)) {
            continue;
        }
        char const* name = solarSystemBodyName(index);
        ImVec2 const text_size = ImGui::CalcTextSize(name);
        ImVec2 const position{static_cast<float>(x) + (index == 0 ? 24.0f : 4.0f),
                              static_cast<float>(y) + (index == 0 ? 10.0f : -text_size.y - 2.0f)};
        draw->AddText(ImVec2{position.x + 1.0f, position.y + 1.0f}, IM_COL32(0, 0, 0, 180), name);
        draw->AddText(position, IM_COL32(235, 240, 255, 230), name);
    }
}

void applyCameraInput(DiligentSdlApp const& app, SolarSystemSettings& settings, double dt,
                      bool& playing) {
    if (app.wantsKeyboardCapture() ||
        (SDL_GetWindowFlags(app.window()) & SDL_WINDOW_INPUT_FOCUS) == 0U) {
        return;
    }
    bool const* keys = SDL_GetKeyboardState(nullptr);
    double const zoom_speed = 2.5;
    if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP]) {
        settings.camera.distance_m *= std::exp(-dt * zoom_speed);
    }
    if (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN]) {
        settings.camera.distance_m *= std::exp(dt * zoom_speed);
    }
    if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT]) {
        settings.camera.azimuth_deg -= dt * 24.0;
    }
    if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) {
        settings.camera.azimuth_deg += dt * 24.0;
    }
    if (keys[SDL_SCANCODE_Q]) {
        settings.camera.elevation_deg += dt * 24.0;
    }
    if (keys[SDL_SCANCODE_E]) {
        settings.camera.elevation_deg -= dt * 24.0;
    }
    settings.camera.elevation_deg = std::clamp(settings.camera.elevation_deg, -89.0, 89.0);
    settings.camera.distance_m = std::clamp(settings.camera.distance_m, 1.0e8, 2.0e13);
    if (keys[SDL_SCANCODE_SPACE]) {
        playing = !playing;
    }
    (void)settings;
}

[[nodiscard]] std::optional<double> parseNumber(std::string_view text) {
    double value = 0.0;
    auto const [end, code] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (code != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

struct CaptureOptions {
    std::string path;
    std::string report_path;
    std::uint32_t width = 1920;
    std::uint32_t height = 1080;
    int smoke_frames = 0;
    bool help = false;
    SolarSystemSettings settings = defaultSettings();
};

[[nodiscard]] bool parseCapture(int argc, char** argv, CaptureOptions& options,
                                std::string& error) {
    bool requested = false;
    for (int index = 1; index < argc; ++index) {
        std::string_view const argument = argv[index];
        auto value = [&](double minimum, double maximum, double& target) {
            if (index + 1 >= argc) {
                error = "missing value for " + std::string{argument};
                return false;
            }
            auto const parsed = parseNumber(argv[++index]);
            if (!parsed.has_value() || *parsed < minimum || *parsed > maximum) {
                error = std::string{argument} + " is out of range";
                return false;
            }
            target = *parsed;
            return true;
        };
        if (argument == "--capture") {
            if (index + 1 >= argc) {
                error = "--capture needs an output path";
                return false;
            }
            options.path = argv[++index];
            requested = true;
        } else if (argument == "--report") {
            if (index + 1 >= argc) {
                error = "--report needs an output path";
                return false;
            }
            options.report_path = argv[++index];
        } else if (argument == "--width") {
            double v = 0.0;
            if (!value(16.0, 8192.0, v)) {
                return false;
            }
            options.width = static_cast<std::uint32_t>(v);
        } else if (argument == "--height") {
            double v = 0.0;
            if (!value(16.0, 8192.0, v)) {
                return false;
            }
            options.height = static_cast<std::uint32_t>(v);
        } else if (argument == "--days") {
            if (!value(-1.0e6, 1.0e6, options.settings.days_since_j2000)) {
                return false;
            }
        } else if (argument == "--now") {
            options.settings.days_since_j2000 = daysSinceJ2000Now();
        } else if (argument == "--distance-au") {
            double au = 0.0;
            if (!value(1.0e-4, 200.0, au)) {
                return false;
            }
            options.settings.camera.distance_m = au * kAu;
        } else if (argument == "--elevation") {
            if (!value(-89.0, 89.0, options.settings.camera.elevation_deg)) {
                return false;
            }
        } else if (argument == "--azimuth") {
            if (!value(-360.0, 360.0, options.settings.camera.azimuth_deg)) {
                return false;
            }
        } else if (argument == "--fov") {
            if (!value(5.0, 120.0, options.settings.camera.vertical_fov_deg)) {
                return false;
            }
        } else if (argument == "--planet-scale") {
            if (!value(0.01, 2000.0, options.settings.planet_radius_scale)) {
                return false;
            }
        } else if (argument == "--sun-scale") {
            if (!value(0.01, 200.0, options.settings.sun_radius_scale)) {
                return false;
            }
        } else if (argument == "--exposure") {
            double v = 0.0;
            if (!value(-20.0, 20.0, v)) {
                return false;
            }
            options.settings.exposure_ev = static_cast<float>(v);
        } else if (argument == "--glare") {
            double v = 0.0;
            if (!value(0.0, 1.0, v)) {
                return false;
            }
            options.settings.glare = static_cast<float>(v);
        } else if (argument == "--supersample") {
            double v = 0.0;
            if (!value(1.0, 4.0, v)) {
                return false;
            }
            if (v != 1.0 && v != 2.0 && v != 4.0) {
                error = "--supersample must be 1, 2 or 4";
                return false;
            }
            options.settings.supersample = static_cast<int>(v);
        } else if (argument == "--focus") {
            if (index + 1 >= argc) {
                error = "--focus needs a body name";
                return false;
            }
            int const focus = bodyIndex(argv[++index]);
            if (focus < 0) {
                error = "--focus must be the Sun or a planet name";
                return false;
            }
            options.settings.focus = focus;
        } else if (argument == "--true-scale") {
            options.settings.planet_radius_scale = 1.0;
            options.settings.sun_radius_scale = 1.0;
        } else if (argument == "--smoke-frames") {
            double v = 0.0;
            if (!value(1.0, 10000.0, v)) {
                return false;
            }
            options.smoke_frames = static_cast<int>(v);
        } else if (argument == "--help") {
            std::printf(
                "helios_solar_system --capture PATH.png [options]\n"
                "  --days N              days since J2000 (default 0)\n"
                "  --now                 use the current date instead of J2000\n"
                "  --focus NAME          Sun, Mercury, Venus, Earth, Mars, Jupiter, Saturn,\n"
                "                        Uranus or Neptune\n"
                "  --distance-au N       camera distance from the focus\n"
                "  --elevation DEG       camera elevation (default 50)\n"
                "  --azimuth DEG         camera azimuth (default -25)\n"
                "  --fov DEG             vertical field of view (default 55)\n"
                "  --planet-scale N      display radius exaggeration (default 500, 1 = true)\n"
                "  --sun-scale N         Sun display radius exaggeration (default 12)\n"
                "  --true-scale          same as --planet-scale 1 --sun-scale 1\n"
                "  --exposure EV         display exposure\n"
                "  --glare N             glare fraction (default 0.18)\n"
                "  --supersample N       render at N x the output size (1, 2 or 4; default 2)\n"
                "  --width N --height N  image size (default 1920x1080)\n"
                "  --report PATH.json    write the capture parameters and metrics\n"
                "  --smoke-frames N      run the interactive route in a hidden window for N frames\n");
            options.help = true;
            return false;
        } else {
            error = "unknown option: " + std::string{argument};
            return false;
        }
    }
    if (!requested) {
        error = "no capture requested";
        return false;
    }
    return true;
}

int runCapture(CaptureOptions const& options) {
    std::string error;
    auto headless = DiligentHeadless::create(true, error);
    if (headless == nullptr) {
        std::fprintf(stderr, "Solar-system capture initialization failed: %s\n", error.c_str());
        return 1;
    }
    Diligent::IRenderDevice* const device = headless->device();
    Diligent::IDeviceContext* const context = headless->context();
    SolarSystemStage stage;
    if (!stage.initialize(device, context, Diligent::TEX_FORMAT_RGBA8_UNORM_SRGB, error)) {
        std::fprintf(stderr, "Solar-system stage initialization failed: %s\n", error.c_str());
        return 1;
    }
    auto color = createColorTarget(device, "Helios solar-system capture", options.width,
                                   options.height, Diligent::TEX_FORMAT_RGBA8_UNORM_SRGB, false);
    if (color == nullptr) {
        std::fprintf(stderr, "Solar-system capture: failed to create the colour target\n");
        return 1;
    }
    SolarSystemStageMetrics metrics;
    if (!stage.render(options.settings, options.width, options.height, true,
                      color->GetDefaultView(Diligent::TEXTURE_VIEW_RENDER_TARGET), metrics, error)) {
        std::fprintf(stderr, "Solar-system render failed: %s\n", error.c_str());
        return 1;
    }
    render::RgbaImage image;
    image.width = options.width;
    image.height = options.height;
    if (!readbackTexture(device, context, color, 4, image.pixels, error)) {
        std::fprintf(stderr, "Solar-system capture: %s\n", error.c_str());
        return 1;
    }
    context->FinishFrame();
    drawCpuOverlay(image, solarSystemView(options.settings, options.width, options.height),
                   options.settings);
    auto written = render::writePngRgba(options.path, image);
    if (written.failed()) {
        std::fprintf(stderr, "Solar-system capture: %s\n", written.error().to_string().c_str());
        return 1;
    }
    if (!options.report_path.empty()) {
        Json report;
        report["schema"] = "helios-solar-system-capture-v1";
        report["commit"] = HELIOS_GIT_HASH;
        report["build_type"] = HELIOS_BUILD_TYPE;
        report["adapter"] = headless->adapterName();
        report["image"] = options.path;
        report["output_width"] = options.width;
        report["output_height"] = options.height;
        report["supersample"] = options.settings.supersample;
        report["render_width"] = metrics.render_width;
        report["render_height"] = metrics.render_height;
        report["j2000_days"] = options.settings.days_since_j2000;
        report["focus"] = solarSystemBodyName(options.settings.focus);
        report["camera"] = {{"distance_m", options.settings.camera.distance_m},
                            {"elevation_deg", options.settings.camera.elevation_deg},
                            {"azimuth_deg", options.settings.camera.azimuth_deg},
                            {"vertical_fov_deg", options.settings.camera.vertical_fov_deg}};
        report["display"] = {{"planet_radius_scale", options.settings.planet_radius_scale},
                             {"sun_radius_scale", options.settings.sun_radius_scale},
                             {"exposure_ev", options.settings.exposure_ev},
                             {"glare", options.settings.glare}};
        report["metrics"] = {{"bodies_drawn", metrics.bodies_drawn},
                             {"leaves", metrics.leaves},
                             {"draw_calls", metrics.draw_calls},
                             {"triangles", metrics.triangles},
                             {"max_screen_space_error_px", metrics.max_error_px},
                             {"selection_ms", metrics.selection_ms}};
        report["validation_error_count"] = headless->errorCount();
        report["bodies"] = Json::array();
        for (int index = 0; index < static_cast<int>(kSolarSystemBodyCount); ++index) {
            math::Vec3d const position = solarSystemBodyPosition(index, options.settings.days_since_j2000);
            double const display_radius =
                solarSystemDisplayRadius(index, options.settings.planet_radius_scale,
                                         options.settings.sun_radius_scale);
            SolarSystemBody const& data =
                index <= 0 ? kSun : kSolarSystemPlanets[static_cast<std::size_t>(index - 1)];
            report["bodies"].push_back(
                {{"name", solarSystemBodyName(index)},
                 {"position_m", {position[0], position[1], position[2]}},
                 {"distance_au", math::length(position) / kAu},
                 {"radius_km", data.radius_m / 1000.0},
                 {"display_radius_km", display_radius / 1000.0},
                 {"orbital_period_days", data.orbital_period_days},
                 {"rotation_period_hours", data.rotation_period_hours},
                 {"mass_kg", data.mass_kg},
                 {"mean_temperature_k", data.mean_temperature_k}});
        }
        std::ofstream out{options.report_path};
        if (!out) {
            std::fprintf(stderr, "Solar-system capture: cannot write report %s\n",
                         options.report_path.c_str());
        } else {
            out << report.dump(2) << '\n';
        }
    }
    std::printf("Solar-system capture: wrote %s (%ux%u, %dx supersample -> %ux%u, scene %ux%u) at "
                "J2000%+.2f d, focus %s; %u bodies, %u leaves, %llu triangles, "
                "%u validation error(s)\n",
                options.path.c_str(), options.width, options.height,
                options.settings.supersample, metrics.render_width, metrics.render_height,
                metrics.scene_texture_width, metrics.scene_texture_height,
                options.settings.days_since_j2000,
                solarSystemBodyName(options.settings.focus), metrics.bodies_drawn, metrics.leaves,
                static_cast<unsigned long long>(metrics.triangles), headless->errorCount());
    return headless->errorCount() == 0 ? 0 : 2;
}

void drawPanel(LabState& state, SolarSystemStageMetrics const& metrics, float fps) {
    ImGui::Begin("Helios Solar System");
    ImGui::TextUnformatted("The Sun and the eight planets, one cube-sphere renderer.");
    ImGui::Separator();

    int focus = state.settings.focus;
    char const* names[kSolarSystemBodyCount];
    for (int index = 0; index < static_cast<int>(kSolarSystemBodyCount); ++index) {
        names[index] = solarSystemBodyName(index);
    }
    if (ImGui::Combo("Focus", &focus, names, static_cast<int>(kSolarSystemBodyCount))) {
        state.settings.focus = focus;
    }
    ImGui::SameLine();
    if (ImGui::Button("Sun view")) {
        state.settings.focus = 0;
        state.settings.camera.distance_m = 2.0 * kAu;
    }
    ImGui::SameLine();
    if (ImGui::Button("Whole system")) {
        state.settings.focus = 0;
        state.settings.camera.distance_m = 34.0 * kAu;
    }

    ImGui::Checkbox("Play", &state.playing);
    ImGui::SameLine();
    ImGui::Checkbox("Orbit lines", &state.settings.show_orbits);
    ImGui::SameLine();
    if (ImGui::Button("J2000")) {
        state.settings.days_since_j2000 = 0.0;
    }
    ImGui::SameLine();
    if (ImGui::Button("Now")) {
        state.settings.days_since_j2000 = daysSinceJ2000Now();
    }
    double days_min = -20000.0;
    double days_max = 20000.0;
    ImGui::SliderScalar("Days since J2000", ImGuiDataType_Double,
                        &state.settings.days_since_j2000, &days_min, &days_max, "%+.1f");
    double speed_min = 0.01;
    double speed_max = 3650.0;
    ImGui::SliderScalar("Days / second", ImGuiDataType_Double, &state.days_per_second,
                        &speed_min, &speed_max, "%.2f", ImGuiSliderFlags_Logarithmic);

    double distance_au = state.settings.camera.distance_m / kAu;
    double distance_min = 0.001;
    double distance_max = 80.0;
    if (ImGui::SliderScalar("Camera distance (AU)", ImGuiDataType_Double, &distance_au,
                            &distance_min, &distance_max, "%.4f",
                            ImGuiSliderFlags_Logarithmic)) {
        state.settings.camera.distance_m = distance_au * kAu;
    }
    double elevation_min = -89.0;
    double elevation_max = 89.0;
    ImGui::SliderScalar("Elevation (deg)", ImGuiDataType_Double,
                        &state.settings.camera.elevation_deg, &elevation_min, &elevation_max,
                        "%.1f");
    double azimuth_min = -360.0;
    double azimuth_max = 360.0;
    ImGui::SliderScalar("Azimuth (deg)", ImGuiDataType_Double,
                        &state.settings.camera.azimuth_deg, &azimuth_min, &azimuth_max, "%.1f");
    double fov_min = 5.0;
    double fov_max = 120.0;
    ImGui::SliderScalar("Field of view (deg)", ImGuiDataType_Double,
                        &state.settings.camera.vertical_fov_deg, &fov_min, &fov_max, "%.0f");

    ImGui::SeparatorText("Display");
    double planet_scale_min = 1.0;
    double planet_scale_max = 500.0;
    ImGui::SliderScalar("Planet radius x", ImGuiDataType_Double,
                        &state.settings.planet_radius_scale, &planet_scale_min, &planet_scale_max,
                        "%.0f", ImGuiSliderFlags_Logarithmic);
    double sun_scale_min = 1.0;
    double sun_scale_max = 10.0;
    ImGui::SliderScalar("Sun radius x", ImGuiDataType_Double, &state.settings.sun_radius_scale,
                        &sun_scale_min, &sun_scale_max, "%.1f");
    if (ImGui::Button("True scale")) {
        state.settings.planet_radius_scale = 1.0;
        state.settings.sun_radius_scale = 1.0;
    }
    ImGui::SameLine();
    if (ImGui::Button("Schematic")) {
        state.settings.planet_radius_scale = 500.0;
        state.settings.sun_radius_scale = 12.0;
    }
    ImGui::SliderFloat("Exposure (EV)", &state.settings.exposure_ev, -8.0f, 8.0f, "%+.1f");
    ImGui::SliderFloat("Glare", &state.settings.glare, 0.0f, 0.5f, "%.2f");
    int supersample_index = state.settings.supersample >= 4 ? 2 : (state.settings.supersample >= 2 ? 1 : 0);
    char const* supersamples[] = {"1x", "2x", "4x"};
    if (ImGui::Combo("Supersample", &supersample_index, supersamples, 3)) {
        state.settings.supersample = supersample_index == 2 ? 4 : (supersample_index == 1 ? 2 : 1);
    }
    ImGui::SliderFloat("LOD error (px)", &state.settings.lod_error_px, 0.5f, 8.0f, "%.1f");
    if (metrics.render_width > 0) {
        ImGui::Text("Rendering %ux%u, resolving to %ux%u", metrics.render_width,
                    metrics.render_height, metrics.render_width /
                        static_cast<std::uint32_t>(std::max(state.settings.supersample, 1)),
                    metrics.render_height /
                        static_cast<std::uint32_t>(std::max(state.settings.supersample, 1)));
    }

    ImGui::SeparatorText("Frame");
    ImGui::Text("%.1f fps; %u bodies, %u leaves, %u draw calls", fps, metrics.bodies_drawn,
                metrics.leaves, metrics.draw_calls);
    ImGui::Text("%llu triangles; selection %.2f ms; largest SSE %.2f px",
                static_cast<unsigned long long>(metrics.triangles), metrics.selection_ms,
                metrics.max_error_px);
    int const focus_index = std::clamp(state.settings.focus, 0,
                                       static_cast<int>(kSolarSystemBodyCount) - 1);
    if (focus_index > 0) {
        SolarSystemBody const& body =
            kSolarSystemPlanets[static_cast<std::size_t>(focus_index - 1)];
        math::Vec3d const position =
            solarSystemBodyPosition(focus_index, state.settings.days_since_j2000);
        ImGui::SeparatorText(solarSystemBodyName(focus_index));
        ImGui::Text("Radius %.0f km; period %.2f d; rotation %.2f h",
                    body.radius_m / 1000.0, body.orbital_period_days,
                    body.rotation_period_hours);
        ImGui::Text("Distance from the Sun %.3f AU", math::length(position) / kAu);
    } else {
        ImGui::SeparatorText("Sun");
        ImGui::Text("Radius %.0f km; rotation %.1f d; effective temperature %.0f K",
                    kSun.radius_m / 1000.0, kSun.rotation_period_hours / 24.0,
                    kSun.mean_temperature_k);
    }
    ImGui::TextWrapped("Planet radii are exaggerated by default so the worlds are visible at "
                       "system scale; the orbits and positions are the J2000 mean elements. "
                       "The Sun is a real body with an emissive photosphere, not a light.");
    ImGui::End();
}

int runInteractive(int smoke_frames) {
    DiligentSdlCreateInfo create_info;
    create_info.title = "Helios - Solar System";
    create_info.width = 1600;
    create_info.height = 900;
    create_info.enable_validation = true;
    create_info.hidden = smoke_frames > 0;
    std::string error;
    auto app = DiligentSdlApp::create(create_info, error);
    if (app == nullptr) {
        std::fprintf(stderr, "Solar-system initialization failed: %s\n", error.c_str());
        return 1;
    }
    SolarSystemStage stage;
    if (!stage.initialize(app->device(), app->context(),
                          app->swapChain()->GetDesc().ColorBufferFormat, error)) {
        std::fprintf(stderr, "Solar-system stage initialization failed: %s\n", error.c_str());
        return 1;
    }

    LabState state;
    state.settings = defaultSettings();
    RollingStats frame_ms{240};
    SolarSystemStageMetrics metrics;
    std::string status;
    bool running = true;
    int frames = 0;
    Clock::time_point previous = Clock::now();
    while (running) {
        Clock::time_point const begin = Clock::now();
        double const dt = std::clamp(
            std::chrono::duration<double>{begin - previous}.count(), 0.0, 0.1);
        previous = begin;

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            app->forwardEvent(event);
            if (event.type == SDL_EVENT_QUIT ||
                event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                       !app->wantsKeyboardCapture()) {
                switch (event.key.scancode) {
                case SDL_SCANCODE_ESCAPE:
                    running = false;
                    break;
                case SDL_SCANCODE_R:
                    state.settings = defaultSettings();
                    break;
                case SDL_SCANCODE_LEFTBRACKET:
                    state.settings.focus =
                        (state.settings.focus + static_cast<int>(kSolarSystemBodyCount) - 1) %
                        static_cast<int>(kSolarSystemBodyCount);
                    break;
                case SDL_SCANCODE_RIGHTBRACKET:
                    state.settings.focus =
                        (state.settings.focus + 1) % static_cast<int>(kSolarSystemBodyCount);
                    break;
                default:
                    break;
                }
            } else if (event.type == SDL_EVENT_MOUSE_MOTION &&
                       (event.motion.state & SDL_BUTTON_LMASK) != 0U &&
                       !app->wantsMouseCapture()) {
                state.settings.camera.azimuth_deg += event.motion.xrel * 0.25;
                state.settings.camera.elevation_deg =
                    std::clamp(state.settings.camera.elevation_deg - event.motion.yrel * 0.25,
                               -89.0, 89.0);
            } else if (event.type == SDL_EVENT_MOUSE_WHEEL && !app->wantsMouseCapture()) {
                state.settings.camera.distance_m *=
                    std::exp(-static_cast<double>(event.wheel.y) * 0.15);
                state.settings.camera.distance_m =
                    std::clamp(state.settings.camera.distance_m, 1.0e8, 2.0e13);
            }
        }
        if (!running) {
            break;
        }
        if (!app->synchronizeDrawableSize()) {
            SDL_Delay(16);
            continue;
        }

        app->beginImGuiFrame();
        if (state.playing) {
            state.settings.days_since_j2000 += dt * state.days_per_second;
        }
        applyCameraInput(*app, state.settings, dt, state.playing);

        std::uint32_t const width = app->width();
        std::uint32_t const height = app->height();
        if (!stage.render(state.settings, width, height, false,
                          app->swapChain()->GetCurrentBackBufferRTV(), metrics, error)) {
            std::fprintf(stderr, "Solar-system render failed: %s\n", error.c_str());
            return 1;
        }

        SolarSystemView const view = solarSystemView(state.settings, width, height);
        ImDrawList* const draw = ImGui::GetForegroundDrawList();
        drawOrbitOverlay(draw, view, state.settings, width, height);
        drawLabels(draw, view, state.settings, width, height);

        frame_ms.push(std::chrono::duration<double, std::milli>(Clock::now() - begin).count());
        drawPanel(state, metrics, static_cast<float>(1000.0 / std::max(frame_ms.average(), 0.01)));
        app->renderImGui();
        app->present(state.v_sync);
        ++frames;
        if (smoke_frames > 0 && frames >= smoke_frames) {
            running = false;
        }
    }
    (void)status;
    if (smoke_frames > 0) {
        std::printf("Solar-system smoke: %d frame(s), %u validation error(s)\n", frames,
                    app->errorCount());
    }
    return app->errorCount() == 0 ? 0 : 2;
}

} // namespace
} // namespace helios::examples

int main(int argc, char** argv) {
    using namespace helios;
    using namespace helios::examples;

    std::string argument_error;
    CaptureOptions capture;
    bool const parse_ok = parseCapture(argc, argv, capture, argument_error);
    if (!argument_error.empty() && argument_error != "no capture requested") {
        std::fprintf(stderr, "Solar-system lab: %s\n", argument_error.c_str());
        return 2;
    }
    if (capture.help) {
        return 0;
    }
    if (parse_ok) {
        return runCapture(capture);
    }
    return runInteractive(capture.smoke_frames);
}
