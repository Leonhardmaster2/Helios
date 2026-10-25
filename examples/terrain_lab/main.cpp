// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Deterministic, headless terrain inspection for the v0.0.1 content contract.

#include "helios/math/vec.hpp"
#include "helios/planet/cube_sphere.hpp"
#include "helios/planet/residency.hpp"
#include "helios/planet/terrain_generator.hpp"

#include <algorithm>
#include <array>
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
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef HELIOS_GIT_HASH
#define HELIOS_GIT_HASH "unknown"
#endif

#ifndef HELIOS_BUILD_TYPE
#define HELIOS_BUILD_TYPE "unknown"
#endif

namespace fs = std::filesystem;
using helios::math::Vec3d;
using namespace helios::planet;

namespace {

constexpr double kRadiansToDegrees = 57.2957795130823208768;
constexpr std::uint32_t kSamples = kTerrainTileSamplesPerEdge;

struct Options final {
    fs::path outputDirectory = "terrain_lab_output";
    TileKey key{CubeFace::PositiveZ, 4, 0, 0};
    TerrainGeneratorConfig generator{};
    std::uint32_t scale = 8;
    bool xExplicit = false;
    bool yExplicit = false;
    bool showHelp = false;
};

struct ScalarStats final {
    double minimum = std::numeric_limits<double>::infinity();
    double maximum = -std::numeric_limits<double>::infinity();
    long double sum = 0.0L;
    long double sumSquares = 0.0L;
    std::uint64_t count = 0;

    void add(double value) noexcept {
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
        sum += static_cast<long double>(value);
        sumSquares += static_cast<long double>(value) * static_cast<long double>(value);
        ++count;
    }

    [[nodiscard]] double mean() const noexcept {
        return count == 0 ? 0.0 : static_cast<double>(sum / static_cast<long double>(count));
    }

    [[nodiscard]] double rms() const noexcept {
        return count == 0
                   ? 0.0
                   : std::sqrt(static_cast<double>(sumSquares / static_cast<long double>(count)));
    }
};

struct TileMetrics final {
    ScalarStats height{};
    ScalarStats parentHeight{};
    ScalarStats residual{};
    ScalarStats absoluteResidual{};
    ScalarStats slopeDegrees{};
    ScalarStats normalLength{};
    double maximumNormalLengthError = 0.0;
    double minimumOutwardDot = std::numeric_limits<double>::infinity();
    bool finite = true;
};

struct SeamCheck final {
    std::string name{};
    std::string detail{};
    bool applicable = true;
    bool passed = true;
    std::uint32_t comparedSamples = 0;
    std::uint32_t exactSamples = 0;
    double maximumHeightDifferenceM = 0.0;
    double maximumParentHeightDifferenceM = 0.0;
    double maximumDirectionComponentDifference = 0.0;
    double maximumNormalComponentDifference = 0.0;
};

struct ParentCheck final {
    bool applicable = false;
    bool passed = true;
    std::uint32_t comparedSamples = 0;
    std::uint32_t exactSamples = 0;
    double maximumHeightDifferenceM = 0.0;
    double maximumDirectionComponentDifference = 0.0;
};

struct ResidencyFrame final {
    std::uint64_t index = 0;
    ResidencyFrameStats stats{};
    std::vector<TileKey> admitted{};
    std::vector<TileKey> uploaded{};
    std::vector<TileKey> retired{};
    std::vector<ResidencyRecord> records{};
};

struct ResidencyTrace final {
    ResidencyBudgets budgets{};
    std::uint64_t tileBytes = 0;
    std::vector<ResidencyFrame> frames{};
    bool stayedWithinBounds = true;
    bool observedRequestThrottle = false;
    bool observedUploadThrottle = false;
    bool observedPriorityEviction = false;
    bool finalExpectedResidents = false;
    bool passed = false;
};

using Rgb = std::array<std::uint8_t, 3>;

struct ImageView final {
    std::string filename{};
    std::string description{};
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgb{};
    std::uint64_t checksum = 0;
};

[[nodiscard]] std::size_t sampleIndex(std::uint32_t x, std::uint32_t y) noexcept {
    return static_cast<std::size_t>(y) * kSamples + x;
}

[[nodiscard]] std::string hex64(std::uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << value;
    return stream.str();
}

[[nodiscard]] std::uint64_t hashBytes(std::span<std::uint8_t const> bytes,
                                      std::uint64_t hash = 14695981039346656037ULL) noexcept {
    constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
    for (std::uint8_t byte : bytes) {
        hash = (hash ^ byte) * kFnvPrime;
    }
    return hash;
}

[[nodiscard]] std::uint64_t hashText(std::string_view text,
                                     std::uint64_t hash) noexcept {
    return hashBytes(std::span{reinterpret_cast<std::uint8_t const*>(text.data()), text.size()},
                     hash);
}

[[nodiscard]] std::uint8_t toByte(double value) noexcept {
    return static_cast<std::uint8_t>(
        std::clamp(std::lround(std::clamp(value, 0.0, 1.0) * 255.0), 0L, 255L));
}

[[nodiscard]] Rgb mix(Rgb const& left, Rgb const& right, double amount) noexcept {
    const double t = std::clamp(amount, 0.0, 1.0);
    Rgb result{};
    for (std::size_t component = 0; component < result.size(); ++component) {
        result[component] = static_cast<std::uint8_t>(std::clamp(
            std::lround(static_cast<double>(left[component]) * (1.0 - t) +
                        static_cast<double>(right[component]) * t),
            0L, 255L));
    }
    return result;
}

[[nodiscard]] Rgb terrainRamp(double value) noexcept {
    constexpr std::array<Rgb, 6> kStops{{
        Rgb{15, 23, 42}, Rgb{25, 74, 92}, Rgb{43, 126, 99},
        Rgb{118, 166, 83}, Rgb{203, 194, 116}, Rgb{247, 240, 214},
    }};
    const double position = std::clamp(value, 0.0, 1.0) *
                            static_cast<double>(kStops.size() - 1U);
    const auto lower = static_cast<std::size_t>(std::floor(position));
    const std::size_t upper = std::min(lower + 1U, kStops.size() - 1U);
    return mix(kStops[lower], kStops[upper], position - static_cast<double>(lower));
}

[[nodiscard]] Rgb heatRamp(double value) noexcept {
    constexpr std::array<Rgb, 5> kStops{{
        Rgb{12, 10, 28}, Rgb{70, 20, 95}, Rgb{174, 42, 72},
        Rgb{241, 121, 48}, Rgb{255, 239, 157},
    }};
    const double position = std::clamp(value, 0.0, 1.0) *
                            static_cast<double>(kStops.size() - 1U);
    const auto lower = static_cast<std::size_t>(std::floor(position));
    const std::size_t upper = std::min(lower + 1U, kStops.size() - 1U);
    return mix(kStops[lower], kStops[upper], position - static_cast<double>(lower));
}

template <typename ColorFunction>
[[nodiscard]] ImageView makeView(std::string filename, std::string description,
                                 std::uint32_t scale, ColorFunction colorAt) {
    ImageView view{};
    view.filename = std::move(filename);
    view.description = std::move(description);
    view.width = kSamples * scale;
    view.height = kSamples * scale;
    view.rgb.resize(static_cast<std::size_t>(view.width) * view.height * 3U);

    for (std::uint32_t outputY = 0; outputY < view.height; ++outputY) {
        // Terrain v grows northward; image row zero is the top of the image.
        const std::uint32_t sourceY = kSamples - 1U - outputY / scale;
        for (std::uint32_t outputX = 0; outputX < view.width; ++outputX) {
            const std::uint32_t sourceX = outputX / scale;
            const Rgb color = colorAt(sourceX, sourceY);
            const std::size_t pixel =
                (static_cast<std::size_t>(outputY) * view.width + outputX) * 3U;
            view.rgb[pixel + 0U] = color[0];
            view.rgb[pixel + 1U] = color[1];
            view.rgb[pixel + 2U] = color[2];
        }
    }
    view.checksum = hashBytes(view.rgb);
    return view;
}

[[nodiscard]] bool writePpm(fs::path const& path, ImageView const& view) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file << "P6\n# Helios TerrainLab deterministic inspection view\n"
         << view.width << ' ' << view.height << "\n255\n";
    file.write(reinterpret_cast<char const*>(view.rgb.data()),
               static_cast<std::streamsize>(view.rgb.size()));
    return static_cast<bool>(file);
}

