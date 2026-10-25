// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// BlackHoleLab CPU reference image generator.
//
// Geometry is traced by helios_blackhole's adaptive binary64 Schwarzschild
// null-geodesic integrator.  The disk is a multi-temperature black body with
// the Page-Thorne zero-torque radial profile; its observed temperature uses
// the exact frequency ratio for circular-geodesic gas, computed from the
// photon's conserved energy and axial angular momentum.  The real-time lab
// (realtime_main.cpp) implements the same model in float32 on the GPU and
// reads the same black-body table.
// It is not GRMHD, a spectral radiative-transfer solver, or a Kerr renderer.

#include "helios/blackhole/blackbody.hpp"
#include "helios/blackhole/ray_tracer.hpp"
#include "helios/blackhole/schwarzschild.hpp"
#include "helios/blackhole/thin_disk.hpp"
#include "helios/math/vec.hpp"
#include "helios/version.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using helios::blackhole::RayInitialCondition;
using helios::blackhole::RayIntegrationSettings;
using helios::blackhole::RayTermination;
using helios::blackhole::RayTraceResult;
using helios::blackhole::traceSchwarzschildRay;
using helios::math::Vec3d;

namespace {

#ifndef HELIOS_BUILD_TYPE
#define HELIOS_BUILD_TYPE "unknown"
#endif

constexpr double kPi = 3.14159265358979323846264338327950288;

struct Options final {
    std::uint32_t width = 320;
    std::uint32_t height = 180;
    std::uint32_t samplesPerAxis = 1;
    std::uint32_t threads = std::max(1u, std::thread::hardware_concurrency());
    double cameraDistanceInMasses = 20.0;
    double cameraElevationDegrees = 20.0;
    double verticalFovDegrees = 50.0;
    double diskPeakTemperatureKelvin = 5200.0;
    fs::path outputDirectory = "black_hole_lab_output";
    bool writeRawRgba = true;
    bool showHelp = false;
};

struct Color final {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;

    Color& operator+=(Color const& other) noexcept {
        r += other.r;
        g += other.g;
        b += other.b;
        return *this;
    }
};

[[nodiscard]] Color operator*(Color color, double scale) noexcept {
    return Color{color.r * scale, color.g * scale, color.b * scale};
}

struct Camera final {
    Vec3d position{};
    Vec3d forward{};
    Vec3d right{};
    Vec3d up{};
    double tangentHalfFov = 0.0;
    double aspect = 1.0;
};

struct Metrics final {
    std::uint64_t horizon = 0;
    std::uint64_t escaped = 0;
    std::uint64_t disk = 0;
    std::uint64_t maxSteps = 0;
    std::uint64_t invalid = 0;
    std::uint64_t nonFiniteDiagnostics = 0;
    std::uint64_t acceptedSteps = 0;
    std::uint64_t rejectedSteps = 0;
    std::uint32_t maximumAcceptedSteps = 0;
    double maximumAbsoluteNullResidual = 0.0;
    double maximumRelativeNullResidual = 0.0;
    double minimumClosestRadius = std::numeric_limits<double>::infinity();

    void add(RayTraceResult const& result) noexcept {
        switch (result.termination) {
        case RayTermination::Horizon:
            ++horizon;
            break;
        case RayTermination::Escaped:
            ++escaped;
            break;
        case RayTermination::AccretionDisk:
            ++disk;
            break;
        case RayTermination::MaxSteps:
            ++maxSteps;
            break;
        case RayTermination::InvalidInput:
            ++invalid;
            break;
        }

        auto const& diagnostics = result.diagnostics;
        acceptedSteps += diagnostics.acceptedSteps;
        rejectedSteps += diagnostics.rejectedSteps;
        maximumAcceptedSteps = std::max(maximumAcceptedSteps, diagnostics.acceptedSteps);
        maximumAbsoluteNullResidual =
            std::max(maximumAbsoluteNullResidual, diagnostics.maxAbsoluteNullResidual);
        maximumRelativeNullResidual =
            std::max(maximumRelativeNullResidual, diagnostics.maxRelativeNullResidual);
        minimumClosestRadius = std::min(minimumClosestRadius, diagnostics.closestRadius);
        if (!std::isfinite(diagnostics.maxAbsoluteNullResidual) ||
            !std::isfinite(diagnostics.maxRelativeNullResidual) ||
            !std::isfinite(diagnostics.closestRadius)) {
            ++nonFiniteDiagnostics;
        }
    }

    void merge(Metrics const& other) noexcept {
        horizon += other.horizon;
        escaped += other.escaped;
        disk += other.disk;
        maxSteps += other.maxSteps;
        invalid += other.invalid;
        nonFiniteDiagnostics += other.nonFiniteDiagnostics;
        acceptedSteps += other.acceptedSteps;
        rejectedSteps += other.rejectedSteps;
        maximumAcceptedSteps = std::max(maximumAcceptedSteps, other.maximumAcceptedSteps);
        maximumAbsoluteNullResidual =
            std::max(maximumAbsoluteNullResidual, other.maximumAbsoluteNullResidual);
        maximumRelativeNullResidual =
            std::max(maximumRelativeNullResidual, other.maximumRelativeNullResidual);
        minimumClosestRadius = std::min(minimumClosestRadius, other.minimumClosestRadius);
    }

