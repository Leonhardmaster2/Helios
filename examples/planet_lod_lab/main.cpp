// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Interactive Vulkan-only cube-sphere LOD and orbital-to-surface lab.
// It deliberately uses no mouse grab, relative mouse mode, global input hook,
// screen capture, or legacy helios_vk/helios_render object.
//
// `--capture PATH.png` renders one frame of the scripted route offscreen on a
// windowless device instead, so the route can be inspected and regression-
// checked without opening a window.
#include "diligent_headless.hpp"
#include "diligent_sdl_app.hpp"
#include "planet_patch_renderer.hpp"
#include "planet_scene.hpp"
#include "rolling_stats.hpp"

#include "helios/math/vec.hpp"
#include "helios/planet/quadtree_lod.hpp"
#include "helios/render/png_writer.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#ifndef HELIOS_GIT_HASH
#define HELIOS_GIT_HASH "unknown"
#endif
#ifndef HELIOS_DILIGENT_COMMIT
#define HELIOS_DILIGENT_COMMIT "unknown"
#endif
#ifndef HELIOS_DILIGENT_CORE_COMMIT
#define HELIOS_DILIGENT_CORE_COMMIT "unknown"
#endif

namespace helios::examples {
namespace {

using Clock = std::chrono::steady_clock;


using CameraState = PlanetCamera;
using DescentRoute = PlanetDescentRoute;

struct LabState {
    planet::QuadtreeLodConfig lod{};
    PlanetPatchSettings patches{};
    CameraState camera{};
    float render_scale = 1.0f;
    float vertical_fov_deg = 60.0f;
    bool v_sync = true;
};

[[nodiscard]] PlanetPatchView makeView(LabState const& state, std::uint32_t viewport_width,
                                       std::uint32_t viewport_height) {
    return makePlanetView(state.camera, state.patches, static_cast<double>(state.vertical_fov_deg),
                          viewport_width, viewport_height);
}

void applyManualCameraInput(DiligentSdlApp const& app, LabState& state, double dt_seconds) {
    if (app.wantsKeyboardCapture() ||
        (SDL_GetWindowFlags(app.window()) & SDL_WINDOW_INPUT_FOCUS) == 0U) {
        return;
    }
    bool const* keys = SDL_GetKeyboardState(nullptr);
    double const altitude_factor = std::exp(dt_seconds * 1.8);
    if (keys[SDL_SCANCODE_W]) {
        state.camera.altitude_agl_m /= altitude_factor;
    }
    if (keys[SDL_SCANCODE_S]) {
        state.camera.altitude_agl_m *= altitude_factor;
    }
    double const angular_step = dt_seconds * 10.0;
    if (keys[SDL_SCANCODE_A]) {
        state.camera.longitude_deg -= angular_step;
    }
    if (keys[SDL_SCANCODE_D]) {
        state.camera.longitude_deg += angular_step;
    }
    if (keys[SDL_SCANCODE_Q]) {
        state.camera.latitude_deg += angular_step;
    }
    if (keys[SDL_SCANCODE_E]) {
        state.camera.latitude_deg -= angular_step;
    }
    state.camera.altitude_agl_m =
        std::clamp(state.camera.altitude_agl_m, 2.0, 1'000'000'000.0);
    state.camera.latitude_deg = std::clamp(state.camera.latitude_deg, -85.0, 85.0);
    if (state.camera.longitude_deg > 180.0) {
        state.camera.longitude_deg -= 360.0;
    } else if (state.camera.longitude_deg < -180.0) {
        state.camera.longitude_deg += 360.0;
    }
}

[[nodiscard]] bool sliderDouble(char const* label, double& value, double minimum,
                                double maximum, char const* format,
                                ImGuiSliderFlags flags = ImGuiSliderFlags_None) {
    return ImGui::SliderScalar(label, ImGuiDataType_Double, &value, &minimum, &maximum, format,
                               flags);
}

void drawPerformancePlot(char const* label, RollingStats const& statistics,
                         float plot_maximum) {
    std::vector<float> const values = statistics.orderedFloatValues();
    if (!values.empty()) {
        ImGui::PlotLines(label, values.data(), static_cast<int>(values.size()), 0, nullptr, 0.0f,
                         plot_maximum, ImVec2{-1.0f, 64.0f * ImGui::GetStyle().FontScaleDpi});
    }
}

constexpr char const* kSettingsPath = "helios_planet_lod_settings.json";

[[nodiscard]] bool validateLoadedState(LabState const& state, std::string& status_message) {
    auto lod_validity = planet::validate(state.lod);
    if (lod_validity.failed()) {
        status_message = "Settings rejected: " + lod_validity.error().to_string();
        return false;
    }
    bool const camera_valid = std::isfinite(state.camera.altitude_agl_m) &&
                              state.camera.altitude_agl_m >= 2.0 &&
                              state.camera.altitude_agl_m <= 1'000'000'000.0 &&
                              std::isfinite(state.camera.latitude_deg) &&
                              state.camera.latitude_deg >= -85.0 &&
                              state.camera.latitude_deg <= 85.0 &&
                              std::isfinite(state.camera.longitude_deg) &&
                              state.camera.longitude_deg >= -180.0 &&
                              state.camera.longitude_deg <= 180.0 &&
                              std::isfinite(state.camera.look_down_deg) &&
                              state.camera.look_down_deg >= 1.0 &&
                              state.camera.look_down_deg <= 90.0;
    bool const presentation_valid = std::isfinite(state.render_scale) &&
                                    state.render_scale >= 0.25f && state.render_scale <= 1.0f &&
                                    std::isfinite(state.vertical_fov_deg) &&
                                    state.vertical_fov_deg >= 25.0f &&
                                    state.vertical_fov_deg <= 100.0f;
    bool const patch_valid = std::isfinite(state.patches.planet_radius_m) &&
                             state.patches.planet_radius_m > 0.0 &&
                             std::isfinite(state.patches.max_render_distance_m) &&
                             state.patches.max_render_distance_m >= 1'000.0 &&
                             state.patches.max_render_distance_m <= 2'000'000'000.0 &&
                             std::isfinite(state.patches.relief_amplitude_m) &&
                             state.patches.relief_amplitude_m >= 0.0f &&
                             state.patches.relief_amplitude_m <= 3000.0f &&
                             std::isfinite(state.patches.skirt_depth_m) &&
                             state.patches.skirt_depth_m >= 0.0f &&
                             state.patches.skirt_depth_m <= 6000.0f &&
                             std::isfinite(state.patches.max_sse_for_heatmap_px) &&
                             state.patches.max_sse_for_heatmap_px >= 0.5f &&
                             state.patches.max_sse_for_heatmap_px <= 32.0f &&
                             static_cast<unsigned int>(state.patches.debug_color) <=
                                 static_cast<unsigned int>(PatchDebugColor::ScreenSpaceError);
    if (!camera_valid || !presentation_valid || !patch_valid ||
        state.lod.planet_radius_m != state.patches.planet_radius_m) {
        status_message = "Settings rejected: one or more values are outside the UI contract";
        return false;
    }
    return true;
}

[[nodiscard]] bool saveSettings(LabState const& state, std::string& status_message) {
    try {
        nlohmann::json document{
            {"schema", "helios.planet-lod-settings"},
            {"version", 1},
            {"camera",
             {{"altitude_agl_m", state.camera.altitude_agl_m},
              {"latitude_deg", state.camera.latitude_deg},
              {"longitude_deg", state.camera.longitude_deg},
              {"look_down_deg", state.camera.look_down_deg}}},
            {"presentation",
             {{"render_scale", state.render_scale},
              {"vertical_fov_deg", state.vertical_fov_deg},
              {"v_sync", state.v_sync}}},
            {"lod",
             {{"planet_radius_m", state.lod.planet_radius_m},
              {"root_geometric_error_m", state.lod.root_geometric_error_m},
              {"error_scale_per_level", state.lod.error_scale_per_level},
              {"split_threshold_px", state.lod.split_threshold_px},
              {"merge_threshold_px", state.lod.merge_threshold_px},
              {"min_level", state.lod.min_level},
              {"max_level", state.lod.max_level},
              {"leaf_budget", state.lod.leaf_budget},
              {"minimum_distance_m", state.lod.minimum_distance_m}}},
            {"patches",
             {{"horizon_culling", state.patches.horizon_culling},
              {"wire_overlay", state.patches.wire_overlay},
              {"max_render_distance_m", state.patches.max_render_distance_m},
              {"relief_amplitude_m", state.patches.relief_amplitude_m},
              {"skirt_depth_m", state.patches.skirt_depth_m},
              {"debug_color", static_cast<unsigned int>(state.patches.debug_color)},
              {"max_sse_for_heatmap_px", state.patches.max_sse_for_heatmap_px}}},
        };
        std::ofstream output{kSettingsPath, std::ios::binary | std::ios::trunc};
        if (!output) {
            status_message = std::string{"Could not open settings file for writing: "} +
                             kSettingsPath;
            return false;
        }
        output << document.dump(2) << '\n';
        output.flush();
        if (!output.good()) {
            status_message = std::string{"Failed while writing settings file: "} + kSettingsPath;
            return false;
        }
        status_message = std::string{"Saved settings to "} + kSettingsPath;
        return true;
    } catch (std::exception const& exception) {
        status_message = std::string{"Settings save failed: "} + exception.what();
        return false;
    }
}

[[nodiscard]] bool loadSettings(LabState& state, planet::QuadtreeLodSelector& selector,
                                std::string& status_message) {
    try {
        std::ifstream input{kSettingsPath, std::ios::binary};
        if (!input) {
            status_message = std::string{"Could not open settings file: "} + kSettingsPath;
            return false;
        }
        nlohmann::json document;
        input >> document;
        if (document.at("schema").get<std::string>() != "helios.planet-lod-settings" ||
            document.at("version").get<int>() != 1) {
            status_message = "Settings rejected: unsupported schema or version";
            return false;
        }

        LabState candidate = state;
        auto const& camera = document.at("camera");
        candidate.camera.altitude_agl_m = camera.at("altitude_agl_m").get<double>();
        candidate.camera.latitude_deg = camera.at("latitude_deg").get<double>();
        candidate.camera.longitude_deg = camera.at("longitude_deg").get<double>();
        candidate.camera.look_down_deg = camera.at("look_down_deg").get<double>();

        auto const& presentation = document.at("presentation");
        candidate.render_scale = presentation.at("render_scale").get<float>();
        candidate.vertical_fov_deg = presentation.at("vertical_fov_deg").get<float>();
        candidate.v_sync = presentation.at("v_sync").get<bool>();

        auto const& lod = document.at("lod");
        candidate.lod.planet_radius_m = lod.at("planet_radius_m").get<double>();
        candidate.lod.root_geometric_error_m =
            lod.at("root_geometric_error_m").get<double>();
        candidate.lod.error_scale_per_level = lod.at("error_scale_per_level").get<double>();
        candidate.lod.split_threshold_px = lod.at("split_threshold_px").get<double>();
        candidate.lod.merge_threshold_px = lod.at("merge_threshold_px").get<double>();
        candidate.lod.min_level = lod.at("min_level").get<std::uint8_t>();
        candidate.lod.max_level = lod.at("max_level").get<std::uint8_t>();
        candidate.lod.leaf_budget = lod.at("leaf_budget").get<std::uint32_t>();
        candidate.lod.minimum_distance_m = lod.at("minimum_distance_m").get<double>();
        candidate.patches.planet_radius_m = candidate.lod.planet_radius_m;

        auto const& patches = document.at("patches");
        candidate.patches.horizon_culling = patches.at("horizon_culling").get<bool>();
        candidate.patches.wire_overlay = patches.at("wire_overlay").get<bool>();
        candidate.patches.max_render_distance_m =
            patches.at("max_render_distance_m").get<double>();
        candidate.patches.relief_amplitude_m = patches.at("relief_amplitude_m").get<float>();
        candidate.patches.skirt_depth_m = patches.at("skirt_depth_m").get<float>();
        unsigned int const debug_color = patches.at("debug_color").get<unsigned int>();
        if (debug_color > static_cast<unsigned int>(PatchDebugColor::ScreenSpaceError)) {
            status_message = "Settings rejected: patch debug color is out of range";
            return false;
        }
        candidate.patches.debug_color = static_cast<PatchDebugColor>(debug_color);
        candidate.patches.max_sse_for_heatmap_px =
            patches.at("max_sse_for_heatmap_px").get<float>();

        if (!validateLoadedState(candidate, status_message)) {
            return false;
        }
        auto updated = selector.setConfig(candidate.lod, false);
        if (updated.failed()) {
            status_message = "Settings rejected: " + updated.error().to_string();
            return false;
        }
        state = candidate;
        status_message = std::string{"Loaded settings from "} + kSettingsPath;
        return true;
    } catch (std::exception const& exception) {
        status_message = std::string{"Settings load failed: "} + exception.what();
        return false;
    }
}

void drawControlPanel(DiligentSdlApp& app, LabState& state, DescentRoute& route,
                      planet::QuadtreeLodSelector& selector,
                      planet::LodSelection const& selection,
                      PlanetPatchMetrics const& render_metrics,
                      RollingStats const& frame_ms, RollingStats const& lod_ms,
                      RollingStats const& submit_ms, RollingStats const& gpu_ms,
                      std::string& status_message) {
    float const ui_scale = app.uiScale();
    ImGui::SetNextWindowSize(ImVec2{470.0f * ui_scale, 760.0f * ui_scale},
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Helios v0.0.1 - Planet LOD / Descent")) {
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted("Diligent Engine / Vulkan-only path");
    ImGui::TextWrapped("Adapter: %s", app.adapterName().c_str());
    ImGui::TextWrapped("Helios commit: %s", HELIOS_GIT_HASH);
    ImGui::TextWrapped("Diligent Engine pin: %s", HELIOS_DILIGENT_COMMIT);
    ImGui::TextWrapped("Diligent Core pin: %s", HELIOS_DILIGENT_CORE_COMMIT);
    ImGui::Text("Reversed-Z: D32, clear 0, GREATER_EQUAL");
    ImGui::Text("Validation diagnostics: %u warning(s), %u error(s)", app.warningCount(),
                app.errorCount());
    if (ImGui::Button("Save settings")) {
        (void)saveSettings(state, status_message);
    }
    ImGui::SameLine();
    if (ImGui::Button("Load settings")) {
        if (loadSettings(state, selector, status_message)) {
            route.playing = false;
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", kSettingsPath);
    if (!status_message.empty()) {
        ImGui::TextColored(ImVec4{1.0f, 0.55f, 0.2f, 1.0f}, "%s", status_message.c_str());
    }

    if (ImGui::CollapsingHeader("Scripted orbital-to-surface route",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Play descent", &route.playing);
        ImGui::SameLine();
        if (ImGui::Button("Restart")) {
            route.restart(state.camera);
        }
        (void)sliderDouble("Playback speed", route.playback_speed, 0.1, 8.0, "%.2fx",
                           ImGuiSliderFlags_Logarithmic);
        float const progress = static_cast<float>(route.progress());
        ImGui::ProgressBar(progress, ImVec2{-1.0f, 0.0f}, route.stageLabel());
        ImGui::Text("Altitude above procedural surface: %.3f m", state.camera.altitude_agl_m);
        ImGui::Text("Latitude/longitude: %.3f / %.3f deg", state.camera.latitude_deg,
                    state.camera.longitude_deg);
        ImGui::TextWrapped("Focused-window controls while paused: W/S altitude, A/D longitude, "
                           "Q/E latitude, Space play/pause, R restart, Esc quit. No mouse grab or "
                           "relative input is used.");
    }

    if (ImGui::CollapsingHeader("Camera and presentation", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (sliderDouble("Altitude AGL (m)", state.camera.altitude_agl_m, 2.0,
                         1'000'000'000.0,
                         "%.3f", ImGuiSliderFlags_Logarithmic)) {
            route.playing = false;
        }
        bool camera_changed = false;
        camera_changed |= sliderDouble("Latitude (deg)", state.camera.latitude_deg, -85.0,
                                       85.0, "%.3f");
        camera_changed |= sliderDouble("Longitude (deg)", state.camera.longitude_deg, -180.0,
                                       180.0, "%.3f");
        camera_changed |= sliderDouble("Look down (deg)", state.camera.look_down_deg, 1.0, 90.0,
                                       "%.2f");
        if (camera_changed) {
            route.playing = false;
        }
        ImGui::SliderFloat("Vertical FOV", &state.vertical_fov_deg, 25.0f, 100.0f, "%.1f deg");
        ImGui::SliderFloat("Render scale", &state.render_scale, 0.25f, 1.0f, "%.2fx");
        ImGui::Checkbox("V-sync", &state.v_sync);
    }

    if (ImGui::CollapsingHeader("Quadtree LOD policy", ImGuiTreeNodeFlags_DefaultOpen)) {
        planet::QuadtreeLodConfig candidate = state.lod;
        bool changed = false;
        changed |= sliderDouble("Root geometric error (m)", candidate.root_geometric_error_m,
                                10.0, 1'000'000.0, "%.1f", ImGuiSliderFlags_Logarithmic);
        changed |= sliderDouble("Error scale per level", candidate.error_scale_per_level, 0.25,
                                0.8, "%.3f");
        changed |= sliderDouble("Split threshold (px)", candidate.split_threshold_px, 0.25,
                                24.0, "%.2f");
        changed |= sliderDouble("Merge threshold (px)", candidate.merge_threshold_px, 0.0,
                                23.5, "%.2f");
        int minimum_level = static_cast<int>(candidate.min_level);
        int maximum_level = static_cast<int>(candidate.max_level);
        int leaf_budget = static_cast<int>(candidate.leaf_budget);
        changed |= ImGui::SliderInt("Minimum LOD", &minimum_level, 0, 5);
        changed |= ImGui::SliderInt("Maximum LOD", &maximum_level, 1, 18);
        changed |= ImGui::SliderInt("Leaf budget", &leaf_budget, 24,
                                    static_cast<int>(PlanetPatchRenderer::kMaxPatchInstances));
        candidate.min_level = static_cast<std::uint8_t>(std::min(minimum_level, maximum_level));
        candidate.max_level = static_cast<std::uint8_t>(std::max(minimum_level, maximum_level));
        candidate.leaf_budget = static_cast<std::uint32_t>(leaf_budget);
        if (candidate.merge_threshold_px >= candidate.split_threshold_px) {
            candidate.merge_threshold_px =
                std::max(0.0, candidate.split_threshold_px - 0.05);
        }
        if (changed) {
            auto updated = selector.setConfig(candidate, false);
            if (updated.failed()) {
                status_message = updated.error().to_string();
                state.lod = selector.config();
            } else {
                state.lod = candidate;
                status_message.clear();
            }
        }
    }

    if (ImGui::CollapsingHeader("Patch renderer", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Horizon culling", &state.patches.horizon_culling);
        ImGui::SameLine();
        ImGui::Checkbox("Wire overlay", &state.patches.wire_overlay);
        (void)sliderDouble("Maximum render distance (m)",
                           state.patches.max_render_distance_m, 1'000.0, 2'000'000'000.0,
                           "%.0f", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Relief amplitude (m)", &state.patches.relief_amplitude_m, 0.0f,
                           3000.0f, "%.1f");
        ImGui::SliderFloat("Skirt depth (m)", &state.patches.skirt_depth_m, 0.0f, 6000.0f,
                           "%.1f");
        int debug_color = static_cast<int>(state.patches.debug_color);
        char const* modes[] = {"Natural", "LOD level", "Cube face", "Screen-space error"};
        if (ImGui::Combo("Debug color", &debug_color, modes, 4)) {
            state.patches.debug_color = static_cast<PatchDebugColor>(debug_color);
        }
        ImGui::SliderFloat("Heatmap max SSE", &state.patches.max_sse_for_heatmap_px, 0.5f,
                           32.0f, "%.2f px");
        ImGui::TextWrapped("The patch shader uses a deterministic direction-space relief signal. "
                           "This proves continuous geometry/LOD; it is not the final terrain bake.");
    }

    if (ImGui::CollapsingHeader("Live telemetry", ImGuiTreeNodeFlags_DefaultOpen)) {
        double const average_frame_ms = frame_ms.average();
        ImGui::Text("FPS: %.1f", average_frame_ms > 0.0 ? 1000.0 / average_frame_ms : 0.0);
        ImGui::Text("Frame: %.3f ms | p95 %.3f | p99 %.3f", frame_ms.latest(),
                    frame_ms.percentile(95.0), frame_ms.percentile(99.0));
        ImGui::Text("CPU LOD: %.3f ms | submit: %.3f ms", lod_ms.latest(), submit_ms.latest());
        if (render_metrics.gpu_timer_supported && render_metrics.gpu_sample_available) {
            ImGui::Text("GPU planet pass: %.3f ms | p95 %.3f", gpu_ms.latest(),
                        gpu_ms.percentile(95.0));
        } else {
            ImGui::TextUnformatted("GPU duration query: unavailable / warming up");
        }
        ImGui::Text("Leaves: %u selected, %u visible, %u horizon / %u distance culled",
                    render_metrics.selected_leaf_count, render_metrics.visible_leaf_count,
                    render_metrics.horizon_culled_count, render_metrics.distance_culled_count);
        ImGui::Text("Triangles: %llu | draw calls: %u",
                    static_cast<unsigned long long>(render_metrics.triangles),
                    render_metrics.draw_calls);
        ImGui::Text("LOD transitions: +%u split / -%u merge | max leaf SSE %.2f px%s",
                    selection.splits_from_previous, selection.merges_from_previous,
                    selection.max_leaf_sse_px, selection.budget_limited ? " | BUDGET LIMITED" : "");
        drawPerformancePlot("Frame time history (ms)", frame_ms,
                            static_cast<float>(std::max(20.0, frame_ms.percentile(99.0) * 1.25)));

        if (ImGui::BeginTable("LOD levels", 4,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Level");
            ImGui::TableSetupColumn("Leaves");
            ImGui::TableSetupColumn("Mean SSE");
            ImGui::TableSetupColumn("Max SSE");
            ImGui::TableHeadersRow();
            for (planet::LodLevelMetrics const& level : selection.levels) {
                if (level.leaf_count == 0) {
                    continue;
                }
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%u", static_cast<unsigned int>(level.level));
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%u", level.leaf_count);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.2f", level.mean_leaf_sse_px);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%.2f", level.max_leaf_sse_px);
            }
            ImGui::EndTable();
        }
    }

    if (ImGui::CollapsingHeader("Diligent diagnostics")) {
        auto const diagnostics = app.diagnosticsSnapshot();
        std::size_t const first = diagnostics.size() > 16 ? diagnostics.size() - 16 : 0;
        for (std::size_t i = first; i < diagnostics.size(); ++i) {
            DiligentDiagnostic const& diagnostic = diagnostics[i];
            ImVec4 color{0.72f, 0.78f, 0.86f, 1.0f};
            if (diagnostic.severity == Diligent::DEBUG_MESSAGE_SEVERITY_WARNING) {
                color = {1.0f, 0.72f, 0.2f, 1.0f};
            } else if (diagnostic.severity == Diligent::DEBUG_MESSAGE_SEVERITY_ERROR ||
                       diagnostic.severity == Diligent::DEBUG_MESSAGE_SEVERITY_FATAL_ERROR) {
                color = {1.0f, 0.25f, 0.25f, 1.0f};
            }
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::TextWrapped("%s", diagnostic.text.c_str());
            ImGui::PopStyleColor();
        }
    }

    ImGui::End();
}

[[nodiscard]] double milliseconds(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>{end - begin}.count();
}

[[nodiscard]] LabState makeInitialState() {
    LabState state;
    state.lod = defaultPlanetLod(state.patches.planet_radius_m);
    return state;
}

struct CaptureOptions {
    std::string path;
    std::uint32_t width = 1920;
    std::uint32_t height = 1080;
    double route_seconds = 0.0;
};

// Renders one frame of the scripted route on a windowless device.
int runCapture(CaptureOptions const& options) {
    std::string error;
    auto headless = DiligentHeadless::create(true, error);
    if (headless == nullptr) {
        std::fprintf(stderr, "Planet LOD capture initialization failed: %s\n", error.c_str());
        return 1;
    }
    Diligent::IRenderDevice* const device = headless->device();
    Diligent::IDeviceContext* const context = headless->context();

    // The patch pipeline only needs the attachment formats of its target.
    Diligent::SwapChainDesc formats;
    formats.Width = options.width;
    formats.Height = options.height;
    formats.ColorBufferFormat = Diligent::TEX_FORMAT_RGBA8_UNORM_SRGB;
    formats.DepthBufferFormat = Diligent::TEX_FORMAT_D32_FLOAT;
    PlanetPatchRenderer renderer;
    if (!renderer.initialize(device, context, formats, error)) {
        std::fprintf(stderr, "Planet patch renderer initialization failed: %s\n", error.c_str());
        return 1;
    }
    auto color = createColorTarget(device, "Helios planet capture colour", options.width,
                                   options.height, formats.ColorBufferFormat, false);
    Diligent::TextureDesc depth_desc;
    depth_desc.Name = "Helios planet capture depth";
    depth_desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
    depth_desc.Width = options.width;
    depth_desc.Height = options.height;
    depth_desc.Format = formats.DepthBufferFormat;
    depth_desc.BindFlags = Diligent::BIND_DEPTH_STENCIL;
    Diligent::RefCntAutoPtr<Diligent::ITexture> depth;
    device->CreateTexture(depth_desc, nullptr, &depth);
    if (color == nullptr || depth == nullptr) {
        std::fprintf(stderr, "Planet LOD capture: failed to create the offscreen targets\n");
        return 1;
    }

    LabState state = makeInitialState();
    DescentRoute route;
    route.restart(state.camera);
    route.advance(options.route_seconds, state.camera);

    auto selector_result = planet::QuadtreeLodSelector::create(state.lod);
    if (selector_result.failed()) {
        std::fprintf(stderr, "LOD selector initialization failed: %s\n",
                     selector_result.error().to_string().c_str());
        return 1;
    }
    planet::QuadtreeLodSelector selector = std::move(selector_result.value());
    PlanetPatchView const view = makeView(state, options.width, options.height);
    planet::LodViewInput lod_view;
    lod_view.camera_position_body_fixed_m = view.camera_body_fixed_m;
    lod_view.viewport_height_px = options.height;
    lod_view.vertical_fov_rad = view.vertical_fov_rad;
    // The selector keeps hysteresis state between frames; repeat the selection
    // until a stationary camera produces no further splits or merges.
    planet::LodSelection selection;
    for (int pass = 0; pass < 64; ++pass) {
        auto selected = selector.select(lod_view);
        if (selected.failed()) {
            std::fprintf(stderr, "LOD selection failed: %s\n",
                         selected.error().to_string().c_str());
            return 1;
        }
        selection = std::move(selected.value());
        if (pass > 0 && selection.splits_from_previous == 0 &&
            selection.merges_from_previous == 0) {
            break;
        }
    }

    Diligent::ITextureView* rtv = color->GetDefaultView(Diligent::TEXTURE_VIEW_RENDER_TARGET);
    Diligent::ITextureView* dsv = depth->GetDefaultView(Diligent::TEXTURE_VIEW_DEPTH_STENCIL);
    context->SetRenderTargets(1, &rtv, dsv, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    float const clear_color[4]{0.004f, 0.007f, 0.018f, 1.0f};
    context->ClearRenderTarget(rtv, clear_color,
                               Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->ClearDepthStencil(dsv, Diligent::CLEAR_DEPTH_FLAG, 0.0f, 0,
                               Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    PlanetPatchMetrics metrics;
    if (!renderer.render(selection.leaves, view, state.patches, metrics, error)) {
        std::fprintf(stderr, "Planet render failed: %s\n", error.c_str());
        return 1;
    }

    render::RgbaImage image;
    image.width = options.width;
    image.height = options.height;
    if (!readbackTexture(device, context, color, 4, image.pixels, error)) {
        std::fprintf(stderr, "Planet LOD capture: %s\n", error.c_str());
        return 1;
    }
    context->FinishFrame();
    auto written = render::writePngRgba(options.path, image);
    if (written.failed()) {
        std::fprintf(stderr, "Planet LOD capture: %s\n", written.error().to_string().c_str());
        return 1;
    }
    std::printf("Planet LOD capture: wrote %s (%ux%u) at route time %.2f s, altitude %.1f m; "
                "%u leaves visible, %llu triangles, max leaf SSE %.2f px, %u validation "
                "error(s)\n",
                options.path.c_str(), options.width, options.height, options.route_seconds,
                state.camera.altitude_agl_m, metrics.visible_leaf_count,
                static_cast<unsigned long long>(metrics.triangles), selection.max_leaf_sse_px,
                headless->errorCount());
    return headless->errorCount() == 0 ? 0 : 2;
}

// Returns the capture request, or std::nullopt for the interactive route. A
// malformed command line sets `error`.
[[nodiscard]] std::optional<CaptureOptions> parseCapture(int argc, char** argv,
                                                         std::string& error) {
    bool requested = false;
    CaptureOptions options;
    auto number = [&](int& index, std::string_view option, double minimum, double maximum,
                      double& target) {
        if (index + 1 >= argc) {
            error = "missing value for " + std::string{option};
            return false;
        }
        std::string_view const text = argv[++index];
        double parsed = 0.0;
        auto const [end, code] = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (code != std::errc{} || end != text.data() + text.size() || parsed < minimum ||
            parsed > maximum) {
            error = std::string{option} + " is out of range";
            return false;
        }
        target = parsed;
        return true;
    };
    for (int index = 1; index < argc; ++index) {
        std::string_view const argument = argv[index];
        double value = 0.0;
        if (argument == "--capture") {
            if (index + 1 >= argc) {
                error = "--capture needs an output path";
                return std::nullopt;
            }
            options.path = argv[++index];
            requested = true;
        } else if (argument == "--width") {
            if (!number(index, argument, 16.0, 8192.0, value)) {
                return std::nullopt;
            }
            options.width = static_cast<std::uint32_t>(value);
        } else if (argument == "--height") {
            if (!number(index, argument, 16.0, 8192.0, value)) {
                return std::nullopt;
            }
            options.height = static_cast<std::uint32_t>(value);
        } else if (argument == "--route-seconds") {
            if (!number(index, argument, 0.0, 3600.0, value)) {
                return std::nullopt;
            }
            options.route_seconds = value;
        } else {
            error = "unknown option: " + std::string{argument} +
                    " (options: --capture PATH.png [--width N --height N --route-seconds S])";
            return std::nullopt;
        }
    }
    if (!requested) {
        return std::nullopt;
    }
    return options;
}

} // namespace
} // namespace helios::examples

int main(int argc, char** argv) {
    using namespace helios;
    using namespace helios::examples;

    std::string argument_error;
    std::optional<CaptureOptions> const capture = parseCapture(argc, argv, argument_error);
    if (!argument_error.empty()) {
        std::fprintf(stderr, "Planet LOD lab: %s\n", argument_error.c_str());
        return 2;
    }
    if (capture.has_value()) {
        return runCapture(*capture);
    }

    DiligentSdlCreateInfo create_info;
    create_info.title = "Helios v0.0.1 - Vulkan Planet LOD / Descent Lab";
    create_info.enable_validation = true;
    std::string error;
    auto app = DiligentSdlApp::create(create_info, error);
    if (app == nullptr) {
        std::fprintf(stderr, "Planet LOD lab initialization failed: %s\n", error.c_str());
        return 1;
    }

    LabState state = makeInitialState();

    auto selector_result = planet::QuadtreeLodSelector::create(state.lod);
    if (selector_result.failed()) {
        std::fprintf(stderr, "LOD selector initialization failed: %s\n",
                     selector_result.error().to_string().c_str());
        return 1;
    }
    planet::QuadtreeLodSelector selector = std::move(selector_result.value());

    PlanetPatchRenderer renderer;
    if (!renderer.initialize(app->device(), app->context(), app->swapChain()->GetDesc(), error)) {
        std::fprintf(stderr, "Planet patch renderer initialization failed: %s\n", error.c_str());
        return 1;
    }

    DescentRoute route;
    route.restart(state.camera);
    RollingStats frame_ms{360};
    RollingStats lod_ms{360};
    RollingStats submit_ms{360};
    RollingStats gpu_ms{360};
    planet::LodSelection selection;
    PlanetPatchMetrics render_metrics;
    std::string status_message;

    bool running = true;
    Clock::time_point previous_frame = Clock::now();
    while (running) {
        Clock::time_point const frame_begin = Clock::now();
        double const dt_seconds = std::clamp(
            std::chrono::duration<double>{frame_begin - previous_frame}.count(), 0.0, 0.1);
        previous_frame = frame_begin;

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            app->forwardEvent(event);
            if (event.type == SDL_EVENT_QUIT ||
                event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                       !app->wantsKeyboardCapture()) {
                if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
                    running = false;
                } else if (event.key.scancode == SDL_SCANCODE_SPACE) {
                    route.playing = !route.playing;
                } else if (event.key.scancode == SDL_SCANCODE_R) {
                    route.restart(state.camera);
                }
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
        route.advance(dt_seconds, state.camera);
        if (!route.playing) {
            applyManualCameraInput(*app, state, dt_seconds);
        }

        std::uint32_t const viewport_width = std::max(
            1U, static_cast<std::uint32_t>(
                    std::lround(static_cast<double>(app->width()) * state.render_scale)));
        std::uint32_t const viewport_height = std::max(
            1U, static_cast<std::uint32_t>(
                    std::lround(static_cast<double>(app->height()) * state.render_scale)));
        PlanetPatchView const view = makeView(state, viewport_width, viewport_height);

        Clock::time_point const lod_begin = Clock::now();
        planet::LodViewInput lod_view;
        lod_view.camera_position_body_fixed_m = view.camera_body_fixed_m;
        lod_view.viewport_height_px = viewport_height;
        lod_view.vertical_fov_rad = view.vertical_fov_rad;
        auto selected = selector.select(lod_view);
        Clock::time_point const lod_end = Clock::now();
        lod_ms.push(milliseconds(lod_begin, lod_end));
        if (selected.failed()) {
            std::fprintf(stderr, "LOD selection failed: %s\n",
                         selected.error().to_string().c_str());
            return 1;
        }
        selection = std::move(selected.value());

        float const clear_color[4]{0.004f, 0.007f, 0.018f, 1.0f};
        app->bindAndClearBackBuffer(clear_color, 0.0f);
        app->setSceneViewport(state.render_scale);
        Clock::time_point const submit_begin = Clock::now();
        if (!renderer.render(selection.leaves, view, state.patches, render_metrics, error)) {
            std::fprintf(stderr, "Planet render failed: %s\n", error.c_str());
            return 1;
        }
        Clock::time_point const submit_end = Clock::now();
        submit_ms.push(milliseconds(submit_begin, submit_end));
        if (render_metrics.gpu_sample_new) {
            gpu_ms.push(render_metrics.gpu_planet_pass_ms);
        }

        drawControlPanel(*app, state, route, selector, selection, render_metrics, frame_ms, lod_ms,
                         submit_ms, gpu_ms, status_message);
        app->renderImGui();
        app->present(state.v_sync);
        frame_ms.push(milliseconds(frame_begin, Clock::now()));
    }

    return app->errorCount() == 0 ? 0 : 2;
}