[[nodiscard]] bool sameVector(Vec3d const& left, Vec3d const& right) noexcept {
    return left.v[0] == right.v[0] && left.v[1] == right.v[1] &&
           left.v[2] == right.v[2];
}

[[nodiscard]] bool sameSample(TerrainSample const& left,
                              TerrainSample const& right) noexcept {
    return sameVector(left.direction, right.direction) && left.height_m == right.height_m &&
           left.parent_compatible_height_m == right.parent_compatible_height_m &&
           sameVector(left.normal, right.normal);
}

void addSeamPair(SeamCheck& check, TerrainSample const& left,
                 TerrainSample const& right) noexcept {
    ++check.comparedSamples;
    if (sameSample(left, right)) {
        ++check.exactSamples;
    } else {
        check.passed = false;
    }
    check.maximumHeightDifferenceM =
        std::max(check.maximumHeightDifferenceM, std::abs(left.height_m - right.height_m));
    check.maximumParentHeightDifferenceM =
        std::max(check.maximumParentHeightDifferenceM,
                 std::abs(left.parent_compatible_height_m - right.parent_compatible_height_m));
    for (std::size_t component = 0; component < 3U; ++component) {
        check.maximumDirectionComponentDifference =
            std::max(check.maximumDirectionComponentDifference,
                     std::abs(left.direction.v[component] - right.direction.v[component]));
        check.maximumNormalComponentDifference =
            std::max(check.maximumNormalComponentDifference,
                     std::abs(left.normal.v[component] - right.normal.v[component]));
    }
}

[[nodiscard]] bool buildSeamChecks(TerrainGenerator const& generator,
                                   TerrainTile const& selected,
                                   std::vector<SeamCheck>& checks,
                                   std::string& error) {
    const std::uint64_t side = std::uint64_t{1} << selected.key.level;

    SeamCheck xCheck{"selected_same_face_x", "selected tile against one x-axis neighbour"};
    if (side == 1U) {
        xCheck.applicable = false;
        xCheck.detail = "root face has no same-face x neighbour";
    } else {
        TileKey neighbourKey = selected.key;
        const bool east = static_cast<std::uint64_t>(selected.key.x) + 1U < side;
        neighbourKey.x = east ? selected.key.x + 1U : selected.key.x - 1U;
        auto neighbourResult = generator.generate(neighbourKey);
        if (neighbourResult.failed()) {
            error = neighbourResult.error().to_string();
            return false;
        }
        TerrainTile const& neighbour = neighbourResult.value();
        for (std::uint32_t y = 0; y < kSamples; ++y) {
            const std::uint32_t selectedX = east ? kSamples - 1U : 0U;
            const std::uint32_t neighbourX = east ? 0U : kSamples - 1U;
            addSeamPair(xCheck, selected.samples[sampleIndex(selectedX, y)],
                        neighbour.samples[sampleIndex(neighbourX, y)]);
        }
        xCheck.detail = std::string(east ? "east edge vs " : "west edge vs ") +
                        toString(neighbourKey);
    }
    checks.push_back(std::move(xCheck));

    SeamCheck yCheck{"selected_same_face_y", "selected tile against one y-axis neighbour"};
    if (side == 1U) {
        yCheck.applicable = false;
        yCheck.detail = "root face has no same-face y neighbour";
    } else {
        TileKey neighbourKey = selected.key;
        const bool north = static_cast<std::uint64_t>(selected.key.y) + 1U < side;
        neighbourKey.y = north ? selected.key.y + 1U : selected.key.y - 1U;
        auto neighbourResult = generator.generate(neighbourKey);
        if (neighbourResult.failed()) {
            error = neighbourResult.error().to_string();
            return false;
        }
        TerrainTile const& neighbour = neighbourResult.value();
        for (std::uint32_t x = 0; x < kSamples; ++x) {
            const std::uint32_t selectedY = north ? kSamples - 1U : 0U;
            const std::uint32_t neighbourY = north ? 0U : kSamples - 1U;
            addSeamPair(yCheck, selected.samples[sampleIndex(x, selectedY)],
                        neighbour.samples[sampleIndex(x, neighbourY)]);
        }
        yCheck.detail = std::string(north ? "north edge vs " : "south edge vs ") +
                        toString(neighbourKey);
    }
    checks.push_back(std::move(yCheck));

    SeamCheck cubeCheck{"canonical_cross_face",
                        "+X west edge against +Z east edge at the root level"};
    auto positiveX = generator.generate(TileKey{CubeFace::PositiveX, 0, 0, 0});
    auto positiveZ = generator.generate(TileKey{CubeFace::PositiveZ, 0, 0, 0});
    if (positiveX.failed()) {
        error = positiveX.error().to_string();
        return false;
    }
    if (positiveZ.failed()) {
        error = positiveZ.error().to_string();
        return false;
    }
    for (std::uint32_t y = 0; y < kSamples; ++y) {
        addSeamPair(cubeCheck, positiveX.value().samples[sampleIndex(0U, y)],
                    positiveZ.value().samples[sampleIndex(kSamples - 1U, y)]);
    }
    checks.push_back(std::move(cubeCheck));
    return true;
}