    [[nodiscard]] std::uint64_t rayCount() const noexcept {
        return horizon + escaped + disk + maxSteps + invalid;
    }
};

struct RenderResult final {
    std::vector<std::uint8_t> rgba{};
    Metrics metrics{};
    double renderSeconds = 0.0;
};

[[nodiscard]] std::uint64_t mix64(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] double unitFromBits(std::uint64_t value) noexcept {
    return static_cast<double>(value >> 11U) * (1.0 / 9007199254740992.0);
}

// Display scale shared with the real-time lab: an unshifted black body at
// the disk's peak temperature sits about two stops above display white
// before the tone map, so the redshifted bulk of the disk stays visible.
constexpr double kBaseExposure = 3.4;

// The sky is test content with no physical brightness relation to the disk;
// this keeps its display level where the pre-radiometric lab had it.
constexpr double kSkyScale = 0.34;

// Black-body colours and the luminance that maps to 1.  The table is the one
// the real-time shader samples (helios_blackhole/blackbody).
struct Radiometry final {
    helios::blackhole::BlackbodyTable table{};
    double peakTemperatureKelvin = 0.0;
    double logReferenceLuminance = 0.0;
};

[[nodiscard]] Radiometry makeRadiometry(double peakTemperatureKelvin) {
    Radiometry radiometry{};
    radiometry.table = helios::blackhole::makeBlackbodyTable(500.0, 1.0e6, 512);
    radiometry.peakTemperatureKelvin = peakTemperatureKelvin;
    radiometry.logReferenceLuminance =
        helios::blackhole::sampleBlackbodyLogLuminance(radiometry.table, peakTemperatureKelvin);
    return radiometry;
}

// Chromaticity of a black body, scaled to unit luminance.
[[nodiscard]] Color blackBodyChromaticity(Radiometry const& radiometry, double kelvin) noexcept {
    const Vec3d rgb = helios::blackhole::sampleBlackbodyTable(
        radiometry.table, kelvin,
        helios::blackhole::sampleBlackbodyLogLuminance(radiometry.table, kelvin));
    return Color{rgb.v[0], rgb.v[1], rgb.v[2]};
}

[[nodiscard]] Color starLayer(Radiometry const& radiometry, Vec3d const& direction,
                              std::uint32_t longitudeCells, std::uint32_t latitudeCells,
                              double radiusInCell, std::uint64_t seed) noexcept {
    double longitude = std::atan2(direction.v[1], direction.v[0]) / (2.0 * kPi) + 0.5;
    longitude -= std::floor(longitude);
    const double latitude = std::asin(std::clamp(direction.v[2], -1.0, 1.0)) / kPi + 0.5;

    const double gridX = longitude * static_cast<double>(longitudeCells);
    const double gridY = std::clamp(latitude, 0.0, std::nextafter(1.0, 0.0)) *
                         static_cast<double>(latitudeCells);
    const auto cellX = static_cast<std::uint32_t>(std::floor(gridX));
    const auto cellY = static_cast<std::uint32_t>(std::floor(gridY));
    const std::uint64_t cellKey = (static_cast<std::uint64_t>(cellY) << 32U) | cellX;
    const std::uint64_t hash = mix64(cellKey ^ seed);

    // Some cells are intentionally empty, providing deterministic variation
    // without a stored sky catalogue.
    if ((hash & 0xffffU) > 0x9000U) {
        return {};
    }

    const double starX = unitFromBits(mix64(hash ^ 0x3c79ac492ba7b653ULL));
    const double starY = unitFromBits(mix64(hash ^ 0x1c69b3f74ac4ae35ULL));
    const double dx = (gridX - std::floor(gridX)) - starX;
    const double dy = (gridY - std::floor(gridY)) - starY;
    const double distance = std::sqrt(dx * dx + dy * dy);
    if (distance >= radiusInCell) {
        return {};
    }

    const double core = 1.0 - distance / radiusInCell;
    const double luminosity = 0.6 + 5.5 * std::pow(unitFromBits(mix64(hash ^ 0xd1b54a32d192ed03ULL)), 5.0);
    const double temperature = 2800.0 + 11000.0 * unitFromBits(mix64(hash ^ 0x94d049bb133111ebULL));
    return blackBodyChromaticity(radiometry, temperature) *
           (kSkyScale * luminosity * core * core * core);
}

[[nodiscard]] Color proceduralSky(Radiometry const& radiometry, Vec3d direction) noexcept {
    direction = helios::math::normalized(direction);
    const Vec3d galacticNormal = helios::math::normalized(Vec3d{0.22, 0.91, 0.35});
    const double bandDistance = std::abs(helios::math::dot(direction, galacticNormal));
    const double band = std::exp(-bandDistance * bandDistance * 85.0);

    Color result = Color{0.0007 + 0.0030 * band, 0.0009 + 0.0024 * band,
                         0.0016 + 0.0042 * band} *
                   kSkyScale;
    result += starLayer(radiometry, direction, 512, 256, 0.035, 0x6a09e667f3bcc909ULL);
    result += starLayer(radiometry, direction, 1024, 512, 0.024, 0xbb67ae8584caa73bULL);
    return result;
}

[[nodiscard]] Color shadeDisk(RayTraceResult const& result, Camera const& camera,
                              Vec3d const& direction, RayIntegrationSettings const& settings,
                              Radiometry const& radiometry) noexcept {
    const double mass = settings.geometry.mass;
    const double radiusInMasses = result.diskHit.radius / mass;
    const Vec3d diskNormal = helios::math::normalized(settings.disk.normal);

    // b = L/E and lambda = L_z/E are conserved along the geodesic, so the
    // values at the camera are the values at the emission point.  The photon
    // that reaches the camera travels opposite to the traced direction.
    const double cameraRadius = helios::math::length(camera.position);
    const double lapse = helios::blackhole::staticClockRateAtInfinity(mass, cameraRadius);
    const double axialImpactInMasses =
        -helios::math::dot(helios::math::cross(camera.position, direction), diskNormal) /
        (lapse * mass);
    const helios::blackhole::DiskPhotonShift shift = helios::blackhole::thinDiskPhotonShift(
        cameraRadius / mass, radiusInMasses, result.diagnostics.impactParameter / mass,
        axialImpactInMasses);
    if (!std::isfinite(shift.frequencyRatio)) {
        return Color{1.0, 0.0, 1.0};
    }

    // A black body at T is observed as a black body at g * T (I_nu / nu^3 is
    // invariant), which carries the Doppler boost, gravitational redshift and
    // time dilation in both colour and brightness.
    const double emittedTemperature =
        radiometry.peakTemperatureKelvin * helios::blackhole::diskTemperatureRatio(radiusInMasses);
    const Vec3d radiance = helios::blackhole::sampleBlackbodyTable(
        radiometry.table, shift.frequencyRatio * emittedTemperature,
        radiometry.logReferenceLuminance);
    const double limb = helios::blackhole::limbDarkening(shift.cosEmissionAngle);
    return Color{radiance.v[0] * limb, radiance.v[1] * limb, radiance.v[2] * limb};
}

[[nodiscard]] Color shadeRay(RayTraceResult const& result, Camera const& camera,
                             Vec3d const& direction, RayIntegrationSettings const& settings,
                             Radiometry const& radiometry) noexcept {
    switch (result.termination) {
    case RayTermination::Horizon:
        return Color{};
    case RayTermination::AccretionDisk:
        return shadeDisk(result, camera, direction, settings, radiometry);
    case RayTermination::Escaped:
        return proceduralSky(radiometry,
                             helios::math::normalized(result.terminalSample.position));
    case RayTermination::MaxSteps:
        return Color{0.45, 0.02, 0.35};
    case RayTermination::InvalidInput:
        return Color{1.0, 0.0, 1.0};
    }
    return Color{1.0, 0.0, 1.0};
}

// The display transform of the real-time lab (shaders/black_hole_composite.slang),
// without its dither: Stephen Hill's fit of the ACES reference rendering and
// output transforms, then sRGB encoding.  Sharing it keeps the reference image
// directly comparable to a real-time capture.
[[nodiscard]] std::array<std::uint8_t, 3> encodeColor(Color const& linear) noexcept {
    const double r = std::max(linear.r * kBaseExposure, 0.0);
    const double g = std::max(linear.g * kBaseExposure, 0.0);
    const double b = std::max(linear.b * kBaseExposure, 0.0);
    const std::array<double, 3> input{0.59719 * r + 0.35458 * g + 0.04823 * b,
                                      0.07600 * r + 0.90834 * g + 0.01566 * b,
                                      0.02840 * r + 0.13383 * g + 0.83777 * b};
    std::array<double, 3> fitted{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const double v = input[channel];
        fitted[channel] = (v * (v + 0.0245786) - 0.000090537) /
                          (v * (0.983729 * v + 0.4329510) + 0.238081);
    }
    const std::array<double, 3> mapped{
        1.60475 * fitted[0] - 0.53108 * fitted[1] - 0.07367 * fitted[2],
        -0.10208 * fitted[0] + 1.10813 * fitted[1] - 0.00605 * fitted[2],
        -0.00327 * fitted[0] - 0.07276 * fitted[1] + 1.07602 * fitted[2]};
    std::array<std::uint8_t, 3> encoded{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const double value = std::clamp(mapped[channel], 0.0, 1.0);
        const double srgb = value <= 0.0031308 ? 12.92 * value
                                               : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
        encoded[channel] =
            static_cast<std::uint8_t>(std::clamp(std::lround(srgb * 255.0), 0L, 255L));
    }
    return encoded;
}

[[nodiscard]] Camera makeCamera(Options const& options) {
    const double elevation = options.cameraElevationDegrees * kPi / 180.0;
    Camera camera{};
    camera.position = Vec3d{0.0, -options.cameraDistanceInMasses * std::cos(elevation),
                            options.cameraDistanceInMasses * std::sin(elevation)};
    camera.forward = helios::math::normalized(camera.position * -1.0);
    camera.right = helios::math::normalized(helios::math::cross(camera.forward, Vec3d{0.0, 0.0, 1.0}));
    camera.up = helios::math::normalized(helios::math::cross(camera.right, camera.forward));
    camera.tangentHalfFov = std::tan(options.verticalFovDegrees * kPi / 360.0);
    camera.aspect = static_cast<double>(options.width) / static_cast<double>(options.height);
    return camera;
}

[[nodiscard]] Vec3d rayDirection(Camera const& camera, Options const& options,
                                 std::uint32_t pixelX, std::uint32_t pixelY,
                                 std::uint32_t sampleX, std::uint32_t sampleY) noexcept {
    const double subpixelX = (static_cast<double>(sampleX) + 0.5) /
                             static_cast<double>(options.samplesPerAxis);
    const double subpixelY = (static_cast<double>(sampleY) + 0.5) /
                             static_cast<double>(options.samplesPerAxis);
    const double ndcX =
        2.0 * (static_cast<double>(pixelX) + subpixelX) / static_cast<double>(options.width) - 1.0;
    const double ndcY =
        1.0 - 2.0 * (static_cast<double>(pixelY) + subpixelY) / static_cast<double>(options.height);
    return helios::math::normalized(camera.forward +
                                    camera.right * (ndcX * camera.aspect * camera.tangentHalfFov) +
                                    camera.up * (ndcY * camera.tangentHalfFov));
}

[[nodiscard]] RayIntegrationSettings makeSettings() {
    RayIntegrationSettings settings{};
    settings.geometry.mass = 1.0;
    settings.disk.enabled = true;
    settings.disk.innerRadiusInMasses = 6.0;
    settings.disk.outerRadiusInMasses = 30.0;
    settings.disk.normal = Vec3d{0.0, 0.0, 1.0};
    settings.escapeRadiusInMasses = 100.0;
    settings.initialStepInMasses = 0.06;
    settings.minStepInMasses = 1.0e-7;
    settings.maxStepInMasses = 0.5;
    settings.absoluteTolerance = 1.0e-10;
    settings.relativeTolerance = 1.0e-9;
    settings.maxRadialFractionPerStep = 0.025;
    settings.maxOrbitalAnglePerStep = 0.02;
    settings.maxSteps = 120000;
    settings.maxAffineLengthInMasses = 600.0;
    settings.recordTrajectory = false;
    return settings;
}

[[nodiscard]] RenderResult render(Options const& options, Camera const& camera,
                                  RayIntegrationSettings const& settings,
                                  Radiometry const& radiometry) {
    RenderResult output{};
    output.rgba.resize(static_cast<std::size_t>(options.width) * options.height * 4U);

    const std::uint32_t workerCount = std::max(1u, std::min(options.threads, options.height));
    std::vector<Metrics> workerMetrics(workerCount);
    std::atomic<std::uint32_t> nextRow{0};
    std::vector<std::thread> workers{};
    workers.reserve(workerCount);
    const auto start = std::chrono::steady_clock::now();

    for (std::uint32_t workerIndex = 0; workerIndex < workerCount; ++workerIndex) {
        workers.emplace_back([&, workerIndex] {
            Metrics& metrics = workerMetrics[workerIndex];
            for (;;) {
                const std::uint32_t y = nextRow.fetch_add(1, std::memory_order_relaxed);
                if (y >= options.height) {
                    break;
                }
                for (std::uint32_t x = 0; x < options.width; ++x) {
                    Color accumulated{};
                    for (std::uint32_t sy = 0; sy < options.samplesPerAxis; ++sy) {
                        for (std::uint32_t sx = 0; sx < options.samplesPerAxis; ++sx) {
                            const Vec3d direction = rayDirection(camera, options, x, y, sx, sy);
                            const RayTraceResult result = traceSchwarzschildRay(
                                RayInitialCondition{camera.position, direction}, settings);
                            metrics.add(result);
                            accumulated +=
                                shadeRay(result, camera, direction, settings, radiometry);
                        }
                    }
                    const double sampleCount = static_cast<double>(options.samplesPerAxis) *
                                               static_cast<double>(options.samplesPerAxis);
                    const Color averaged = accumulated * (1.0 / sampleCount);
                    const std::size_t offset =
                        (static_cast<std::size_t>(y) * options.width + x) * 4U;
                    const std::array<std::uint8_t, 3> encoded = encodeColor(averaged);
                    output.rgba[offset + 0] = encoded[0];
                    output.rgba[offset + 1] = encoded[1];
                    output.rgba[offset + 2] = encoded[2];
                    output.rgba[offset + 3] = 255U;
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    output.renderSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    for (Metrics const& metrics : workerMetrics) {
        output.metrics.merge(metrics);
    }
    return output;
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
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << value;
    return stream.str();
}

[[nodiscard]] bool writePpm(fs::path const& path, std::uint32_t width,
                            std::uint32_t height, std::vector<std::uint8_t> const& rgba) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file << "P6\n" << width << ' ' << height << "\n255\n";
    for (std::size_t i = 0; i < rgba.size(); i += 4U) {
        file.put(static_cast<char>(rgba[i + 0]));
        file.put(static_cast<char>(rgba[i + 1]));
        file.put(static_cast<char>(rgba[i + 2]));
    }
    return static_cast<bool>(file);
}

[[nodiscard]] bool writeRgba(fs::path const& path, std::vector<std::uint8_t> const& rgba) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file.write(reinterpret_cast<char const*>(rgba.data()), static_cast<std::streamsize>(rgba.size()));
    return static_cast<bool>(file);
}

[[nodiscard]] std::string jsonEscape(std::string_view value) {
    std::string escaped{};
    escaped.reserve(value.size() + 8U);
    for (char character : value) {
        switch (character) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped += character;
            break;
        }
    }
    return escaped;
}

[[nodiscard]] std::string makeReport(Options const& options, Camera const& camera,
                                     RayIntegrationSettings const& settings,
                                     RenderResult const& renderResult,
                                     std::string_view checksum,
                                     fs::path const& ppmPath, fs::path const& rgbaPath,
                                     bool rawRgbaWritten) {
    Metrics const& metrics = renderResult.metrics;
    const std::uint64_t expectedRays = static_cast<std::uint64_t>(options.width) * options.height *
                                       options.samplesPerAxis * options.samplesPerAxis;
    constexpr double residualThreshold = 5.0e-7;
    const bool classificationsComplete = metrics.rayCount() == expectedRays;
    const bool meaningfulCoverage = metrics.horizon > 0 && metrics.escaped > 0 && metrics.disk > 0;
    const bool validationPassed = classificationsComplete && meaningfulCoverage && metrics.invalid == 0 &&
                                  metrics.maxSteps == 0 && metrics.nonFiniteDiagnostics == 0 &&
                                  metrics.maximumRelativeNullResidual <= residualThreshold;

    const double raysPerSecond = renderResult.renderSeconds > 0.0
                                     ? static_cast<double>(metrics.rayCount()) / renderResult.renderSeconds
                                     : 0.0;
    const double pixelsPerSecond = renderResult.renderSeconds > 0.0
                                       ? static_cast<double>(options.width) * options.height /
                                             renderResult.renderSeconds
                                       : 0.0;
    const double averageAccepted = metrics.rayCount() > 0
                                       ? static_cast<double>(metrics.acceptedSteps) /
                                             static_cast<double>(metrics.rayCount())
                                       : 0.0;

    std::ostringstream json;
    json << std::setprecision(17);
    json << "{\n";
    json << "  \"schema_version\": 2,\n";
    json << "  \"lab\": \"Helios BlackHoleLab CPU Reference\",\n";
    json << "  \"deterministic_image\": true,\n";
    auto const heliosVersion = helios::heliosVersion();
    json << "  \"engine\": {\n";
    json << "    \"version\": \"" << heliosVersion.major << '.' << heliosVersion.minor << '.'
         << heliosVersion.patch << "\",\n";
    json << "    \"git_commit\": \"" << jsonEscape(helios::heliosGitHash()) << "\",\n";
    json << "    \"build_type\": \"" << jsonEscape(HELIOS_BUILD_TYPE) << "\"\n";
    json << "  },\n";
    json << "  \"model\": {\n";
    json << "    \"spacetime\": \"Schwarzschild exterior vacuum\",\n";
    json << "    \"dimensionless_spin_a_star\": 0.0,\n";
    json << "    \"units\": \"geometric units G=c=1; M=GM/c^2\",\n";
    json << "    \"ray_direction\": \"backward from a local static camera\",\n";
    json << "    \"integrator\": \"adaptive Dormand-Prince RK5(4), binary64\",\n";
    json << "    \"events\": [\"horizon\", \"escape sphere\", \"first optically-thick thin-disk crossing\"],\n";
    json << "    \"horizon_radius_M\": 2.0,\n";
    json << "    \"photon_sphere_radius_M\": 3.0,\n";
    json << "    \"isco_radius_M\": 6.0,\n";
    json << "    \"critical_impact_parameter_M\": "
         << helios::blackhole::criticalImpactParameter(settings.geometry.mass) / settings.geometry.mass
         << "\n";
    json << "  },\n";
    json << "  \"camera\": {\n";
    json << "    \"position_M\": [" << camera.position.v[0] << ", " << camera.position.v[1]
         << ", " << camera.position.v[2] << "],\n";
    json << "    \"distance_M\": " << options.cameraDistanceInMasses << ",\n";
    json << "    \"elevation_degrees\": " << options.cameraElevationDegrees << ",\n";
    json << "    \"vertical_fov_degrees\": " << options.verticalFovDegrees << "\n";
    json << "  },\n";
    json << "  \"image\": {\n";
    json << "    \"width\": " << options.width << ",\n";
    json << "    \"height\": " << options.height << ",\n";
    json << "    \"samples_per_axis\": " << options.samplesPerAxis << ",\n";
    json << "    \"sample_count_per_pixel\": " << options.samplesPerAxis * options.samplesPerAxis
         << ",\n";
    json << "    \"rgba8_fnv1a64\": \"" << checksum << "\",\n";
    json << "    \"ppm\": \"" << jsonEscape(ppmPath.generic_string()) << "\",\n";
    if (rawRgbaWritten) {
        json << "    \"raw_rgba8\": \"" << jsonEscape(rgbaPath.generic_string()) << "\"\n";
    } else {
        json << "    \"raw_rgba8\": null\n";
    }
    json << "  },\n";
    json << "  \"disk_model\": {\n";
    json << "    \"inner_radius_M\": " << settings.disk.innerRadiusInMasses << ",\n";
    json << "    \"outer_radius_M\": " << settings.disk.outerRadiusInMasses << ",\n";
    json << "    \"peak_temperature_K\": " << options.diskPeakTemperatureKelvin << ",\n";
    json << "    \"radial_flux\": \"Page-Thorne (1974) zero-torque thin disk, a*=0\",\n";
    json << "    \"emitter\": \"prograde circular geodesics, Omega=M^(1/2) r^(-3/2)\",\n";
    json << "    \"frequency_ratio\": \"g=sqrt(1-3M/r)/[sqrt(1-2M/r_cam)(1-Omega L_z/E)] from the conserved photon E and L_z\",\n";
    json << "    \"spectrum\": \"black body at g*T_eff (I_nu/nu^3 invariance), CIE 1931 to linear sRGB\",\n";
    json << "    \"limb_darkening\": \"(1+2.06 mu)/2.3733, mu measured in the gas frame\",\n";
    json << "    \"display\": \"exposure 3.4 x reference luminance of an unshifted black body at the peak temperature, ACES fit (Hill), sRGB\",\n";
    json << "    \"background\": \"deterministic procedural star cells sampled at escaped geodesic direction\"\n";
    json << "  },\n";
    json << "  \"classification\": {\n";
    json << "    \"total_rays\": " << metrics.rayCount() << ",\n";
    json << "    \"expected_rays\": " << expectedRays << ",\n";
    json << "    \"horizon\": " << metrics.horizon << ",\n";
    json << "    \"escaped\": " << metrics.escaped << ",\n";
    json << "    \"thin_disk\": " << metrics.disk << ",\n";
    json << "    \"max_steps\": " << metrics.maxSteps << ",\n";
    json << "    \"invalid_input\": " << metrics.invalid << "\n";
    json << "  },\n";
    json << "  \"performance\": {\n";
    json << "    \"worker_threads\": " << std::max(1u, std::min(options.threads, options.height))
         << ",\n";
    json << "    \"render_seconds\": " << renderResult.renderSeconds << ",\n";
    json << "    \"rays_per_second\": " << raysPerSecond << ",\n";
    json << "    \"pixels_per_second\": " << pixelsPerSecond << ",\n";
    json << "    \"accepted_steps\": " << metrics.acceptedSteps << ",\n";
    json << "    \"rejected_steps\": " << metrics.rejectedSteps << ",\n";
    json << "    \"average_accepted_steps_per_ray\": " << averageAccepted << ",\n";
    json << "    \"maximum_accepted_steps_for_one_ray\": " << metrics.maximumAcceptedSteps << "\n";
    json << "  },\n";
    json << "  \"validation\": {\n";
    json << "    \"status\": \"" << (validationPassed ? "pass" : "fail") << "\",\n";
    json << "    \"classifications_complete\": " << (classificationsComplete ? "true" : "false")
         << ",\n";
    json << "    \"contains_horizon_escape_and_disk\": " << (meaningfulCoverage ? "true" : "false")
         << ",\n";
    json << "    \"non_finite_diagnostic_rays\": " << metrics.nonFiniteDiagnostics << ",\n";
    json << "    \"maximum_absolute_null_residual\": " << metrics.maximumAbsoluteNullResidual << ",\n";
    json << "    \"maximum_relative_null_residual\": " << metrics.maximumRelativeNullResidual << ",\n";
    json << "    \"relative_null_residual_threshold\": " << residualThreshold << ",\n";
    json << "    \"minimum_closest_radius_M\": " << metrics.minimumClosestRadius / settings.geometry.mass
         << "\n";
    json << "  },\n";
    json << "  \"limitations\": [\n";
    json << "    \"a*=0 only: no Kerr spin, frame dragging, ergosphere, or rotating horizon\",\n";
    json << "    \"No GRMHD: disk density, temperature, magnetic fields, turbulence, and dynamics are not simulated\",\n";
    json << "    \"No general-relativistic radiative transfer: no absorption, scattering, optical depth integration, polarization, or spectral line transport\",\n";
    json << "    \"The disk is a smooth, razor-thin black body: no turbulence, thickness, self-irradiation, or colour-correction factor\",\n";
    json << "    \"The procedural star background is deterministic test content, not an astronomical catalogue\",\n";
    json << "    \"The CPU image is the correctness reference; helios_black_hole_realtime is the real-time Vulkan renderer\"\n";
    json << "  ]\n";
    json << "}\n";
    return json.str();
}

template <typename T>
[[nodiscard]] std::optional<T> parseNumber(std::string_view text) {
    T value{};
    auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] bool consumeValue(int& index, int argc, char** argv, std::string_view option,
                                std::string_view& value, std::string& error) {
    const std::string_view argument = argv[index];
    const std::string prefix = std::string(option) + "=";
    if (argument.starts_with(prefix)) {
        value = argument.substr(prefix.size());
        return true;
    }
    if (argument == option) {
        if (index + 1 >= argc) {
            error = "missing value for " + std::string(option);
            return false;
        }
        value = argv[++index];
        return true;
    }
    return false;
}

[[nodiscard]] std::optional<Options> parseOptions(int argc, char** argv, std::string& error) {
    Options options{};
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            options.showHelp = true;
            continue;
        }
        if (argument == "--no-rgba") {
            options.writeRawRgba = false;
            continue;
        }

        std::string_view value{};
        if (argument == "--width" || argument.starts_with("--width=")) {
            if (!consumeValue(index, argc, argv, "--width", value, error)) {
                return std::nullopt;
            }
            const auto parsed = parseNumber<std::uint32_t>(value);
            if (!parsed || *parsed < 16U || *parsed > 8192U) {
                error = "--width must be in [16, 8192]";
                return std::nullopt;
            }
            options.width = *parsed;
        } else if (argument == "--height" || argument.starts_with("--height=")) {
            if (!consumeValue(index, argc, argv, "--height", value, error)) {
                return std::nullopt;
            }
            const auto parsed = parseNumber<std::uint32_t>(value);
            if (!parsed || *parsed < 16U || *parsed > 8192U) {
                error = "--height must be in [16, 8192]";
                return std::nullopt;
            }
            options.height = *parsed;
        } else if (argument == "--samples" || argument.starts_with("--samples=")) {
            if (!consumeValue(index, argc, argv, "--samples", value, error)) {
                return std::nullopt;
            }
            const auto parsed = parseNumber<std::uint32_t>(value);
            if (!parsed || *parsed < 1U || *parsed > 4U) {
                error = "--samples is samples per axis and must be in [1, 4]";
                return std::nullopt;
            }
            options.samplesPerAxis = *parsed;
        } else if (argument == "--threads" || argument.starts_with("--threads=")) {
            if (!consumeValue(index, argc, argv, "--threads", value, error)) {
                return std::nullopt;
            }
            const auto parsed = parseNumber<std::uint32_t>(value);
            if (!parsed || *parsed < 1U || *parsed > 256U) {
                error = "--threads must be in [1, 256]";
                return std::nullopt;
            }
            options.threads = *parsed;
        } else if (argument == "--distance" || argument.starts_with("--distance=")) {
            if (!consumeValue(index, argc, argv, "--distance", value, error)) {
                return std::nullopt;
            }
            const auto parsed = parseNumber<double>(value);
            if (!parsed || !std::isfinite(*parsed) || *parsed <= 6.5 || *parsed >= 90.0) {
                error = "--distance must be finite and in (6.5, 90) M";
                return std::nullopt;
            }
            options.cameraDistanceInMasses = *parsed;
        } else if (argument == "--elevation" || argument.starts_with("--elevation=")) {
            if (!consumeValue(index, argc, argv, "--elevation", value, error)) {
                return std::nullopt;
            }
            const auto parsed = parseNumber<double>(value);
            if (!parsed || !std::isfinite(*parsed) || *parsed <= 1.0 || *parsed >= 85.0) {
                error = "--elevation must be finite and in (1, 85) degrees";
                return std::nullopt;
            }
            options.cameraElevationDegrees = *parsed;
        } else if (argument == "--fov" || argument.starts_with("--fov=")) {
            if (!consumeValue(index, argc, argv, "--fov", value, error)) {
                return std::nullopt;
            }
            const auto parsed = parseNumber<double>(value);
            if (!parsed || !std::isfinite(*parsed) || *parsed < 10.0 || *parsed > 120.0) {
                error = "--fov must be finite and in [10, 120] degrees";
                return std::nullopt;
            }
            options.verticalFovDegrees = *parsed;
        } else if (argument == "--temperature" || argument.starts_with("--temperature=")) {
            if (!consumeValue(index, argc, argv, "--temperature", value, error)) {
                return std::nullopt;
            }
            const auto parsed = parseNumber<double>(value);
            if (!parsed || !std::isfinite(*parsed) || *parsed < 1000.0 || *parsed > 500000.0) {
                error = "--temperature must be finite and in [1000, 500000] K";
                return std::nullopt;
            }
            options.diskPeakTemperatureKelvin = *parsed;
        } else if (argument == "--output-dir" || argument.starts_with("--output-dir=")) {
            if (!consumeValue(index, argc, argv, "--output-dir", value, error)) {
                return std::nullopt;
            }
            if (value.empty()) {
                error = "--output-dir must not be empty";
                return std::nullopt;
            }
            options.outputDirectory = fs::path{value};
        } else {
            error = "unknown option: " + std::string(argument);
            return std::nullopt;
        }
    }
    return options;
}

void printHelp() {
    std::cout
        << "Helios BlackHoleLab CPU Reference\n\n"
        << "Usage: helios_black_hole_lab [options]\n\n"
        << "  --width N          Image width, default 320\n"
        << "  --height N         Image height, default 180\n"
        << "  --samples N        Deterministic samples per pixel axis (N*N), default 1\n"
        << "  --threads N        CPU worker threads\n"
        << "  --distance M       Camera radius in gravitational lengths, default 20\n"
        << "  --elevation DEG    Camera elevation above disk, default 20\n"
        << "  --fov DEG          Vertical field of view, default 50\n"
        << "  --temperature K    Peak disk temperature, default 5200\n"
        << "  --output-dir PATH  Output directory\n"
        << "  --no-rgba          Write PPM and JSON only, omitting raw RGBA8\n"
        << "  --help              Show this help\n\n"
        << "Outputs: black_hole.ppm, black_hole.rgba, report.json. No window or GPU is used.\n";
}

} // namespace

