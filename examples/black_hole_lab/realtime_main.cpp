// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Real-time Schwarzschild black-hole lab (Diligent/Vulkan).
//
// Four routes share one renderer:
//   - interactive   an SDL window with an orbit camera and ImGui controls;
//   - --capture     a windowless offscreen render written as a PNG;
//   - --benchmark   a windowless timing run with GPU duration queries;
//   - --validate    a windowless per-pixel comparison of the float32 GPU
//                   geodesics against the binary64 CPU reference tracer.
// The windowless routes create no window or swap chain, so they never appear
// on, or take input focus from, the desktop.  The interactive route uses no
// mouse grab, relative mouse mode, global input hook, or screen capture.

#include "black_hole_renderer.hpp"
#include "diligent_headless.hpp"
#include "diligent_sdl_app.hpp"
#include "rolling_stats.hpp"

#include "helios/blackhole/ray_tracer.hpp"
#include "helios/blackhole/schwarzschild.hpp"
#include "helios/blackhole/thin_disk.hpp"
#include "helios/math/vec.hpp"
#include "helios/render/png_writer.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#ifndef HELIOS_GIT_HASH
#define HELIOS_GIT_HASH "unknown"
#endif
#ifndef HELIOS_DILIGENT_COMMIT
#define HELIOS_DILIGENT_COMMIT "unknown"
#endif
#ifndef HELIOS_DILIGENT_CORE_COMMIT
#define HELIOS_DILIGENT_CORE_COMMIT "unknown"
#endif
#ifndef HELIOS_BUILD_TYPE
#define HELIOS_BUILD_TYPE "unknown"
#endif