[[nodiscard]] bool buildParentCheck(TerrainGenerator const& generator,
                                    TerrainTile const& selected, ParentCheck& check,
                                    std::string& error) {
    if (selected.key.level == 0U) {
        check.applicable = false;
        return true;
    }

    check.applicable = true;
    auto parentKey = tileParent(selected.key);
    if (parentKey.failed()) {
        error = parentKey.error().to_string();
        return false;
    }
    auto parentResult = generator.generate(parentKey.value());
    if (parentResult.failed()) {
        error = parentResult.error().to_string();
        return false;
    }
    TerrainTile const& parent = parentResult.value();
    const std::uint32_t quadrantX = selected.key.x & 1U;
    const std::uint32_t quadrantY = selected.key.y & 1U;

    for (std::uint32_t childY = 0; childY < kSamples; childY += 2U) {
        for (std::uint32_t childX = 0; childX < kSamples; childX += 2U) {
            const std::uint32_t parentX = quadrantX * 16U + childX / 2U;
            const std::uint32_t parentY = quadrantY * 16U + childY / 2U;
            TerrainSample const& child = selected.samples[sampleIndex(childX, childY)];
            TerrainSample const& parentSample = parent.samples[sampleIndex(parentX, parentY)];
            ++check.comparedSamples;
            const bool exact = sameVector(child.direction, parentSample.direction) &&
                               child.parent_compatible_height_m == parentSample.height_m;
            if (exact) {
                ++check.exactSamples;
            } else {
                check.passed = false;
            }
            check.maximumHeightDifferenceM =
                std::max(check.maximumHeightDifferenceM,
                         std::abs(child.parent_compatible_height_m - parentSample.height_m));
            for (std::size_t component = 0; component < 3U; ++component) {
                check.maximumDirectionComponentDifference =
                    std::max(check.maximumDirectionComponentDifference,
                             std::abs(child.direction.v[component] -
                                      parentSample.direction.v[component]));
            }
        }
    }
    return true;
}

[[nodiscard]] ResidencyFrame captureFrame(ResidencySnapshot const& snapshot) {
    return ResidencyFrame{snapshot.frameIndex(), snapshot.stats(), snapshot.admittedRequests(),
                          snapshot.completedUploads(), snapshot.retiredTiles(), snapshot.records()};
}

[[nodiscard]] bool runResidencyTrace(TileKey const& selected, std::uint64_t tileBytes,
                                     ResidencyTrace& trace, std::string& error) {
    trace.tileBytes = std::max<std::uint64_t>(tileBytes, 1U);
    trace.budgets.max_resident_tiles = 2;
    trace.budgets.max_cpu_bytes = trace.tileBytes * 2U;
    trace.budgets.max_upload_bytes_per_frame = trace.tileBytes;
    trace.budgets.max_new_requests_per_frame = 2;
    trace.budgets.max_pending_tiles = 4;
    trace.budgets.resident_grace_frames = 100;

    TileKey second = selected;
    TileKey third = selected;
    const std::uint64_t side = std::uint64_t{1} << selected.level;
    if (side > 1U) {
        second.x = static_cast<std::uint32_t>((static_cast<std::uint64_t>(selected.x) + 1U) % side);
        third.y = static_cast<std::uint32_t>((static_cast<std::uint64_t>(selected.y) + 1U) % side);
    } else {
        second.face = static_cast<CubeFace>((static_cast<std::uint8_t>(selected.face) + 1U) %
                                            kCubeFaceCount);
        third.face = static_cast<CubeFace>((static_cast<std::uint8_t>(selected.face) + 2U) %
                                           kCubeFaceCount);
    }

    auto request = [&](TileKey key, double priority) {
        return ResidencyRequest{key, priority, trace.tileBytes, trace.tileBytes};
    };
    auto completion = [&](TileKey key) {
        return ResidencyCompletion{key, true, trace.tileBytes, trace.tileBytes};
    };

    ResidencySnapshot snapshot{};
    auto update = [&](std::span<ResidencyRequest const> requests,
                      std::span<ResidencyCompletion const> completions) -> bool {
        auto next = updateResidency(snapshot, trace.budgets, requests, completions);
        if (next.failed()) {
            error = next.error().to_string();
            return false;
        }
        snapshot = std::move(next.value());
        trace.frames.push_back(captureFrame(snapshot));
        ResidencyFrameStats const& stats = snapshot.stats();
        trace.stayedWithinBounds =
            trace.stayedWithinBounds &&
            stats.resident_tiles <= trace.budgets.max_resident_tiles &&
            stats.resident_cpu_bytes <= trace.budgets.max_cpu_bytes &&
            stats.uploaded_bytes <= trace.budgets.max_upload_bytes_per_frame &&
            stats.pending_tiles <= trace.budgets.max_pending_tiles;
        trace.observedRequestThrottle =
            trace.observedRequestThrottle || stats.request_budget_limited;
        trace.observedUploadThrottle =
            trace.observedUploadThrottle || stats.upload_budget_limited;
        return true;
    };

    const std::array<ResidencyRequest, 3> initial{
        request(selected, 100.0), request(second, 50.0), request(third, 10.0)};
    if (!update(initial, {})) {
        return false;
    }
    if (!update(initial, {})) {
        return false;
    }
    const std::array<ResidencyCompletion, 2> firstCompletions{
        completion(selected), completion(second)};
    if (!update(initial, firstCompletions)) {
        return false;
    }
    const ResidencyCompletion secondCompletion = completion(second);
    if (!update(initial, std::span{&secondCompletion, 1U})) {
        return false;
    }

    const std::array<ResidencyRequest, 3> reprioritized{
        request(selected, 30.0), request(second, 20.0), request(third, 200.0)};
    const ResidencyCompletion thirdCompletion = completion(third);
    if (!update(reprioritized, std::span{&thirdCompletion, 1U})) {
        return false;
    }
    const std::array<ResidencyRequest, 2> finalRequests{
        request(selected, 30.0), request(third, 200.0)};
    if (!update(finalRequests, {})) {
        return false;
    }

    if (trace.frames.size() >= 5U) {
        trace.observedPriorityEviction =
            std::find(trace.frames[4].retired.begin(), trace.frames[4].retired.end(), second) !=
            trace.frames[4].retired.end();
    }
    auto const* selectedRecord = snapshot.find(selected);
    auto const* thirdRecord = snapshot.find(third);
    trace.finalExpectedResidents = selectedRecord != nullptr && thirdRecord != nullptr &&
                                   selectedRecord->status == ResidencyStatus::Resident &&
                                   thirdRecord->status == ResidencyStatus::Resident &&
                                   snapshot.stats().resident_tiles == 2U;
    trace.passed = trace.stayedWithinBounds && trace.observedRequestThrottle &&
                   trace.observedUploadThrottle && trace.observedPriorityEviction &&
                   trace.finalExpectedResidents;
    return true;
}