int main(int argc, char** argv) {
    std::string argumentError{};
    const std::optional<Options> parsed = parseOptions(argc, argv, argumentError);
    if (!parsed) {
        std::cerr << "BlackHoleLab: " << argumentError << '\n';
        return 2;
    }
    Options const& options = *parsed;
    if (options.showHelp) {
        printHelp();
        return 0;
    }

    std::error_code directoryError{};
    fs::create_directories(options.outputDirectory, directoryError);
    if (directoryError) {
        std::cerr << "BlackHoleLab: cannot create output directory: "
                  << directoryError.message() << '\n';
        return 2;
    }

    const Camera camera = makeCamera(options);
    const RayIntegrationSettings settings = makeSettings();
    const Radiometry radiometry = makeRadiometry(options.diskPeakTemperatureKelvin);
    std::cout << "BlackHoleLab: tracing " << options.width << 'x' << options.height << " at "
              << options.samplesPerAxis * options.samplesPerAxis << " sample(s)/pixel on "
              << std::max(1u, std::min(options.threads, options.height)) << " thread(s)\n";

    RenderResult result = render(options, camera, settings, radiometry);
    const fs::path ppmPath = options.outputDirectory / "black_hole.ppm";
    const fs::path rgbaPath = options.outputDirectory / "black_hole.rgba";
    const fs::path reportPath = options.outputDirectory / "report.json";

    if (!writePpm(ppmPath, options.width, options.height, result.rgba)) {
        std::cerr << "BlackHoleLab: failed to write " << ppmPath << '\n';
        return 2;
    }
    bool rawRgbaWritten = false;
    if (options.writeRawRgba) {
        rawRgbaWritten = writeRgba(rgbaPath, result.rgba);
        if (!rawRgbaWritten) {
            std::cerr << "BlackHoleLab: failed to write " << rgbaPath << '\n';
            return 2;
        }
    }

    const std::string checksum = hex64(fnv1a64(result.rgba));
    const std::string report = makeReport(options, camera, settings, result, checksum,
                                          ppmPath, rgbaPath, rawRgbaWritten);
    {
        std::ofstream reportFile(reportPath, std::ios::binary | std::ios::trunc);
        reportFile.write(report.data(), static_cast<std::streamsize>(report.size()));
        if (!reportFile) {
            std::cerr << "BlackHoleLab: failed to write " << reportPath << '\n';
            return 2;
        }
    }

    const bool passed = report.find("\"status\": \"pass\"") != std::string::npos;
    std::cout << std::fixed << std::setprecision(3)
              << "BlackHoleLab: " << result.metrics.rayCount() << " rays in "
              << result.renderSeconds << " s ("
              << (result.renderSeconds > 0.0
                      ? static_cast<double>(result.metrics.rayCount()) / result.renderSeconds
                      : 0.0)
              << " rays/s)\n"
              << "  horizon=" << result.metrics.horizon << " disk=" << result.metrics.disk
              << " escaped=" << result.metrics.escaped << " max_steps=" << result.metrics.maxSteps
              << " invalid=" << result.metrics.invalid << '\n'
              << "  max relative null residual=" << std::scientific
              << result.metrics.maximumRelativeNullResidual << '\n'
              << "  rgba8 fnv1a64=" << checksum << '\n'
              << "  image=" << ppmPath << '\n'
              << "  report=" << reportPath << '\n'
              << "  validation=" << (passed ? "PASS" : "FAIL") << '\n';
    return passed ? 0 : 3;
}