namespace helios::examples {
namespace {

using Clock = std::chrono::steady_clock;
using Json = nlohmann::ordered_json;

constexpr double kRadiansPerDegree = std::numbers::pi / 180.0;
constexpr double kDegreesPerRadian = 180.0 / std::numbers::pi;

// The disk pattern is periodic in this many gravitational times; wrapping the
// clock keeps the float32 time uniform precise in long sessions.
constexpr double kDiskTimeWrapInMasses = 180.0 * 1024.0;

enum class Mode : std::uint8_t { Interactive, Capture, Benchmark, Validate };

// A static observer on a sphere around the hole, looking at the centre with
// an optional yaw/pitch offset.
struct OrbitCamera {
    double distance_in_masses = 75.0;
    double elevation_deg = 6.0;
    double azimuth_deg = -90.0;
    double look_yaw_deg = 0.0;
    double look_pitch_deg = 0.0;
    double vertical_fov_deg = 38.0;
};

struct LabState {
    OrbitCamera orbit{};
    BlackHoleSettings scene{};
    float render_scale = 1.0f;
    bool v_sync = true;
    bool animate_disk = true;
    float time_rate_in_masses_per_second = 6.0f;
    float auto_orbit_deg_per_second = 0.0f;
    int quality_preset = 1;
};

struct Options {
    Mode mode = Mode::Interactive;
    LabState state{};
    std::uint32_t width = 1920;
    std::uint32_t height = 1080;
    int supersample = 1;
    int frames = 240;
    int smoke_frames = 0;
    std::optional<bool> validation_layers{};
    std::string capture_path{};
    std::string report_path{};
    bool show_help = false;
};

constexpr std::array<char const*, 3> kQualityPresetNames{"Performance", "Balanced", "Ultra"};

void applyQualityPreset(LabState& state, int preset) {
    state.quality_preset = std::clamp(preset, 0, 2);
    BlackHoleSettings& scene = state.scene;
    switch (state.quality_preset) {
    // Samples per pixel are spent adaptively (see black_hole_refine.slang),
    // so the presets differ mostly in how finely edges are resolved.
    case 0:
        scene.steps_per_half_turn = 24;
        scene.max_image_orders = 2;
        scene.samples_per_pixel = 1;
        scene.critical_samples = 4;
        break;
    case 1:
        scene.steps_per_half_turn = 32;
        scene.max_image_orders = 3;
        scene.samples_per_pixel = 4;
        scene.critical_samples = 8;
        break;
    default:
        scene.steps_per_half_turn = 48;
        scene.max_image_orders = 4;
        scene.samples_per_pixel = 8;
        scene.critical_samples = 12;
        break;
    }
}

[[nodiscard]] BlackHoleCamera makeCamera(OrbitCamera const& orbit) {
    double const elevation = std::clamp(orbit.elevation_deg, -89.5, 89.5) * kRadiansPerDegree;
    double const azimuth = orbit.azimuth_deg * kRadiansPerDegree;
    double const distance =
        std::max(orbit.distance_in_masses, BlackHoleRenderer::kMinimumCameraRadiusInMasses);
    math::Vec3d const outward{std::cos(elevation) * std::cos(azimuth),
                              std::cos(elevation) * std::sin(azimuth), std::sin(elevation)};
    math::Vec3d const toward_centre = outward * -1.0;
    math::Vec3d const base_right =
        math::normalized(math::cross(toward_centre, math::Vec3d{0.0, 0.0, 1.0}));
    math::Vec3d const base_up = math::normalized(math::cross(base_right, toward_centre));

    double const yaw = orbit.look_yaw_deg * kRadiansPerDegree;
    double const pitch = orbit.look_pitch_deg * kRadiansPerDegree;
    BlackHoleCamera camera;
    camera.position_in_masses = outward * distance;
    camera.forward = math::normalized(toward_centre * (std::cos(yaw) * std::cos(pitch)) +
                                      base_right * (std::sin(yaw) * std::cos(pitch)) +
                                      base_up * std::sin(pitch));
    camera.right = math::normalized(math::cross(camera.forward, base_up));
    camera.up = math::normalized(math::cross(camera.right, camera.forward));
    camera.vertical_fov_rad = std::clamp(orbit.vertical_fov_deg, 1.0, 150.0) * kRadiansPerDegree;
    return camera;
}

[[nodiscard]] double milliseconds(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>{end - begin}.count();
}

[[nodiscard]] std::uint64_t fnv1a64(std::vector<std::uint8_t> const& bytes) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (std::uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

[[nodiscard]] std::string hex64(std::uint64_t value) {
    char buffer[17]{};
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
    return buffer;
}

[[nodiscard]] char const* skyModeName(BlackHoleSkyMode mode) {
    switch (mode) {
    case BlackHoleSkyMode::Stars:
        return "stars";
    case BlackHoleSkyMode::CelestialGrid:
        return "grid";
    case BlackHoleSkyMode::Black:
        return "black";
    }
    return "stars";
}

// ---------------------------------------------------------------------------
// Reports
// ---------------------------------------------------------------------------

[[nodiscard]] Json describeScene(LabState const& state) {
    BlackHoleSettings const& scene = state.scene;
    OrbitCamera const& orbit = state.orbit;
    double const lapse = std::sqrt(1.0 - 2.0 / orbit.distance_in_masses);
    double const shadow_sine =
        std::min(1.0, blackhole::criticalImpactParameter(1.0) / orbit.distance_in_masses * lapse);
    return Json{
        {"camera",
         {{"model", "static observer, pinhole, looking at the centre plus yaw/pitch offset"},
          {"distance_M", orbit.distance_in_masses},
          {"elevation_deg", orbit.elevation_deg},
          {"azimuth_deg", orbit.azimuth_deg},
          {"look_yaw_deg", orbit.look_yaw_deg},
          {"look_pitch_deg", orbit.look_pitch_deg},
          {"vertical_fov_deg", orbit.vertical_fov_deg},
          {"lapse", lapse},
          {"shadow_angular_diameter_deg",
           orbit.distance_in_masses >= 3.0 ? 2.0 * std::asin(shadow_sine) * kDegreesPerRadian
                                           : 0.0}}},
        {"disk",
         {{"enabled", scene.disk_enabled},
          {"inner_radius_M", BlackHoleRenderer::kDiskInnerRadiusInMasses},
          {"outer_radius_M", scene.disk_outer_radius_in_masses},
          {"peak_temperature_K", scene.disk_peak_temperature_k},
          {"flux_profile", "Page-Thorne zero-torque, a*=0"},
          {"spectrum", "black body at g*T_eff, CIE 1931 -> linear sRGB table"},
          {"opacity", scene.disk_opacity},
          {"turbulence_amplitude", scene.disk_turbulence},
          {"limb_darkening", scene.limb_darkening},
          {"time_M", scene.disk_time_in_masses}}},
        {"relativity",
         {{"spacetime", scene.curvature ? "Schwarzschild, a*=0" : "flat comparison"},
          {"doppler_term", scene.doppler},
          {"gravitational_and_time_dilation_term", scene.gravitational_shift}}},
        {"sky",
         {{"mode", skyModeName(scene.sky_mode)},
          {"star_intensity", scene.star_intensity},
          {"galaxy_intensity", scene.galaxy_intensity}}},
        {"display",
         {{"exposure_ev", scene.exposure_ev},
          {"glare", scene.glare},
          {"tone_map", scene.tone_map ? "ACES fit" : "clamp"},
          {"dither", scene.dither}}},
        {"quality",
         {{"rk4_steps_per_half_turn", scene.steps_per_half_turn},
          {"max_image_orders", scene.max_image_orders},
          {"samples_per_pixel", scene.samples_per_pixel},
          {"critical_curve_extra_samples", scene.critical_samples},
          {"adaptive_sampling", scene.adaptive_sampling},
          {"adaptive_contrast_threshold", scene.adaptive_contrast}}},
    };
}

[[nodiscard]] Json describeBuild(std::string const& adapter) {
    return Json{{"helios_commit", HELIOS_GIT_HASH},
                {"build_type", HELIOS_BUILD_TYPE},
                {"diligent_commit", HELIOS_DILIGENT_COMMIT},
                {"diligent_core_commit", HELIOS_DILIGENT_CORE_COMMIT},
                {"backend", "Diligent Vulkan"},
                {"adapter", adapter}};
}

[[nodiscard]] bool writeReport(std::string const& path, Json const& report) {
    if (path.empty()) {
        return true;
    }
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file << report.dump(2) << '\n';
    file.flush();
    if (!file.good()) {
        std::fprintf(stderr, "BlackHoleLab: failed to write %s\n", path.c_str());
        return false;
    }
    return true;
}

[[nodiscard]] Json summarize(RollingStats const& stats) {
    return Json{{"samples", stats.size()},   {"mean", stats.average()},
                {"p50", stats.percentile(50.0)}, {"p95", stats.percentile(95.0)},
                {"p99", stats.percentile(99.0)}, {"min", stats.minimum()},
                {"max", stats.maximum()}};
}

// ---------------------------------------------------------------------------
// Windowless routes
// ---------------------------------------------------------------------------

struct HeadlessContext {
    std::unique_ptr<DiligentHeadless> device;
    BlackHoleRenderer renderer;
};

[[nodiscard]] bool createHeadless(HeadlessContext& headless, bool validation_layers,
                                  bool float32_scene, std::string& error) {
    headless.device = DiligentHeadless::create(validation_layers, error);
    if (headless.device == nullptr) {
        return false;
    }
    BlackHoleRenderer::CreateInfo info;
    info.output_format = Diligent::TEX_FORMAT_RGBA8_UNORM_SRGB;
    info.float32_scene = float32_scene;
    return headless.renderer.initialize(headless.device->device(), headless.device->context(),
                                        info, error);
}

void finishHeadlessFrame(DiligentHeadless& device) {
    device.context()->Flush();
    device.context()->FinishFrame();
    device.device()->ReleaseStaleResources();
}

[[nodiscard]] Json describeDiagnostics(DiligentHeadless const& device) {
    return Json{{"validation_warnings", device.warningCount()},
                {"validation_errors", device.errorCount()},
                {"messages", device.warningAndErrorMessages()}};
}

int runCapture(Options const& options) {
    std::string error;
    HeadlessContext headless;
    if (!createHeadless(headless, options.validation_layers.value_or(true), false, error)) {
        std::fprintf(stderr, "BlackHoleLab: %s\n", error.c_str());
        return 1;
    }
    Diligent::IRenderDevice* const device = headless.device->device();
    Diligent::IDeviceContext* const context = headless.device->context();

    auto target = createColorTarget(device, "Helios black-hole capture target", options.width,
                                    options.height, Diligent::TEX_FORMAT_RGBA8_UNORM_SRGB, false);
    if (target == nullptr) {
        std::fprintf(stderr, "BlackHoleLab: failed to create the capture target\n");
        return 1;
    }

    LabState state = options.state;
    state.scene.camera = makeCamera(state.orbit);
    std::uint32_t const scene_width = options.width * static_cast<std::uint32_t>(options.supersample);
    std::uint32_t const scene_height =
        options.height * static_cast<std::uint32_t>(options.supersample);

    BlackHoleMetrics metrics;
    Clock::time_point const begin = Clock::now();
    if (!headless.renderer.render(state.scene, scene_width, scene_height,
                                  target->GetDefaultView(Diligent::TEXTURE_VIEW_RENDER_TARGET),
                                  options.width, options.height, metrics, error)) {
        std::fprintf(stderr, "BlackHoleLab: %s\n", error.c_str());
        return 1;
    }
    render::RgbaImage image;
    image.width = options.width;
    image.height = options.height;
    if (!readbackTexture(device, context, target, 4, image.pixels, error)) {
        std::fprintf(stderr, "BlackHoleLab: %s\n", error.c_str());
        return 1;
    }
    double const elapsed_ms = milliseconds(begin, Clock::now());
    finishHeadlessFrame(*headless.device);

    auto written = render::writePngRgba(options.capture_path, image);
    if (written.failed()) {
        std::fprintf(stderr, "BlackHoleLab: %s\n", written.error().to_string().c_str());
        return 1;
    }

    std::string const checksum = hex64(fnv1a64(image.pixels));
    Json report{{"schema_version", 1},
                {"lab", "Helios BlackHoleLab real-time capture"},
                {"build", describeBuild(headless.device->adapterName())},
                {"image",
                 {{"path", options.capture_path},
                  {"width", options.width},
                  {"height", options.height},
                  {"scene_width", scene_width},
                  {"scene_height", scene_height},
                  {"rgba8_fnv1a64", checksum}}},
                {"scene", describeScene(state)},
                {"first_frame_including_pipeline_warmup_ms", elapsed_ms},
                {"diagnostics", describeDiagnostics(*headless.device)}};
    if (!writeReport(options.report_path, report)) {
        return 1;
    }
    std::printf("BlackHoleLab: wrote %s (%ux%u, scene %ux%u, fnv1a64 %s), %u validation error(s)\n",
                options.capture_path.c_str(), options.width, options.height, scene_width,
                scene_height, checksum.c_str(), headless.device->errorCount());
    return headless.device->errorCount() == 0 ? 0 : 2;
}

int runBenchmark(Options const& options) {
    std::string error;
    HeadlessContext headless;
    // Validation layers intercept every call; they are off unless requested so
    // the numbers describe the renderer rather than the layer.
    bool const validation_layers = options.validation_layers.value_or(false);
    if (!createHeadless(headless, validation_layers, false, error)) {
        std::fprintf(stderr, "BlackHoleLab: %s\n", error.c_str());
        return 1;
    }
    Diligent::IRenderDevice* const device = headless.device->device();
    Diligent::IDeviceContext* const context = headless.device->context();
    auto target = createColorTarget(device, "Helios black-hole benchmark target", options.width,
                                    options.height, Diligent::TEX_FORMAT_RGBA8_UNORM_SRGB, false);
    if (target == nullptr) {
        std::fprintf(stderr, "BlackHoleLab: failed to create the benchmark target\n");
        return 1;
    }
    Diligent::ITextureView* const target_view =
        target->GetDefaultView(Diligent::TEXTURE_VIEW_RENDER_TARGET);

    LabState state = options.state;
    std::uint32_t const scene_width = options.width * static_cast<std::uint32_t>(options.supersample);
    std::uint32_t const scene_height =
        options.height * static_cast<std::uint32_t>(options.supersample);
    RollingStats trace_ms{4096};
    RollingStats post_ms{4096};

    auto renderFrames = [&](int count, bool record) {
        for (int frame = 0; frame < count; ++frame) {
            // A slow orbit and a running disk clock make every frame distinct.
            state.orbit.azimuth_deg += 0.25;
            state.scene.disk_time_in_masses =
                std::fmod(state.scene.disk_time_in_masses + 0.1, kDiskTimeWrapInMasses);
            state.scene.camera = makeCamera(state.orbit);
            BlackHoleMetrics metrics;
            if (!headless.renderer.render(state.scene, scene_width, scene_height, target_view,
                                          options.width, options.height, metrics, error)) {
                return false;
            }
            finishHeadlessFrame(*headless.device);
            if (record && metrics.gpu_sample_new) {
                trace_ms.push(metrics.gpu_trace_ms);
                post_ms.push(metrics.gpu_post_ms);
            }
        }
        return true;
    };

    constexpr int kWarmupFrames = 24;
    if (!renderFrames(kWarmupFrames, false)) {
        std::fprintf(stderr, "BlackHoleLab: %s\n", error.c_str());
        return 1;
    }
    context->WaitForIdle();

    // No presentation or frame pacing: the CPU submits far faster than the GPU
    // executes, so wall time over the batch is the GPU cost of one frame.
    Clock::time_point const begin = Clock::now();
    if (!renderFrames(options.frames, true)) {
        std::fprintf(stderr, "BlackHoleLab: %s\n", error.c_str());
        return 1;
    }
    context->WaitForIdle();
    double const total_ms = milliseconds(begin, Clock::now());
    double const frame_ms = total_ms / static_cast<double>(options.frames);
    double const megapixels =
        static_cast<double>(scene_width) * static_cast<double>(scene_height) * 1.0e-6;

    Json report{{"schema_version", 1},
                {"lab", "Helios BlackHoleLab real-time benchmark"},
                {"build", describeBuild(headless.device->adapterName())},
                {"method",
                 "windowless; N frames submitted without presentation or pacing, then "
                 "WaitForIdle; frame time is wall time divided by N. Per-pass values are GPU "
                 "duration queries. Other GPU workloads on the machine are not controlled."},
                {"validation_layers", validation_layers},
                {"output_width", options.width},
                {"output_height", options.height},
                {"scene_width", scene_width},
                {"scene_height", scene_height},
                {"frames", options.frames},
                {"frame_ms", frame_ms},
                {"frames_per_second", frame_ms > 0.0 ? 1000.0 / frame_ms : 0.0},
                {"megapixels_per_second",
                 frame_ms > 0.0 ? megapixels * 1000.0 / frame_ms : 0.0},
                {"gpu_trace_pass_ms", summarize(trace_ms)},
                {"gpu_post_passes_ms", summarize(post_ms)},
                {"scene", describeScene(state)},
                {"diagnostics", describeDiagnostics(*headless.device)}};
    if (!writeReport(options.report_path, report)) {
        return 1;
    }
    std::printf("BlackHoleLab benchmark: %ux%u (scene %ux%u), %d frames\n"
                "  frame %.3f ms = %.1f fps\n"
                "  GPU trace pass  p50 %.3f ms  p95 %.3f ms\n"
                "  GPU post passes p50 %.3f ms  p95 %.3f ms\n"
                "  validation layers %s, %u error(s)\n",
                options.width, options.height, scene_width, scene_height, options.frames,
                frame_ms, frame_ms > 0.0 ? 1000.0 / frame_ms : 0.0, trace_ms.percentile(50.0),
                trace_ms.percentile(95.0), post_ms.percentile(50.0), post_ms.percentile(95.0),
                validation_layers ? "on" : "off", headless.device->errorCount());
    return headless.device->errorCount() == 0 ? 0 : 2;
}

struct ErrorSeries {
    std::vector<double> values;

    [[nodiscard]] Json summary() {
        if (values.empty()) {
            return Json{{"samples", 0}};
        }
        std::sort(values.begin(), values.end());
        auto at = [&](double fraction) {
            return values[static_cast<std::size_t>(fraction * static_cast<double>(values.size() - 1U))];
        };
        return Json{{"samples", values.size()}, {"median", at(0.5)}, {"p99", at(0.99)},
                    {"max", values.back()}};
    }
    [[nodiscard]] double maximum() const {
        return values.empty() ? 0.0 : *std::max_element(values.begin(), values.end());
    }
};

// Compares the first event of every pixel-centre ray - captured, escaped (and
// in which asymptotic direction), or the first disk crossing (and where) -
// between the float32 shader and the adaptive binary64 reference tracer.
int runValidation(Options const& options) {
    std::string error;
    HeadlessContext headless;
    if (!createHeadless(headless, options.validation_layers.value_or(true), true, error)) {
        std::fprintf(stderr, "BlackHoleLab: %s\n", error.c_str());
        return 1;
    }

    LabState state = options.state;
    state.scene.camera = makeCamera(state.orbit);
    state.scene.output = BlackHoleOutput::GeometryRecord;
    BlackHoleMetrics metrics;
    if (!headless.renderer.render(state.scene, options.width, options.height, nullptr, 0, 0,
                                  metrics, error)) {
        std::fprintf(stderr, "BlackHoleLab: %s\n", error.c_str());
        return 1;
    }
    std::vector<std::uint8_t> bytes;
    if (!readbackTexture(headless.device->device(), headless.device->context(),
                         headless.renderer.sceneTexture(), 4 * sizeof(float), bytes, error)) {
        std::fprintf(stderr, "BlackHoleLab: %s\n", error.c_str());
        return 1;
    }
    finishHeadlessFrame(*headless.device);

    blackhole::RayIntegrationSettings reference{};
    reference.geometry.mass = 1.0;
    reference.disk.enabled = state.scene.disk_enabled;
    reference.disk.innerRadiusInMasses = BlackHoleRenderer::kDiskInnerRadiusInMasses;
    reference.disk.outerRadiusInMasses = state.scene.disk_outer_radius_in_masses;
    reference.disk.normal = math::Vec3d{0.0, 0.0, 1.0};
    // Far enough that the remaining bend to infinity is the flat-space
    // asin(b / r) to better than 1e-8 rad.
    reference.escapeRadiusInMasses = 1.0e5;
    reference.initialStepInMasses = 0.05;
    reference.minStepInMasses = 1.0e-7;
    reference.maxStepInMasses = 1.0e4;
    reference.absoluteTolerance = 1.0e-11;
    reference.relativeTolerance = 1.0e-10;
    reference.maxRadialFractionPerStep = 0.03;
    reference.maxOrbitalAnglePerStep = 0.03;
    reference.maxSteps = 400000;
    reference.maxAffineLengthInMasses = 1.0e7;

    BlackHoleCamera const& camera = state.scene.camera;
    double const tangent_half_fov = std::tan(camera.vertical_fov_rad * 0.5);
    double const aspect = static_cast<double>(options.width) / static_cast<double>(options.height);
    double const camera_radius = math::length(camera.position_in_masses);
    math::Vec3d const radial = camera.position_in_masses / camera_radius;
    double const critical_impact = blackhole::criticalImpactParameter(1.0);
    // Within this band of the critical curve a ray orbits the photon sphere
    // and amplifies any perturbation exponentially, so pointwise agreement is
    // not a meaningful requirement; those pixels are counted separately.
    constexpr double kCriticalBandInMasses = 0.03;

    std::uint64_t compared = 0;
    std::uint64_t critical_band = 0;
    std::uint64_t class_mismatch = 0;
    std::uint64_t unresolved_reference = 0;
    std::array<std::uint64_t, 3> class_counts{};
    ErrorSeries direction_error;
    ErrorSeries disk_radius_error;
    ErrorSeries disk_position_error;

    for (std::uint32_t y = 0; y < options.height; ++y) {
        for (std::uint32_t x = 0; x < options.width; ++x) {
            float record[4];
            std::memcpy(record,
                        bytes.data() + (static_cast<std::size_t>(y) * options.width + x) *
                                           sizeof(record),
                        sizeof(record));

            double const ndc_x =
                (static_cast<double>(x) + 0.5) * 2.0 / static_cast<double>(options.width) - 1.0;
            double const ndc_y =
                1.0 - (static_cast<double>(y) + 0.5) * 2.0 / static_cast<double>(options.height);
            math::Vec3d const direction =
                math::normalized(camera.forward + camera.right * (ndc_x * aspect * tangent_half_fov) +
                                 camera.up * (ndc_y * tangent_half_fov));
            blackhole::RayTraceResult const result = blackhole::traceSchwarzschildRay(
                blackhole::RayInitialCondition{camera.position_in_masses, direction}, reference);

            int reference_class = -1;
            switch (result.termination) {
            case blackhole::RayTermination::Horizon:
                reference_class = 0;
                break;
            case blackhole::RayTermination::Escaped:
                reference_class = 1;
                break;
            case blackhole::RayTermination::AccretionDisk:
                reference_class = 2;
                break;
            default:
                break;
            }
            if (reference_class < 0) {
                ++unresolved_reference;
                continue;
            }
            if (std::abs(result.diagnostics.impactParameter - critical_impact) <
                kCriticalBandInMasses) {
                ++critical_band;
                continue;
            }
            ++compared;
            ++class_counts[static_cast<std::size_t>(reference_class)];
            int const gpu_class = static_cast<int>(std::lround(record[3]));
            if (gpu_class != reference_class) {
                ++class_mismatch;
                continue;
            }

            if (reference_class == 1) {
                double const radial_component = math::dot(direction, radial);
                math::Vec3d const tangential = direction - radial * radial_component;
                double const tangential_length = math::length(tangential);
                math::Vec3d expected = direction;
                if (tangential_length > 1.0e-9) {
                    math::Vec3d const tangent = tangential / tangential_length;
                    double const asymptote =
                        result.terminalSample.orbitalAngle +
                        std::asin(std::min(1.0, result.diagnostics.impactParameter /
                                                    result.terminalSample.radius));
                    expected = radial * std::cos(asymptote) + tangent * std::sin(asymptote);
                }
                math::Vec3d const measured{record[0], record[1], record[2]};
                direction_error.values.push_back(math::length(measured - expected));
            } else if (reference_class == 2) {
                disk_radius_error.values.push_back(
                    std::abs(static_cast<double>(record[0]) - result.diskHit.radius));
                disk_position_error.values.push_back(
                    std::hypot(static_cast<double>(record[1]) - result.diskHit.position[0],
                               static_cast<double>(record[2]) - result.diskHit.position[1]));
            }
        }
    }

    // Tolerances for float32 RK4 against the binary64 reference.  Class
    // disagreements are only expected where a ray grazes an edge (the disk
    // rim, the ISCO, or the shadow) within float32 resolution.
    constexpr double kMaxClassMismatchFraction = 2.0e-3;
    constexpr double kMaxDirectionError = 2.0e-3;    // rad
    constexpr double kMaxMedianDirectionError = 5.0e-5;
    constexpr double kMaxDiskRadiusError = 2.0e-2;   // M
    double const mismatch_fraction =
        compared > 0 ? static_cast<double>(class_mismatch) / static_cast<double>(compared) : 1.0;
    Json const direction_summary = direction_error.summary();
    Json const radius_summary = disk_radius_error.summary();
    Json const position_summary = disk_position_error.summary();
    bool const passed =
        compared > 0 && unresolved_reference == 0 && mismatch_fraction <= kMaxClassMismatchFraction &&
        direction_error.maximum() <= kMaxDirectionError &&
        direction_summary.value("median", 0.0) <= kMaxMedianDirectionError &&
        disk_radius_error.maximum() <= kMaxDiskRadiusError && headless.device->errorCount() == 0;

    Json report{
        {"schema_version", 1},
        {"lab", "Helios BlackHoleLab GPU-versus-CPU geodesic validation"},
        {"build", describeBuild(headless.device->adapterName())},
        {"gpu", "float32 RK4 on the Binet equation, fixed angular step"},
        {"reference", "adaptive Dormand-Prince RK5(4), binary64 (helios_blackhole)"},
        {"width", options.width},
        {"height", options.height},
        {"scene", describeScene(state)},
        {"pixels",
         {{"total", static_cast<std::uint64_t>(options.width) * options.height},
          {"compared", compared},
          {"excluded_critical_band", critical_band},
          {"critical_band_half_width_M", kCriticalBandInMasses},
          {"reference_unresolved", unresolved_reference},
          {"reference_captured", class_counts[0]},
          {"reference_escaped", class_counts[1]},
          {"reference_disk", class_counts[2]}}},
        {"classification",
         {{"mismatches", class_mismatch},
          {"mismatch_fraction", mismatch_fraction},
          {"allowed_fraction", kMaxClassMismatchFraction}}},
        {"escape_direction_error_rad", direction_summary},
        {"disk_radius_error_M", radius_summary},
        {"disk_position_error_M", position_summary},
        {"tolerances",
         {{"max_escape_direction_error_rad", kMaxDirectionError},
          {"max_median_escape_direction_error_rad", kMaxMedianDirectionError},
          {"max_disk_radius_error_M", kMaxDiskRadiusError}}},
        {"diagnostics", describeDiagnostics(*headless.device)},
        {"status", passed ? "pass" : "fail"}};
    if (!writeReport(options.report_path, report)) {
        return 1;
    }
    std::printf("BlackHoleLab validation: %ux%u, %llu compared, %llu in the critical band\n"
                "  class mismatches %llu (%.4f %%)\n"
                "  escape direction error  median %.3e  p99 %.3e  max %.3e rad\n"
                "  disk radius error       median %.3e  p99 %.3e  max %.3e M\n"
                "  status %s\n",
                options.width, options.height, static_cast<unsigned long long>(compared),
                static_cast<unsigned long long>(critical_band),
                static_cast<unsigned long long>(class_mismatch), mismatch_fraction * 100.0,
                direction_summary.value("median", 0.0), direction_summary.value("p99", 0.0),
                direction_summary.value("max", 0.0), radius_summary.value("median", 0.0),
                radius_summary.value("p99", 0.0), radius_summary.value("max", 0.0),
                passed ? "PASS" : "FAIL");
    return passed ? 0 : 3;
}

// ---------------------------------------------------------------------------
// Interactive route
// ---------------------------------------------------------------------------

constexpr char const* kSettingsPath = "helios_black_hole_settings.json";

[[nodiscard]] Json settingsToJson(LabState const& state) {
    BlackHoleSettings const& scene = state.scene;
    return Json{
        {"schema", "helios.black-hole-settings"},
        {"version", 1},
        {"camera",
         {{"distance_M", state.orbit.distance_in_masses},
          {"elevation_deg", state.orbit.elevation_deg},
          {"azimuth_deg", state.orbit.azimuth_deg},
          {"look_yaw_deg", state.orbit.look_yaw_deg},
          {"look_pitch_deg", state.orbit.look_pitch_deg},
          {"vertical_fov_deg", state.orbit.vertical_fov_deg},
          {"auto_orbit_deg_per_second", state.auto_orbit_deg_per_second}}},
        {"disk",
         {{"enabled", scene.disk_enabled},
          {"outer_radius_M", scene.disk_outer_radius_in_masses},
          {"peak_temperature_K", scene.disk_peak_temperature_k},
          {"opacity", scene.disk_opacity},
          {"turbulence", scene.disk_turbulence},
          {"limb_darkening", scene.limb_darkening},
          {"animate", state.animate_disk},
          {"time_rate_M_per_second", state.time_rate_in_masses_per_second}}},
        {"relativity",
         {{"doppler", scene.doppler},
          {"gravitational_shift", scene.gravitational_shift},
          {"curvature", scene.curvature}}},
        {"sky",
         {{"mode", static_cast<int>(scene.sky_mode)},
          {"star_intensity", scene.star_intensity},
          {"galaxy_intensity", scene.galaxy_intensity}}},
        {"display",
         {{"exposure_ev", scene.exposure_ev},
          {"glare", scene.glare},
          {"tone_map", scene.tone_map},
          {"dither", scene.dither},
          {"render_scale", state.render_scale},
          {"v_sync", state.v_sync}}},
        {"quality",
         {{"preset", state.quality_preset},
          {"steps_per_half_turn", scene.steps_per_half_turn},
          {"max_image_orders", scene.max_image_orders},
          {"samples_per_pixel", scene.samples_per_pixel},
          {"critical_samples", scene.critical_samples},
          {"adaptive_sampling", scene.adaptive_sampling},
          {"adaptive_contrast", scene.adaptive_contrast}}},
    };
}

[[nodiscard]] bool validState(LabState const& state) {
    BlackHoleSettings const& scene = state.scene;
    OrbitCamera const& orbit = state.orbit;
    auto inRange = [](double value, double minimum, double maximum) {
        return std::isfinite(value) && value >= minimum && value <= maximum;
    };
    return inRange(orbit.distance_in_masses, BlackHoleRenderer::kMinimumCameraRadiusInMasses, 5000.0) &&
           inRange(orbit.elevation_deg, -89.5, 89.5) && inRange(orbit.azimuth_deg, -720.0, 720.0) &&
           inRange(orbit.look_yaw_deg, -180.0, 180.0) && inRange(orbit.look_pitch_deg, -89.0, 89.0) &&
           inRange(orbit.vertical_fov_deg, 5.0, 120.0) &&
           inRange(state.auto_orbit_deg_per_second, -90.0, 90.0) &&
           inRange(scene.disk_outer_radius_in_masses, 8.0, 200.0) &&
           inRange(scene.disk_peak_temperature_k, 1000.0, 500000.0) &&
           inRange(scene.disk_opacity, 0.0, 1.0) && inRange(scene.disk_turbulence, 0.0, 0.95) &&
           inRange(state.time_rate_in_masses_per_second, 0.0, 200.0) &&
           static_cast<int>(scene.sky_mode) <= static_cast<int>(BlackHoleSkyMode::Black) &&
           inRange(scene.star_intensity, 0.0, 16.0) && inRange(scene.galaxy_intensity, 0.0, 16.0) &&
           inRange(scene.exposure_ev, -8.0, 8.0) && inRange(scene.glare, 0.0, 1.0) &&
           inRange(state.render_scale, 0.25, 2.0) && inRange(scene.steps_per_half_turn, 8, 128) &&
           inRange(scene.max_image_orders, 1, 8) && inRange(scene.samples_per_pixel, 1, 16) &&
           inRange(scene.critical_samples, 0, 32);
}

[[nodiscard]] bool saveSettings(LabState const& state, std::string& status_message) {
    std::ofstream output{kSettingsPath, std::ios::binary | std::ios::trunc};
    output << settingsToJson(state).dump(2) << '\n';
    output.flush();
    if (!output.good()) {
        status_message = std::string{"Failed to write settings file: "} + kSettingsPath;
        return false;
    }
    status_message = std::string{"Saved settings to "} + kSettingsPath;
    return true;
}

[[nodiscard]] bool loadSettings(LabState& state, std::string& status_message) {
    try {
        std::ifstream input{kSettingsPath, std::ios::binary};
        if (!input) {
            status_message = std::string{"Could not open settings file: "} + kSettingsPath;
            return false;
        }
        Json document;
        input >> document;
        if (document.at("schema").get<std::string>() != "helios.black-hole-settings" ||
            document.at("version").get<int>() != 1) {
            status_message = "Settings rejected: unsupported schema or version";
            return false;
        }
        LabState candidate = state;
        BlackHoleSettings& scene = candidate.scene;
        auto const& camera = document.at("camera");
        candidate.orbit.distance_in_masses = camera.at("distance_M").get<double>();
        candidate.orbit.elevation_deg = camera.at("elevation_deg").get<double>();
        candidate.orbit.azimuth_deg = camera.at("azimuth_deg").get<double>();
        candidate.orbit.look_yaw_deg = camera.at("look_yaw_deg").get<double>();
        candidate.orbit.look_pitch_deg = camera.at("look_pitch_deg").get<double>();
        candidate.orbit.vertical_fov_deg = camera.at("vertical_fov_deg").get<double>();
        candidate.auto_orbit_deg_per_second = camera.at("auto_orbit_deg_per_second").get<float>();
        auto const& disk = document.at("disk");
        scene.disk_enabled = disk.at("enabled").get<bool>();
        scene.disk_outer_radius_in_masses = disk.at("outer_radius_M").get<float>();
        scene.disk_peak_temperature_k = disk.at("peak_temperature_K").get<float>();
        scene.disk_opacity = disk.at("opacity").get<float>();
        scene.disk_turbulence = disk.at("turbulence").get<float>();
        scene.limb_darkening = disk.at("limb_darkening").get<bool>();
        candidate.animate_disk = disk.at("animate").get<bool>();
        candidate.time_rate_in_masses_per_second = disk.at("time_rate_M_per_second").get<float>();
        auto const& relativity = document.at("relativity");
        scene.doppler = relativity.at("doppler").get<bool>();
        scene.gravitational_shift = relativity.at("gravitational_shift").get<bool>();
        scene.curvature = relativity.at("curvature").get<bool>();
        auto const& sky = document.at("sky");
        int const sky_mode = sky.at("mode").get<int>();
        if (sky_mode < 0 || sky_mode > static_cast<int>(BlackHoleSkyMode::Black)) {
            status_message = "Settings rejected: sky mode is out of range";
            return false;
        }
        scene.sky_mode = static_cast<BlackHoleSkyMode>(sky_mode);
        scene.star_intensity = sky.at("star_intensity").get<float>();
        scene.galaxy_intensity = sky.at("galaxy_intensity").get<float>();
        auto const& display = document.at("display");
        scene.exposure_ev = display.at("exposure_ev").get<float>();
        scene.glare = display.at("glare").get<float>();
        scene.tone_map = display.at("tone_map").get<bool>();
        scene.dither = display.at("dither").get<bool>();
        candidate.render_scale = display.at("render_scale").get<float>();
        candidate.v_sync = display.at("v_sync").get<bool>();
        auto const& quality = document.at("quality");
        candidate.quality_preset = std::clamp(quality.at("preset").get<int>(), 0, 2);
        scene.steps_per_half_turn = quality.at("steps_per_half_turn").get<int>();
        scene.max_image_orders = quality.at("max_image_orders").get<int>();
        scene.samples_per_pixel = quality.at("samples_per_pixel").get<int>();
        scene.critical_samples = quality.at("critical_samples").get<int>();
        scene.adaptive_sampling = quality.value("adaptive_sampling", true);
        scene.adaptive_contrast = quality.value("adaptive_contrast", 0.05f);

        if (!validState(candidate)) {
            status_message = "Settings rejected: one or more values are outside the UI contract";
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

[[nodiscard]] bool sliderDouble(char const* label, double& value, double minimum, double maximum,
                                char const* format,
                                ImGuiSliderFlags flags = ImGuiSliderFlags_None) {
    return ImGui::SliderScalar(label, ImGuiDataType_Double, &value, &minimum, &maximum, format,
                               flags);
}

struct ViewPreset {
    char const* label;
    double distance_in_masses;
    double elevation_deg;
    double vertical_fov_deg;
};

constexpr std::array<ViewPreset, 4> kViewPresets{{
    {"Grazing (6 deg)", 75.0, 6.0, 38.0},
    {"Inclined (25 deg)", 80.0, 25.0, 38.0},
    {"Face-on (80 deg)", 90.0, 80.0, 38.0},
    {"Photon ring close-up", 40.0, 12.0, 18.0},
}};

void applyViewPreset(LabState& state, ViewPreset const& preset) {
    state.orbit.distance_in_masses = preset.distance_in_masses;
    state.orbit.elevation_deg = preset.elevation_deg;
    state.orbit.vertical_fov_deg = preset.vertical_fov_deg;
    state.orbit.look_yaw_deg = 0.0;
    state.orbit.look_pitch_deg = 0.0;
}

void drawControlPanel(DiligentSdlApp& app, LabState& state, BlackHoleMetrics const& metrics,
                      RollingStats const& frame_ms, RollingStats const& trace_ms,
                      RollingStats const& post_ms, std::string& status_message) {
    float const scale = app.uiScale();
    ImGui::SetNextWindowPos(ImVec2{16.0f * scale, 16.0f * scale}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2{470.0f * scale, 780.0f * scale}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Helios v0.0.1 - Black Hole Lab (real time)")) {
        ImGui::End();
        return;
    }
    BlackHoleSettings& scene = state.scene;
    OrbitCamera& orbit = state.orbit;

    ImGui::TextUnformatted("Schwarzschild (a* = 0), Diligent Engine / Vulkan");
    ImGui::TextWrapped("Adapter: %s", app.adapterName().c_str());
    ImGui::Text("Validation diagnostics: %u warning(s), %u error(s)", app.warningCount(),
                app.errorCount());
    if (ImGui::Button("Save settings")) {
        (void)saveSettings(state, status_message);
    }
    ImGui::SameLine();
    if (ImGui::Button("Load settings")) {
        (void)loadSettings(state, status_message);
    }
    if (!status_message.empty()) {
        ImGui::TextColored(ImVec4{1.0f, 0.55f, 0.2f, 1.0f}, "%s", status_message.c_str());
    }

    if (ImGui::CollapsingHeader("Performance", ImGuiTreeNodeFlags_DefaultOpen)) {
        double const average_frame_ms = frame_ms.average();
        ImGui::Text("FPS: %.1f", average_frame_ms > 0.0 ? 1000.0 / average_frame_ms : 0.0);
        ImGui::Text("Frame: %.3f ms | p50 %.3f | p95 %.3f | p99 %.3f", frame_ms.latest(),
                    frame_ms.percentile(50.0), frame_ms.percentile(95.0),
                    frame_ms.percentile(99.0));
        if (metrics.gpu_timer_supported && !trace_ms.empty()) {
            ImGui::Text("GPU geodesic trace: %.3f ms | p95 %.3f", trace_ms.latest(),
                        trace_ms.percentile(95.0));
            ImGui::Text("GPU glare + composite: %.3f ms | p95 %.3f", post_ms.latest(),
                        post_ms.percentile(95.0));
        } else {
            ImGui::TextUnformatted("GPU duration query: unavailable / warming up");
        }
        ImGui::Text("Scene %ux%u -> window %ux%u", metrics.scene_width, metrics.scene_height,
                    app.width(), app.height());
        std::vector<float> const history = frame_ms.orderedFloatValues();
        if (!history.empty()) {
            ImGui::PlotLines("Frame time (ms)", history.data(), static_cast<int>(history.size()),
                             0, nullptr, 0.0f,
                             static_cast<float>(std::max(8.0, frame_ms.percentile(99.0) * 1.25)),
                             ImVec2{-1.0f, 56.0f * scale});
        }
        int preset = state.quality_preset;
        if (ImGui::Combo("Quality preset", &preset, kQualityPresetNames.data(),
                         static_cast<int>(kQualityPresetNames.size()))) {
            applyQualityPreset(state, preset);
        }
        ImGui::SliderInt("RK4 steps per half turn", &scene.steps_per_half_turn, 8, 128);
        ImGui::SliderInt("Disk image orders", &scene.max_image_orders, 1, 8);
        ImGui::SliderInt("Samples on edges", &scene.samples_per_pixel, 1, 16);
        ImGui::SliderInt("Extra photon-ring samples", &scene.critical_samples, 0, 32);
        ImGui::Checkbox("Adaptive sampling", &scene.adaptive_sampling);
        ImGui::SameLine();
        ImGui::SliderFloat("Contrast", &scene.adaptive_contrast, 0.005f, 0.25f, "%.3f",
                           ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Render scale", &state.render_scale, 0.25f, 2.0f, "%.2fx");
        ImGui::Checkbox("V-sync", &state.v_sync);
    }

    if (ImGui::CollapsingHeader("Camera (static observer)", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (std::size_t index = 0; index < kViewPresets.size(); ++index) {
            if (index > 0) {
                ImGui::SameLine();
            }
            if (ImGui::SmallButton(kViewPresets[index].label)) {
                applyViewPreset(state, kViewPresets[index]);
            }
        }
        (void)sliderDouble("Distance (M)", orbit.distance_in_masses,
                           BlackHoleRenderer::kMinimumCameraRadiusInMasses, 2000.0, "%.2f",
                           ImGuiSliderFlags_Logarithmic);
        (void)sliderDouble("Elevation above disk (deg)", orbit.elevation_deg, -89.5, 89.5, "%.2f");
        (void)sliderDouble("Azimuth (deg)", orbit.azimuth_deg, -180.0, 180.0, "%.2f");
        (void)sliderDouble("Look yaw (deg)", orbit.look_yaw_deg, -180.0, 180.0, "%.2f");
        (void)sliderDouble("Look pitch (deg)", orbit.look_pitch_deg, -89.0, 89.0, "%.2f");
        (void)sliderDouble("Vertical FOV (deg)", orbit.vertical_fov_deg, 5.0, 120.0, "%.1f");
        ImGui::SliderFloat("Auto orbit (deg/s)", &state.auto_orbit_deg_per_second, -30.0f, 30.0f,
                           "%.1f");
        double const lapse = std::sqrt(1.0 - 2.0 / orbit.distance_in_masses);
        double const shadow_sine = std::min(
            1.0, blackhole::criticalImpactParameter(1.0) / orbit.distance_in_masses * lapse);
        ImGui::Text("Clock rate vs infinity: %.4f | sky blueshift x%.4f", lapse, 1.0 / lapse);
        if (orbit.distance_in_masses >= 3.0) {
            ImGui::Text("Shadow angular diameter: %.2f deg",
                        2.0 * std::asin(shadow_sine) * kDegreesPerRadian);
        } else {
            ImGui::TextUnformatted("Inside the photon sphere: the sky shrinks to a cap overhead");
        }
        ImGui::TextWrapped("Drag with the left mouse button to orbit, right button to look "
                           "around, wheel to change distance. Space pauses the disk, R resets "
                           "the view, Esc quits. No mouse grab is used.");
    }

    if (ImGui::CollapsingHeader("Accretion disk", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Disk", &scene.disk_enabled);
        ImGui::SameLine();
        ImGui::Checkbox("Limb darkening", &scene.limb_darkening);
        ImGui::SliderFloat("Outer radius (M)", &scene.disk_outer_radius_in_masses, 8.0f, 200.0f,
                           "%.1f", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Peak temperature (K)", &scene.disk_peak_temperature_k, 1000.0f,
                           500000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Opacity", &scene.disk_opacity, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Turbulence (illustrative)", &scene.disk_turbulence, 0.0f, 0.95f,
                           "%.2f");
        ImGui::Checkbox("Animate", &state.animate_disk);
        ImGui::SliderFloat("Time rate (M/s)", &state.time_rate_in_masses_per_second, 0.0f, 200.0f,
                           "%.1f", ImGuiSliderFlags_Logarithmic);
        ImGui::TextWrapped("Inner edge fixed at the ISCO (6 M). Radial profile: Page-Thorne "
                           "zero-torque flux, peaking near 9.55 M. ISCO orbital period: 92.3 M. "
                           "Real disks peak in the UV/X-ray and would look uniformly blue-white; "
                           "lower the temperature to see the radial and Doppler structure.");
    }

    if (ImGui::CollapsingHeader("Relativistic effects", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Curved space-time (light bending)", &scene.curvature);
        ImGui::Checkbox("Doppler shift and beaming", &scene.doppler);
        ImGui::Checkbox("Gravitational redshift and time dilation", &scene.gravitational_shift);
        ImGui::TextWrapped("All three on is the physical image. Switching one off shows what "
                           "that effect contributes; it is not a physical configuration.");
    }

    if (ImGui::CollapsingHeader("Sky and display", ImGuiTreeNodeFlags_DefaultOpen)) {
        int sky_mode = static_cast<int>(scene.sky_mode);
        char const* sky_modes[] = {"Procedural stars", "Celestial grid", "Black"};
        if (ImGui::Combo("Sky", &sky_mode, sky_modes, 3)) {
            scene.sky_mode = static_cast<BlackHoleSkyMode>(sky_mode);
        }
        ImGui::SliderFloat("Star intensity", &scene.star_intensity, 0.0f, 16.0f, "%.2f",
                           ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Galactic band intensity", &scene.galaxy_intensity, 0.0f, 16.0f,
                           "%.2f", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Exposure (EV)", &scene.exposure_ev, -8.0f, 8.0f, "%.2f");
        ImGui::SliderFloat("Glare", &scene.glare, 0.0f, 0.5f, "%.3f");
        ImGui::Checkbox("ACES tone map", &scene.tone_map);
        ImGui::SameLine();
        ImGui::Checkbox("Dither", &scene.dither);
        int output = static_cast<int>(scene.output);
        char const* outputs[] = {"Radiance", "Geometry record", "Integration-step heat map"};
        if (ImGui::Combo("Output", &output, outputs, 3)) {
            scene.output = static_cast<BlackHoleOutput>(output);
        }
    }

    if (ImGui::CollapsingHeader("Model limits")) {
        ImGui::TextWrapped(
            "Geodesics are exact Schwarzschild null geodesics integrated in float32; run "
            "--validate for the measured agreement with the binary64 CPU reference. Not "
            "modelled: spin (Kerr), disk thickness, self-irradiation, radiative transfer, "
            "polarisation, light travel-time delay, and a real star catalogue. The camera is a "
            "static observer, so there is no orbital aberration. Turbulence and stars are "
            "procedural presentation content.");
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

int runInteractive(Options const& options) {
    DiligentSdlCreateInfo create_info;
    create_info.title = "Helios v0.0.1 - Vulkan Black Hole Lab";
    create_info.enable_validation = options.validation_layers.value_or(true);
    create_info.hidden = options.smoke_frames > 0;
    std::string error;
    auto app = DiligentSdlApp::create(create_info, error);
    if (app == nullptr) {
        std::fprintf(stderr, "Black-hole lab initialization failed: %s\n", error.c_str());
        return 1;
    }

    BlackHoleRenderer renderer;
    BlackHoleRenderer::CreateInfo renderer_info;
    renderer_info.output_format = app->swapChain()->GetDesc().ColorBufferFormat;
    if (!renderer.initialize(app->device(), app->context(), renderer_info, error)) {
        std::fprintf(stderr, "Black-hole renderer initialization failed: %s\n", error.c_str());
        return 1;
    }

    LabState state = options.state;
    LabState const initial_state = state;
    RollingStats frame_ms{360};
    RollingStats trace_ms{360};
    RollingStats post_ms{360};
    BlackHoleMetrics metrics;
    std::string status_message;

    bool running = true;
    bool orbiting = false;
    bool looking = false;
    int frame_count = 0;
    Clock::time_point previous_frame = Clock::now();
    while (running) {
        Clock::time_point const frame_begin = Clock::now();
        double const dt_seconds = std::clamp(
            std::chrono::duration<double>{frame_begin - previous_frame}.count(), 0.0, 0.1);
        previous_frame = frame_begin;

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            app->forwardEvent(event);
            switch (event.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                running = false;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (event.key.repeat || app->wantsKeyboardCapture()) {
                    break;
                }
                if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
                    running = false;
                } else if (event.key.scancode == SDL_SCANCODE_SPACE) {
                    state.animate_disk = !state.animate_disk;
                } else if (event.key.scancode == SDL_SCANCODE_R) {
                    state.orbit = initial_state.orbit;
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (!app->wantsMouseCapture()) {
                    orbiting = orbiting || event.button.button == SDL_BUTTON_LEFT;
                    looking = looking || event.button.button == SDL_BUTTON_RIGHT;
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (event.button.button == SDL_BUTTON_LEFT) {
                    orbiting = false;
                } else if (event.button.button == SDL_BUTTON_RIGHT) {
                    looking = false;
                }
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                orbiting = false;
                looking = false;
                break;
            case SDL_EVENT_MOUSE_MOTION: {
                // Degrees per pixel scale with the field of view so a drag
                // moves the image by about the distance the cursor travels.
                double const degrees_per_pixel =
                    state.orbit.vertical_fov_deg / std::max(1.0, static_cast<double>(app->height()));
                if (orbiting) {
                    state.orbit.azimuth_deg -= event.motion.xrel * degrees_per_pixel * 3.0;
                    state.orbit.elevation_deg += event.motion.yrel * degrees_per_pixel * 3.0;
                } else if (looking) {
                    state.orbit.look_yaw_deg -= event.motion.xrel * degrees_per_pixel;
                    state.orbit.look_pitch_deg += event.motion.yrel * degrees_per_pixel;
                }
                break;
            }
            case SDL_EVENT_MOUSE_WHEEL:
                if (!app->wantsMouseCapture()) {
                    state.orbit.distance_in_masses *= std::exp(-0.12 * event.wheel.y);
                }
                break;
            default:
                break;
            }
        }
        if (!running) {
            break;
        }
        if (!app->synchronizeDrawableSize()) {
            SDL_Delay(16);
            continue;
        }

        state.orbit.azimuth_deg += static_cast<double>(state.auto_orbit_deg_per_second) * dt_seconds;
        state.orbit.azimuth_deg = std::remainder(state.orbit.azimuth_deg, 360.0);
        state.orbit.elevation_deg = std::clamp(state.orbit.elevation_deg, -89.5, 89.5);
        state.orbit.look_yaw_deg = std::clamp(state.orbit.look_yaw_deg, -180.0, 180.0);
        state.orbit.look_pitch_deg = std::clamp(state.orbit.look_pitch_deg, -89.0, 89.0);
        state.orbit.distance_in_masses = std::clamp(
            state.orbit.distance_in_masses, BlackHoleRenderer::kMinimumCameraRadiusInMasses, 2000.0);
        if (state.animate_disk) {
            state.scene.disk_time_in_masses = std::fmod(
                state.scene.disk_time_in_masses +
                    static_cast<double>(state.time_rate_in_masses_per_second) * dt_seconds,
                kDiskTimeWrapInMasses);
        }
        state.scene.camera = makeCamera(state.orbit);

        app->beginImGuiFrame();
        float const scale = std::clamp(state.render_scale, 0.25f, 2.0f);
        std::uint32_t const scene_width = std::max(
            1U, static_cast<std::uint32_t>(std::lround(static_cast<float>(app->width()) * scale)));
        std::uint32_t const scene_height = std::max(
            1U, static_cast<std::uint32_t>(std::lround(static_cast<float>(app->height()) * scale)));

        Diligent::ITextureView* back_buffer = app->swapChain()->GetCurrentBackBufferRTV();
        if (!renderer.render(state.scene, scene_width, scene_height, back_buffer, app->width(),
                             app->height(), metrics, error)) {
            std::fprintf(stderr, "Black-hole render failed: %s\n", error.c_str());
            return 1;
        }
        if (metrics.gpu_sample_new) {
            trace_ms.push(metrics.gpu_trace_ms);
            post_ms.push(metrics.gpu_post_ms);
        }

        // The ImGui pipeline was created for the swap chain's colour + depth pair.
        app->context()->SetRenderTargets(1, &back_buffer, app->swapChain()->GetDepthBufferDSV(),
                                         Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        drawControlPanel(*app, state, metrics, frame_ms, trace_ms, post_ms, status_message);
        app->renderImGui();
        app->present(state.v_sync);
        frame_ms.push(milliseconds(frame_begin, Clock::now()));

        ++frame_count;
        if (options.smoke_frames > 0 && frame_count >= options.smoke_frames) {
            running = false;
        }
    }

    if (options.smoke_frames > 0) {
        std::printf("BlackHoleLab smoke: %d hidden-window frame(s), frame p50 %.3f ms, GPU trace "
                    "p50 %.3f ms, %u warning(s), %u error(s)\n",
                    frame_count, frame_ms.percentile(50.0), trace_ms.percentile(50.0),
                    app->warningCount(), app->errorCount());
    }
    return app->errorCount() == 0 ? 0 : 2;
}

// ---------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------

void printHelp() {
    std::printf(
        "Helios BlackHoleLab (real time, Diligent/Vulkan)\n\n"
        "Usage: helios_black_hole_realtime [route] [options]\n\n"
        "Routes (default: interactive window):\n"
        "  --capture PATH.png   Windowless render to a PNG\n"
        "  --benchmark          Windowless GPU timing run\n"
        "  --validate           Windowless GPU-versus-CPU-reference geodesic comparison\n"
        "  --smoke-frames N     Interactive route in a hidden window for N frames\n\n"
        "Image:\n"
        "  --width N --height N Output size, default 1920x1080\n"
        "  --supersample N      Trace N*N samples per output pixel (1-4), default 1\n"
        "  --frames N           Benchmark frame count, default 240\n"
        "  --report PATH.json   Write a JSON report\n"
        "  --validation on|off  Vulkan validation layers (default on, off for --benchmark)\n\n"
        "Camera (static observer):\n"
        "  --distance M         Radius in M, default 75\n"
        "  --elevation DEG      Elevation above the disk plane, default 6\n"
        "  --azimuth DEG        Azimuth, default -90\n"
        "  --fov DEG            Vertical field of view, default 38\n"
        "  --look-yaw DEG --look-pitch DEG   Look away from the centre\n\n"
        "Disk:\n"
        "  --temperature K      Peak effective temperature, default 5200\n"
        "  --outer-radius M     Outer radius, default 26\n"
        "  --opacity X          0 disables the disk, default 1\n"
        "  --turbulence X       Illustrative emissivity structure, 0-0.95, default 0.6\n"
        "  --time M             Disk clock, default 0\n"
        "  --no-limb-darkening\n\n"
        "Effects and sky:\n"
        "  --no-doppler --no-gravity --flat   Switch off individual relativistic terms\n"
        "  --sky stars|grid|black\n"
        "  --stars X --galaxy X               Sky intensities, default 1\n"
        "  --exposure EV --glare X            Display transform, defaults 0 and 0.08\n\n"
        "Quality:\n"
        "  --preset performance|balanced|ultra\n"
        "  --steps N --orders N --spp N --critical N\n"
        "  --uniform-sampling   Take --spp samples in every pixel instead of only where needed\n"
        "  --contrast X         Adaptive-sampling contrast threshold, default 0.05\n"
        "  --heatmap            Output the integration-step heat map\n");
}

template <typename T> [[nodiscard]] std::optional<T> parseNumber(std::string_view text) {
    T value{};
    auto const [end, code] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (code != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] bool parseOptions(int argc, char** argv, Options& options, std::string& error) {
    applyQualityPreset(options.state, options.state.quality_preset);
    LabState& state = options.state;
    BlackHoleSettings& scene = state.scene;

    int index = 1;
    auto value = [&](std::string_view option) -> std::optional<std::string_view> {
        if (index + 1 >= argc) {
            error = "missing value for " + std::string{option};
            return std::nullopt;
        }
        return std::string_view{argv[++index]};
    };
    auto number = [&](std::string_view option, double minimum, double maximum,
                      auto& target) -> bool {
        auto const text = value(option);
        if (!text) {
            return false;
        }
        auto const parsed = parseNumber<double>(*text);
        if (!parsed || !std::isfinite(*parsed) || *parsed < minimum || *parsed > maximum) {
            error = std::string{option} + " must be in [" + std::to_string(minimum) + ", " +
                    std::to_string(maximum) + "]";
            return false;
        }
        target = static_cast<std::remove_reference_t<decltype(target)>>(*parsed);
        return true;
    };

    for (; index < argc; ++index) {
        std::string_view const argument = argv[index];
        bool ok = true;
        if (argument == "--help" || argument == "-h") {
            options.show_help = true;
        } else if (argument == "--capture") {
            auto const path = value(argument);
            ok = path.has_value() && !path->empty();
            if (ok) {
                options.mode = Mode::Capture;
                options.capture_path = std::string{*path};
            } else if (error.empty()) {
                error = "--capture needs an output path";
            }
        } else if (argument == "--benchmark") {
            options.mode = Mode::Benchmark;
        } else if (argument == "--validate") {
            options.mode = Mode::Validate;
        } else if (argument == "--smoke-frames") {
            ok = number(argument, 1.0, 100000.0, options.smoke_frames);
        } else if (argument == "--report") {
            auto const path = value(argument);
            ok = path.has_value();
            if (ok) {
                options.report_path = std::string{*path};
            }
        } else if (argument == "--validation") {
            auto const text = value(argument);
            ok = text.has_value() && (*text == "on" || *text == "off");
            if (ok) {
                options.validation_layers = *text == "on";
            } else if (error.empty()) {
                error = "--validation must be on or off";
            }
        } else if (argument == "--width") {
            ok = number(argument, 16.0, 8192.0, options.width);
        } else if (argument == "--height") {
            ok = number(argument, 16.0, 8192.0, options.height);
        } else if (argument == "--supersample") {
            ok = number(argument, 1.0, 4.0, options.supersample);
        } else if (argument == "--frames") {
            ok = number(argument, 1.0, 100000.0, options.frames);
        } else if (argument == "--distance") {
            ok = number(argument, BlackHoleRenderer::kMinimumCameraRadiusInMasses, 5000.0,
                        state.orbit.distance_in_masses);
        } else if (argument == "--elevation") {
            ok = number(argument, -89.5, 89.5, state.orbit.elevation_deg);
        } else if (argument == "--azimuth") {
            ok = number(argument, -720.0, 720.0, state.orbit.azimuth_deg);
        } else if (argument == "--fov") {
            ok = number(argument, 5.0, 120.0, state.orbit.vertical_fov_deg);
        } else if (argument == "--look-yaw") {
            ok = number(argument, -180.0, 180.0, state.orbit.look_yaw_deg);
        } else if (argument == "--look-pitch") {
            ok = number(argument, -89.0, 89.0, state.orbit.look_pitch_deg);
        } else if (argument == "--temperature") {
            ok = number(argument, 1000.0, 500000.0, scene.disk_peak_temperature_k);
        } else if (argument == "--outer-radius") {
            ok = number(argument, 8.0, 200.0, scene.disk_outer_radius_in_masses);
        } else if (argument == "--opacity") {
            ok = number(argument, 0.0, 1.0, scene.disk_opacity);
            scene.disk_enabled = scene.disk_opacity > 0.0f;
        } else if (argument == "--turbulence") {
            ok = number(argument, 0.0, 0.95, scene.disk_turbulence);
        } else if (argument == "--time") {
            ok = number(argument, 0.0, kDiskTimeWrapInMasses, scene.disk_time_in_masses);
        } else if (argument == "--no-limb-darkening") {
            scene.limb_darkening = false;
        } else if (argument == "--no-doppler") {
            scene.doppler = false;
        } else if (argument == "--no-gravity") {
            scene.gravitational_shift = false;
        } else if (argument == "--flat") {
            scene.curvature = false;
        } else if (argument == "--sky") {
            auto const text = value(argument);
            ok = text.has_value();
            if (ok && *text == "stars") {
                scene.sky_mode = BlackHoleSkyMode::Stars;
            } else if (ok && *text == "grid") {
                scene.sky_mode = BlackHoleSkyMode::CelestialGrid;
            } else if (ok && *text == "black") {
                scene.sky_mode = BlackHoleSkyMode::Black;
            } else {
                ok = false;
                if (error.empty()) {
                    error = "--sky must be stars, grid or black";
                }
            }
        } else if (argument == "--stars") {
            ok = number(argument, 0.0, 16.0, scene.star_intensity);
        } else if (argument == "--galaxy") {
            ok = number(argument, 0.0, 16.0, scene.galaxy_intensity);
        } else if (argument == "--exposure") {
            ok = number(argument, -8.0, 8.0, scene.exposure_ev);
        } else if (argument == "--glare") {
            ok = number(argument, 0.0, 1.0, scene.glare);
        } else if (argument == "--preset") {
            auto const text = value(argument);
            ok = text.has_value();
            if (ok && *text == "performance") {
                applyQualityPreset(state, 0);
            } else if (ok && *text == "balanced") {
                applyQualityPreset(state, 1);
            } else if (ok && *text == "ultra") {
                applyQualityPreset(state, 2);
            } else {
                ok = false;
                if (error.empty()) {
                    error = "--preset must be performance, balanced or ultra";
                }
            }
        } else if (argument == "--steps") {
            ok = number(argument, 8.0, 128.0, scene.steps_per_half_turn);
        } else if (argument == "--orders") {
            ok = number(argument, 1.0, 8.0, scene.max_image_orders);
        } else if (argument == "--spp") {
            ok = number(argument, 1.0, 16.0, scene.samples_per_pixel);
        } else if (argument == "--critical") {
            ok = number(argument, 0.0, 32.0, scene.critical_samples);
        } else if (argument == "--uniform-sampling") {
            scene.adaptive_sampling = false;
        } else if (argument == "--contrast") {
            ok = number(argument, 0.001, 1.0, scene.adaptive_contrast);
        } else if (argument == "--heatmap") {
            scene.output = BlackHoleOutput::StepHeatMap;
        } else {
            error = "unknown option: " + std::string{argument};
            ok = false;
        }
        if (!ok) {
            return false;
        }
    }
    return true;
}

} // namespace
} // namespace helios::examples

int main(int argc, char** argv) {
    using namespace helios::examples;

    Options options;
    std::string error;
    if (!parseOptions(argc, argv, options, error)) {
        std::fprintf(stderr, "BlackHoleLab: %s\n", error.c_str());
        return 2;
    }
    if (options.show_help) {
        printHelp();
        return 0;
    }
    switch (options.mode) {
    case Mode::Capture:
        return runCapture(options);
    case Mode::Benchmark:
        return runBenchmark(options);
    case Mode::Validate:
        return runValidation(options);
    case Mode::Interactive:
        break;
    }
    return runInteractive(options);
}