[[nodiscard]] TileMetrics measureTile(TerrainTile const& tile) noexcept {
    TileMetrics metrics{};
    for (TerrainSample const& sample : tile.samples) {
        const double residual = sample.height_m - sample.parent_compatible_height_m;
        const double normalLength = helios::math::length(sample.normal);
        const double outwardDot = helios::math::dot(sample.direction, sample.normal);
        const double slopeRadians =
            std::acos(std::clamp(outwardDot / std::max(normalLength, 1.0e-300), -1.0, 1.0));
        const double slopeDegrees = slopeRadians * kRadiansToDegrees;

        metrics.height.add(sample.height_m);
        metrics.parentHeight.add(sample.parent_compatible_height_m);
        metrics.residual.add(residual);
        metrics.absoluteResidual.add(std::abs(residual));
        metrics.slopeDegrees.add(slopeDegrees);
        metrics.normalLength.add(normalLength);
        metrics.maximumNormalLengthError =
            std::max(metrics.maximumNormalLengthError, std::abs(normalLength - 1.0));
        metrics.minimumOutwardDot = std::min(metrics.minimumOutwardDot, outwardDot);
        metrics.finite = metrics.finite && std::isfinite(sample.height_m) &&
                         std::isfinite(sample.parent_compatible_height_m) &&
                         std::isfinite(normalLength) && std::isfinite(outwardDot) &&
                         std::isfinite(slopeDegrees);
    }
    return metrics;
}

[[nodiscard]] std::vector<ImageView> buildViews(TerrainTile const& tile,
                                                TileMetrics const& metrics,
                                                std::uint32_t scale) {
    const double commonMinimum = std::min(metrics.height.minimum, metrics.parentHeight.minimum);
    const double commonMaximum = std::max(metrics.height.maximum, metrics.parentHeight.maximum);
    const double commonRange = std::max(commonMaximum - commonMinimum, 1.0e-12);
    const double errorBound = std::max(tile.parent_relative_geometric_error_m, 1.0e-12);

    std::vector<ImageView> views{};
    views.reserve(6U);
    views.push_back(makeView(
        "height.ppm", "generated child-band height; shared observed height scale", scale,
        [&](std::uint32_t x, std::uint32_t y) {
            const double height = tile.samples[sampleIndex(x, y)].height_m;
            return terrainRamp((height - commonMinimum) / commonRange);
        }));
    views.push_back(makeView(
        "normal.ppm", "world-space XYZ normal encoded as RGB", scale,
        [&](std::uint32_t x, std::uint32_t y) {
            Vec3d const& normal = tile.samples[sampleIndex(x, y)].normal;
            return Rgb{toByte(normal.v[0] * 0.5 + 0.5), toByte(normal.v[1] * 0.5 + 0.5),
                       toByte(normal.v[2] * 0.5 + 0.5)};
        }));
    views.push_back(makeView(
        "slope.ppm", "surface slope in degrees; fixed 0 to 60 degree heat scale", scale,
        [&](std::uint32_t x, std::uint32_t y) {
            TerrainSample const& sample = tile.samples[sampleIndex(x, y)];
            const double normalLength = helios::math::length(sample.normal);
            const double cosine = helios::math::dot(sample.direction, sample.normal) /
                                  std::max(normalLength, 1.0e-300);
            const double degrees = std::acos(std::clamp(cosine, -1.0, 1.0)) *
                                   kRadiansToDegrees;
            return heatRamp(degrees / 60.0);
        }));
    views.push_back(makeView(
        "parent_height.ppm", "parent-octave-band height; shared observed height scale", scale,
        [&](std::uint32_t x, std::uint32_t y) {
            const double height =
                tile.samples[sampleIndex(x, y)].parent_compatible_height_m;
            return terrainRamp((height - commonMinimum) / commonRange);
        }));
    views.push_back(makeView(
        "residual.ppm", "signed child minus parent detail; blue negative, red positive", scale,
        [&](std::uint32_t x, std::uint32_t y) {
            TerrainSample const& sample = tile.samples[sampleIndex(x, y)];
            const double residual = sample.height_m - sample.parent_compatible_height_m;
            const double amount = std::clamp(std::abs(residual) / errorBound, 0.0, 1.0);
            constexpr Rgb kNeutral{232, 232, 226};
            constexpr Rgb kPositive{220, 38, 28};
            constexpr Rgb kNegative{30, 76, 210};
            return mix(kNeutral, residual >= 0.0 ? kPositive : kNegative, amount);
        }));
    views.push_back(makeView(
        "error.ppm", "absolute child-parent residual divided by analytic error bound", scale,
        [&](std::uint32_t x, std::uint32_t y) {
            TerrainSample const& sample = tile.samples[sampleIndex(x, y)];
            return heatRamp(std::abs(sample.height_m - sample.parent_compatible_height_m) /
                            errorBound);
        }));
    return views;
}

[[nodiscard]] std::string jsonEscape(std::string_view text) {
    std::string escaped{};
    escaped.reserve(text.size() + 8U);
    for (char character : text) {
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

void writeStats(std::ostream& output, ScalarStats const& stats,
                std::string_view indentation) {
    output << indentation << "{\"min\": " << stats.minimum << ", \"max\": "
           << stats.maximum << ", \"mean\": " << stats.mean() << ", \"rms\": "
           << stats.rms() << '}';
}

void writeTileKey(std::ostream& output, TileKey const& key) {
    output << "{\"face\": \"" << toString(key.face) << "\", \"level\": "
           << static_cast<unsigned int>(key.level) << ", \"x\": " << key.x
           << ", \"y\": " << key.y << '}';
}

[[nodiscard]] std::string makeReport(
    Options const& options, TerrainTile const& tile, TileMetrics const& metrics,
    std::vector<ImageView> const& views, std::vector<SeamCheck> const& seams,
    ParentCheck const& parentCheck, ResidencyTrace const& residency,
    bool stableHashValid, bool regenerationMatches, std::uint64_t bundleChecksum,
    double generationMilliseconds, double outputMilliseconds, bool validationPassed) {
    std::ostringstream json;
    json << std::setprecision(17);
    json << "{\n";
    json << "  \"schema_version\": 1,\n";
    json << "  \"lab\": \"Helios TerrainLab v0.0.1\",\n";
    json << "  \"deterministic_content\": true,\n";
    json << "  \"provenance\": {\n";
    json << "    \"helios_semantic_version\": \"0.0.1\",\n";
    json << "    \"git_commit\": \"" << jsonEscape(HELIOS_GIT_HASH) << "\",\n";
    json << "    \"build_type\": \"" << jsonEscape(HELIOS_BUILD_TYPE) << "\",\n";
    json << "    \"terrain_generator_version\": " << tile.generator_version << ",\n";
    json << "    \"seed_decimal\": \"" << tile.seed << "\",\n";
    json << "    \"seed_hex\": \"0x" << hex64(tile.seed) << "\",\n";
    json << "    \"tile_content_fnv1a64\": \"0x" << hex64(tile.content_hash) << "\",\n";
    json << "    \"view_bundle_fnv1a64\": \"0x" << hex64(bundleChecksum) << "\"\n";
    json << "  },\n";
    json << "  \"tile\": ";
    writeTileKey(json, tile.key);
    json << ",\n";
    json << "  \"generator_config\": {\n";
    json << "    \"planet_radius_m\": " << options.generator.planet_radius_m << ",\n";
    json << "    \"height_amplitude_m\": " << options.generator.height_amplitude_m << ",\n";
    json << "    \"base_frequency\": " << options.generator.base_frequency << ",\n";
    json << "    \"lacunarity\": " << options.generator.lacunarity << ",\n";
    json << "    \"persistence\": " << options.generator.persistence << ",\n";
    json << "    \"root_octaves\": "
         << static_cast<unsigned int>(options.generator.root_octaves) << ",\n";
    json << "    \"max_octaves\": "
         << static_cast<unsigned int>(options.generator.max_octaves) << ",\n";
    json << "    \"normal_angular_step_rad\": "
         << options.generator.normal_angular_step_rad << "\n";
    json << "  },\n";
    json << "  \"tile_content\": {\n";
    json << "    \"samples_per_edge\": " << kSamples << ",\n";
    json << "    \"sample_count\": " << tile.samples.size() << ",\n";
    json << "    \"active_octaves\": " << static_cast<unsigned int>(tile.octave_count)
         << ",\n";
    json << "    \"parent_octaves\": "
         << static_cast<unsigned int>(tile.parent_octave_count) << ",\n";
    json << "    \"reported_min_height_m\": " << tile.min_height_m << ",\n";
    json << "    \"reported_max_height_m\": " << tile.max_height_m << ",\n";
    json << "    \"reported_max_observed_parent_delta_m\": "
         << tile.max_observed_parent_delta_m << ",\n";
    json << "    \"parent_relative_geometric_error_bound_m\": "
         << tile.parent_relative_geometric_error_m << ",\n";
    json << "    \"stable_hash_recomputed\": " << (stableHashValid ? "true" : "false")
         << ",\n";
    json << "    \"regenerated_hash_matches\": "
         << (regenerationMatches ? "true" : "false") << "\n";
    json << "  },\n";
    json << "  \"metrics\": {\n";
    json << "    \"height_m\": ";
    writeStats(json, metrics.height, "");
    json << ",\n    \"parent_height_m\": ";
    writeStats(json, metrics.parentHeight, "");
    json << ",\n    \"signed_residual_m\": ";
    writeStats(json, metrics.residual, "");
    json << ",\n    \"absolute_residual_m\": ";
    writeStats(json, metrics.absoluteResidual, "");
    json << ",\n    \"slope_degrees\": ";
    writeStats(json, metrics.slopeDegrees, "");
    json << ",\n    \"normal_length\": ";
    writeStats(json, metrics.normalLength, "");
    json << ",\n";
    json << "    \"maximum_normal_length_error\": " << metrics.maximumNormalLengthError
         << ",\n";
    json << "    \"minimum_normal_dot_radial_up\": " << metrics.minimumOutwardDot << "\n";
    json << "  },\n";
    json << "  \"seam_checks\": [\n";
    for (std::size_t index = 0; index < seams.size(); ++index) {
        SeamCheck const& seam = seams[index];
        json << "    {\"name\": \"" << jsonEscape(seam.name) << "\", \"status\": \""
             << (!seam.applicable ? "not_applicable" : seam.passed ? "pass" : "fail")
             << "\", \"detail\": \"" << jsonEscape(seam.detail)
             << "\", \"compared_samples\": " << seam.comparedSamples
             << ", \"bit_exact_samples\": " << seam.exactSamples
             << ", \"max_height_difference_m\": " << seam.maximumHeightDifferenceM
             << ", \"max_parent_height_difference_m\": "
             << seam.maximumParentHeightDifferenceM
             << ", \"max_direction_component_difference\": "
             << seam.maximumDirectionComponentDifference
             << ", \"max_normal_component_difference\": "
             << seam.maximumNormalComponentDifference << '}';
        json << (index + 1U == seams.size() ? "\n" : ",\n");
    }
    json << "  ],\n";
    json << "  \"parent_compatibility\": {\"status\": \""
         << (!parentCheck.applicable ? "not_applicable" : parentCheck.passed ? "pass" : "fail")
         << "\", \"compared_samples\": " << parentCheck.comparedSamples
         << ", \"bit_exact_samples\": " << parentCheck.exactSamples
         << ", \"max_height_difference_m\": " << parentCheck.maximumHeightDifferenceM
         << ", \"max_direction_component_difference\": "
         << parentCheck.maximumDirectionComponentDifference << "},\n";
    json << "  \"residency_trace\": {\n";
    json << "    \"tile_bytes\": " << residency.tileBytes << ",\n";
    json << "    \"budgets\": {\"max_resident_tiles\": "
         << residency.budgets.max_resident_tiles << ", \"max_cpu_bytes\": "
         << residency.budgets.max_cpu_bytes << ", \"max_upload_bytes_per_frame\": "
         << residency.budgets.max_upload_bytes_per_frame
         << ", \"max_new_requests_per_frame\": "
         << residency.budgets.max_new_requests_per_frame << ", \"max_pending_tiles\": "
         << residency.budgets.max_pending_tiles << "},\n";
    json << "    \"frames\": [\n";
    for (std::size_t frameIndex = 0; frameIndex < residency.frames.size(); ++frameIndex) {
        ResidencyFrame const& frame = residency.frames[frameIndex];
        json << "      {\"frame\": " << frame.index
             << ", \"stats\": {\"resident_tiles\": " << frame.stats.resident_tiles
             << ", \"pending_tiles\": " << frame.stats.pending_tiles
             << ", \"resident_cpu_bytes\": " << frame.stats.resident_cpu_bytes
             << ", \"uploaded_bytes\": " << frame.stats.uploaded_bytes
             << ", \"request_budget_limited\": "
             << (frame.stats.request_budget_limited ? "true" : "false")
             << ", \"upload_budget_limited\": "
             << (frame.stats.upload_budget_limited ? "true" : "false")
             << ", \"resident_budget_limited\": "
             << (frame.stats.resident_budget_limited ? "true" : "false") << "}, ";

        auto writeKeys = [&](std::string_view name, std::vector<TileKey> const& keys) {
            json << '\"' << name << "\": [";
            for (std::size_t keyIndex = 0; keyIndex < keys.size(); ++keyIndex) {
                json << '\"' << jsonEscape(toString(keys[keyIndex])) << '\"';
                if (keyIndex + 1U != keys.size()) {
                    json << ", ";
                }
            }
            json << ']';
        };
        writeKeys("admitted", frame.admitted);
        json << ", ";
        writeKeys("uploaded", frame.uploaded);
        json << ", ";
        writeKeys("retired", frame.retired);
        json << ", \"records\": [";
        for (std::size_t recordIndex = 0; recordIndex < frame.records.size(); ++recordIndex) {
            ResidencyRecord const& record = frame.records[recordIndex];
            json << "{\"key\": \"" << jsonEscape(toString(record.key))
                 << "\", \"status\": \"" << toString(record.status)
                 << "\", \"priority\": " << record.priority
                 << ", \"cpu_bytes\": " << record.cpu_bytes << '}';
            if (recordIndex + 1U != frame.records.size()) {
                json << ", ";
            }
        }
        json << "]}";
        json << (frameIndex + 1U == residency.frames.size() ? "\n" : ",\n");
    }
    json << "    ],\n";
    json << "    \"stayed_within_bounds\": "
         << (residency.stayedWithinBounds ? "true" : "false") << ",\n";
    json << "    \"observed_request_throttle\": "
         << (residency.observedRequestThrottle ? "true" : "false") << ",\n";
    json << "    \"observed_upload_throttle\": "
         << (residency.observedUploadThrottle ? "true" : "false") << ",\n";
    json << "    \"observed_priority_eviction\": "
         << (residency.observedPriorityEviction ? "true" : "false") << ",\n";
    json << "    \"status\": \"" << (residency.passed ? "pass" : "fail") << "\"\n";
    json << "  },\n";
    json << "  \"outputs\": [\n";
    for (std::size_t index = 0; index < views.size(); ++index) {
        ImageView const& view = views[index];
        json << "    {\"file\": \""
             << jsonEscape((options.outputDirectory / view.filename).generic_string())
             << "\", \"description\": \"" << jsonEscape(view.description)
             << "\", \"width\": " << view.width << ", \"height\": " << view.height
             << ", \"rgb8_fnv1a64\": \"0x" << hex64(view.checksum) << "\"}";
        json << (index + 1U == views.size() ? "\n" : ",\n");
    }
    json << "  ],\n";
    json << "  \"performance_observation\": {\n";
    json << "    \"deterministic\": false,\n";
    json << "    \"terrain_generation_milliseconds\": " << generationMilliseconds << ",\n";
    json << "    \"image_and_validation_milliseconds\": " << outputMilliseconds << "\n";
    json << "  },\n";
    json << "  \"validation\": {\n";
    json << "    \"finite_samples\": " << (metrics.finite ? "true" : "false") << ",\n";
    json << "    \"normals_outward\": "
         << (metrics.minimumOutwardDot > 0.0 ? "true" : "false") << ",\n";
    json << "    \"normal_unit_tolerance\": 1e-9,\n";
    json << "    \"normals_unit_length\": "
         << (metrics.maximumNormalLengthError <= 1.0e-9 ? "true" : "false") << ",\n";
    json << "    \"sampled_residual_inside_analytic_bound\": "
         << (metrics.absoluteResidual.maximum <= tile.parent_relative_geometric_error_m
                 ? "true"
                 : "false")
         << ",\n";
    json << "    \"status\": \"" << (validationPassed ? "pass" : "fail") << "\"\n";
    json << "  }\n";
    json << "}\n";
    return json.str();
}

template <typename T>
[[nodiscard]] std::optional<T> parseDecimal(std::string_view text) {
    T value{};
    auto const [end, status] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (status != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<std::uint64_t> parseSeed(std::string_view text) {
    int base = 10;
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.remove_prefix(2U);
        base = 16;
    }
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    auto const [end, status] =
        std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (status != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<CubeFace> parseFace(std::string_view text) {
    std::string lowered{text};
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](char character) {
        if (character >= 'A' && character <= 'Z') {
            return static_cast<char>(character - 'A' + 'a');
        }
        return character;
    });
    if (lowered == "+x" || lowered == "px" || lowered == "positive-x" || lowered == "0") {
        return CubeFace::PositiveX;
    }
    if (lowered == "-x" || lowered == "nx" || lowered == "negative-x" || lowered == "1") {
        return CubeFace::NegativeX;
    }
    if (lowered == "+y" || lowered == "py" || lowered == "positive-y" || lowered == "2") {
        return CubeFace::PositiveY;
    }
    if (lowered == "-y" || lowered == "ny" || lowered == "negative-y" || lowered == "3") {
        return CubeFace::NegativeY;
    }
    if (lowered == "+z" || lowered == "pz" || lowered == "positive-z" || lowered == "4") {
        return CubeFace::PositiveZ;
    }
    if (lowered == "-z" || lowered == "nz" || lowered == "negative-z" || lowered == "5") {
        return CubeFace::NegativeZ;
    }
    return std::nullopt;
}

[[nodiscard]] bool consumeValue(int& index, int argc, char** argv, std::string_view option,
                                std::string_view& value, std::string& error) {
    const std::string_view argument = argv[index];
    const std::string prefix = std::string(option) + '=';
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

        std::string_view value{};
        if (argument == "--output-dir" || argument.starts_with("--output-dir=")) {
            if (!consumeValue(index, argc, argv, "--output-dir", value, error)) {
                return std::nullopt;
            }
            if (value.empty()) {
                error = "--output-dir must not be empty";
                return std::nullopt;
            }
            options.outputDirectory = fs::path{value};
        } else if (argument == "--face" || argument.starts_with("--face=")) {
            if (!consumeValue(index, argc, argv, "--face", value, error)) {
                return std::nullopt;
            }
            const auto parsed = parseFace(value);
            if (!parsed) {
                error = "--face must be one of +X, -X, +Y, -Y, +Z, -Z";
                return std::nullopt;
            }
            options.key.face = *parsed;
        } else if (argument == "--level" || argument.starts_with("--level=")) {
            if (!consumeValue(index, argc, argv, "--level", value, error)) {
                return std::nullopt;
            }
            const auto parsed = parseDecimal<std::uint32_t>(value);
            if (!parsed || *parsed > kMaxTileLevel) {
                error = "--level must be in [0, 30]";
                return std::nullopt;
            }
            options.key.level = static_cast<std::uint8_t>(*parsed);
        } else if (argument == "--x" || argument.starts_with("--x=")) {
            if (!consumeValue(index, argc, argv, "--x", value, error)) {
                return std::nullopt;
            }
            const auto parsed = parseDecimal<std::uint32_t>(value);
            if (!parsed) {
                error = "--x must be an unsigned integer";
                return std::nullopt;
            }
            options.key.x = *parsed;
            options.xExplicit = true;
        } else if (argument == "--y" || argument.starts_with("--y=")) {
            if (!consumeValue(index, argc, argv, "--y", value, error)) {
                return std::nullopt;
            }
            const auto parsed = parseDecimal<std::uint32_t>(value);
            if (!parsed) {
                error = "--y must be an unsigned integer";
                return std::nullopt;
            }
            options.key.y = *parsed;
            options.yExplicit = true;
        } else if (argument == "--seed" || argument.starts_with("--seed=")) {
            if (!consumeValue(index, argc, argv, "--seed", value, error)) {
                return std::nullopt;
            }
            const auto parsed = parseSeed(value);
            if (!parsed) {
                error = "--seed must be a decimal or 0x-prefixed 64-bit unsigned integer";
                return std::nullopt;
            }
            options.generator.seed = *parsed;
        } else if (argument == "--amplitude" || argument.starts_with("--amplitude=")) {
            if (!consumeValue(index, argc, argv, "--amplitude", value, error)) {
                return std::nullopt;
            }
            const auto parsed = parseDecimal<double>(value);
            if (!parsed || !std::isfinite(*parsed) || *parsed <= 0.0) {
                error = "--amplitude must be a finite positive number of metres";
                return std::nullopt;
            }
            options.generator.height_amplitude_m = *parsed;
        } else if (argument == "--root-octaves" || argument.starts_with("--root-octaves=")) {
            if (!consumeValue(index, argc, argv, "--root-octaves", value, error)) {
                return std::nullopt;
            }
            const auto parsed = parseDecimal<std::uint32_t>(value);
            if (!parsed || *parsed < 1U || *parsed > kTerrainMaximumOctaves) {
                error = "--root-octaves must be in [1, 24]";
                return std::nullopt;
            }
            options.generator.root_octaves = static_cast<std::uint8_t>(*parsed);
        } else if (argument == "--octaves" || argument.starts_with("--octaves=") ||
                   argument == "--max-octaves" || argument.starts_with("--max-octaves=")) {
            const std::string_view option = argument.starts_with("--max-octaves")
                                                ? "--max-octaves"
                                                : "--octaves";
            if (!consumeValue(index, argc, argv, option, value, error)) {
                return std::nullopt;
            }
            const auto parsed = parseDecimal<std::uint32_t>(value);
            if (!parsed || *parsed < 1U || *parsed > kTerrainMaximumOctaves) {
                error = "--octaves must be in [1, 24]";
                return std::nullopt;
            }
            options.generator.max_octaves = static_cast<std::uint8_t>(*parsed);
        } else if (argument == "--base-frequency" || argument.starts_with("--base-frequency=")) {
            if (!consumeValue(index, argc, argv, "--base-frequency", value, error)) {
                return std::nullopt;
            }
            const auto parsed = parseDecimal<double>(value);
            if (!parsed || !std::isfinite(*parsed) || *parsed <= 0.0) {
                error = "--base-frequency must be finite and positive";
                return std::nullopt;
            }
            options.generator.base_frequency = *parsed;
        } else if (argument == "--lacunarity" || argument.starts_with("--lacunarity=")) {
            if (!consumeValue(index, argc, argv, "--lacunarity", value, error)) {
                return std::nullopt;
            }
            const auto parsed = parseDecimal<double>(value);
            if (!parsed || !std::isfinite(*parsed) || *parsed <= 1.0) {
                error = "--lacunarity must be finite and greater than 1";
                return std::nullopt;
            }
            options.generator.lacunarity = *parsed;
        } else if (argument == "--persistence" || argument.starts_with("--persistence=")) {
            if (!consumeValue(index, argc, argv, "--persistence", value, error)) {
                return std::nullopt;
            }
            const auto parsed = parseDecimal<double>(value);
            if (!parsed || !std::isfinite(*parsed) || *parsed <= 0.0 || *parsed >= 1.0) {
                error = "--persistence must be finite and in (0, 1)";
                return std::nullopt;
            }
            options.generator.persistence = *parsed;
        } else if (argument == "--scale" || argument.starts_with("--scale=")) {
            if (!consumeValue(index, argc, argv, "--scale", value, error)) {
                return std::nullopt;
            }
            const auto parsed = parseDecimal<std::uint32_t>(value);
            if (!parsed || *parsed < 1U || *parsed > 32U) {
                error = "--scale must be in [1, 32]";
                return std::nullopt;
            }
            options.scale = *parsed;
        } else {
            error = "unknown option: " + std::string(argument);
            return std::nullopt;
        }
    }

    const std::uint64_t side = std::uint64_t{1} << options.key.level;
    if (!options.xExplicit) {
        options.key.x = static_cast<std::uint32_t>(side / 2U);
    }
    if (!options.yExplicit) {
        options.key.y = static_cast<std::uint32_t>(side / 2U);
    }
    if (!isValid(options.key)) {
        error = "tile coordinates must be smaller than 2^level";
        return std::nullopt;
    }
    auto validGenerator = validate(options.generator);
    if (validGenerator.failed()) {
        error = validGenerator.error().to_string();
        return std::nullopt;
    }
    return options;
}

void printHelp() {
    std::cout
        << "Helios TerrainLab v0.0.1 (headless)\n\n"
        << "Usage: helios_terrain_lab [options]\n\n"
        << "  --output-dir PATH    Artifact directory (default terrain_lab_output)\n"
        << "  --face FACE          +X, -X, +Y, -Y, +Z, or -Z (default +Z)\n"
        << "  --level N            Quadtree level in [0,30] (default 4)\n"
        << "  --x N --y N          Tile coordinates; default to the face centre\n"
        << "  --seed N             Decimal or 0x-prefixed deterministic seed\n"
        << "  --amplitude METRES   Height amplitude (default 9000)\n"
        << "  --root-octaves N     Octaves present at level zero (default 4)\n"
        << "  --octaves N          Maximum octave count, alias --max-octaves (default 16)\n"
        << "  --base-frequency X   Base direction-space noise frequency (default 1.5)\n"
        << "  --lacunarity X       Frequency multiplier above 1 (default 2)\n"
        << "  --persistence X      Amplitude multiplier in (0,1) (default 0.5)\n"
        << "  --scale N            Nearest-neighbour view scale in [1,32] (default 8)\n"
        << "  --help               Show this help\n\n"
        << "Outputs: height, normal, slope, parent_height, residual, and error PPMs plus "
           "report.json. No window or GPU is opened.\n";
}

} // namespace

int main(int argc, char** argv) {
    std::string argumentError{};
    const std::optional<Options> parsed = parseOptions(argc, argv, argumentError);
    if (!parsed) {
        std::cerr << "TerrainLab: " << argumentError << '\n';
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
        std::cerr << "TerrainLab: cannot create output directory: "
                  << directoryError.message() << '\n';
        return 2;
    }

    auto generatorResult = TerrainGenerator::create(options.generator);
    if (generatorResult.failed()) {
        std::cerr << "TerrainLab: " << generatorResult.error().to_string() << '\n';
        return 2;
    }
    TerrainGenerator generator = std::move(generatorResult.value());

    std::cout << "TerrainLab: generating " << toString(options.key) << " with seed 0x"
              << hex64(options.generator.seed) << '\n';
    const auto generationStart = std::chrono::steady_clock::now();
    auto tileResult = generator.generate(options.key);
    if (tileResult.failed()) {
        std::cerr << "TerrainLab: " << tileResult.error().to_string() << '\n';
        return 2;
    }
    TerrainTile tile = std::move(tileResult.value());
    auto regenerationResult = generator.generate(options.key);
    if (regenerationResult.failed()) {
        std::cerr << "TerrainLab: regeneration failed: "
                  << regenerationResult.error().to_string() << '\n';
        return 2;
    }
    const auto generationEnd = std::chrono::steady_clock::now();
    const double generationMilliseconds =
        std::chrono::duration<double, std::milli>(generationEnd - generationStart).count();

    const bool stableHashValid = stableTerrainContentHash(tile) == tile.content_hash;
    const bool regenerationMatches =
        regenerationResult.value().content_hash == tile.content_hash;
    TileMetrics const metrics = measureTile(tile);

    const auto outputStart = std::chrono::steady_clock::now();
    std::vector<ImageView> views = buildViews(tile, metrics, options.scale);
    for (ImageView const& view : views) {
        const fs::path path = options.outputDirectory / view.filename;
        if (!writePpm(path, view)) {
            std::cerr << "TerrainLab: failed to write " << path << '\n';
            return 2;
        }
    }

    std::vector<SeamCheck> seams{};
    std::string validationError{};
    if (!buildSeamChecks(generator, tile, seams, validationError)) {
        std::cerr << "TerrainLab: seam validation failed to run: " << validationError << '\n';
        return 2;
    }
    ParentCheck parentCheck{};
    if (!buildParentCheck(generator, tile, parentCheck, validationError)) {
        std::cerr << "TerrainLab: parent validation failed to run: " << validationError << '\n';
        return 2;
    }
    ResidencyTrace residency{};
    const std::uint64_t tileBytes = static_cast<std::uint64_t>(tile.samples.size()) *
                                    static_cast<std::uint64_t>(sizeof(TerrainSample));
    if (!runResidencyTrace(tile.key, tileBytes, residency, validationError)) {
        std::cerr << "TerrainLab: residency trace failed to run: " << validationError << '\n';
        return 2;
    }

    bool seamsPassed = true;
    for (SeamCheck const& seam : seams) {
        seamsPassed = seamsPassed && (!seam.applicable || seam.passed);
    }
    const bool residualInsideBound =
        metrics.absoluteResidual.maximum <= tile.parent_relative_geometric_error_m;
    const bool validationPassed = stableHashValid && regenerationMatches && metrics.finite &&
                                  metrics.minimumOutwardDot > 0.0 &&
                                  metrics.maximumNormalLengthError <= 1.0e-9 &&
                                  residualInsideBound && seamsPassed && parentCheck.passed &&
                                  residency.passed;

    std::uint64_t bundleChecksum = 14695981039346656037ULL;
    for (ImageView const& view : views) {
        bundleChecksum = hashText(view.filename, bundleChecksum);
        bundleChecksum = hashBytes(view.rgb, bundleChecksum);
    }
    const auto outputEnd = std::chrono::steady_clock::now();
    const double outputMilliseconds =
        std::chrono::duration<double, std::milli>(outputEnd - outputStart).count();

    const std::string report =
        makeReport(options, tile, metrics, views, seams, parentCheck, residency,
                   stableHashValid, regenerationMatches, bundleChecksum,
                   generationMilliseconds, outputMilliseconds, validationPassed);
    const fs::path reportPath = options.outputDirectory / "report.json";
    {
        std::ofstream reportFile(reportPath, std::ios::binary | std::ios::trunc);
        reportFile.write(report.data(), static_cast<std::streamsize>(report.size()));
        if (!reportFile) {
            std::cerr << "TerrainLab: failed to write " << reportPath << '\n';
            return 2;
        }
    }

    std::cout << std::fixed << std::setprecision(3)
              << "TerrainLab: generated 2 deterministic tiles in "
              << generationMilliseconds << " ms\n"
              << "  height range=" << metrics.height.minimum << " .. "
              << metrics.height.maximum << " m\n"
              << "  max child-parent residual=" << metrics.absoluteResidual.maximum
              << " m (analytic bound " << tile.parent_relative_geometric_error_m << " m)\n"
              << "  max slope=" << metrics.slopeDegrees.maximum << " deg\n"
              << "  content fnv1a64=0x" << hex64(tile.content_hash) << '\n'
              << "  view bundle fnv1a64=0x" << hex64(bundleChecksum) << '\n'
              << "  report=" << reportPath << '\n'
              << "  validation=" << (validationPassed ? "PASS" : "FAIL") << '\n';
    return validationPassed ? 0 : 3;
}
