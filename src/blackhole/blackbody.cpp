// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors

#include "helios/blackhole/blackbody.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace helios::blackhole {
namespace {

// CODATA 2018 exact values.
constexpr double kFirstRadiationConstant = 1.191042972e-16; // 2 h c^2, W m^2 sr^-1
constexpr double kSecondRadiationConstant = 1.438776877e-2; // h c / k, m K
constexpr double kLuminousEfficacy = 683.002;               // lm/W at 555 nm

// Rec.709 luminance weights of the linear sRGB primaries.
constexpr double kLumaR = 0.2126729;
constexpr double kLumaG = 0.7151522;
constexpr double kLumaB = 0.0721750;

[[nodiscard]] double lobe(double wavelengthNm, double centre, double inverseBelow,
                          double inverseAbove) noexcept {
    const double t = (wavelengthNm - centre) * (wavelengthNm < centre ? inverseBelow : inverseAbove);
    return std::exp(-0.5 * t * t);
}

// Wyman, Sloan & Shirley, "Simple Analytic Approximations to the CIE XYZ
// Color Matching Functions", JCGT 2(2), 2013 - the multi-lobe 1931 fit.
[[nodiscard]] math::Vec3d colourMatching1931(double wavelengthNm) noexcept {
    const double x = 0.362 * lobe(wavelengthNm, 442.0, 0.0624, 0.0374) +
                     1.056 * lobe(wavelengthNm, 599.8, 0.0264, 0.0323) -
                     0.065 * lobe(wavelengthNm, 501.1, 0.0490, 0.0382);
    const double y =
        0.821 * lobe(wavelengthNm, 568.8, 0.0213, 0.0247) + 0.286 * lobe(wavelengthNm, 530.9, 0.0613, 0.0322);
    const double z =
        1.217 * lobe(wavelengthNm, 437.0, 0.0845, 0.0278) + 0.681 * lobe(wavelengthNm, 459.0, 0.0385, 0.0725);
    return math::Vec3d{x, y, z};
}

// Planck spectral radiance per unit wavelength, W sr^-1 m^-3.
[[nodiscard]] double planck(double wavelengthM, double kelvin) noexcept {
    const double exponent = kSecondRadiationConstant / (wavelengthM * kelvin);
    if (exponent > 700.0) {
        return 0.0;
    }
    const double wavelength2 = wavelengthM * wavelengthM;
    return kFirstRadiationConstant / (wavelength2 * wavelength2 * wavelengthM * std::expm1(exponent));
}

[[nodiscard]] math::Vec3d xyzToLinearSrgb(math::Vec3d const& xyz) noexcept {
    return math::Vec3d{3.2404542 * xyz.v[0] - 1.5371385 * xyz.v[1] - 0.4985314 * xyz.v[2],
                       -0.9692660 * xyz.v[0] + 1.8760108 * xyz.v[1] + 0.0415560 * xyz.v[2],
                       0.0556434 * xyz.v[0] - 0.2040259 * xyz.v[1] + 1.0572252 * xyz.v[2]};
}

struct TableLookup final {
    bool belowTable = true;
    std::size_t index = 0;
    double fraction = 0.0;
    double logLuminanceOffset = 0.0;
};

[[nodiscard]] TableLookup locate(BlackbodyTable const& table, double kelvin) noexcept {
    TableLookup lookup{};
    if (!table.valid() || !std::isfinite(kelvin) || kelvin < table.minKelvin) {
        return lookup;
    }
    const double logMin = std::log(table.minKelvin);
    const double logRange = std::log(table.maxKelvin) - logMin;
    const double unit = (std::log(kelvin) - logMin) / logRange;
    const double scaled = std::clamp(unit, 0.0, 1.0) * static_cast<double>(table.count - 1U);
    const double base = std::min(std::floor(scaled), static_cast<double>(table.count - 2U));
    lookup.belowTable = false;
    lookup.index = static_cast<std::size_t>(base);
    lookup.fraction = scaled - base;
    lookup.logLuminanceOffset = std::max(unit - 1.0, 0.0) * logRange;
    return lookup;
}

} // namespace

BlackbodyColor blackbodyColor(double kelvin) noexcept {
    BlackbodyColor result{};
    if (!std::isfinite(kelvin) || kelvin <= 0.0) {
        return result;
    }

    // Composite Simpson rule over the support of the colour-matching fit.
    constexpr double kFirstNm = 360.0;
    constexpr double kLastNm = 830.0;
    constexpr int kIntervals = 470; // 1 nm; must be even
    constexpr double kStepM = (kLastNm - kFirstNm) / kIntervals * 1.0e-9;
    math::Vec3d sum{};
    for (int sample = 0; sample <= kIntervals; ++sample) {
        const double wavelengthNm = kFirstNm + static_cast<double>(sample);
        const double weight = (sample == 0 || sample == kIntervals) ? 1.0 : ((sample & 1) != 0 ? 4.0 : 2.0);
        sum += colourMatching1931(wavelengthNm) * (weight * planck(wavelengthNm * 1.0e-9, kelvin));
    }
    result.xyz = sum * (kStepM / 3.0);
    result.linearSrgb = xyzToLinearSrgb(result.xyz);
    result.luminance = kLuminousEfficacy * result.xyz.v[1];
    return result;
}

math::Vec3d monochromaticLinearSrgb(double wavelengthNm) noexcept {
    if (!std::isfinite(wavelengthNm) || wavelengthNm < 360.0 || wavelengthNm > 830.0) {
        return {};
    }
    return xyzToLinearSrgb(colourMatching1931(wavelengthNm));
}

BlackbodyTable makeBlackbodyTable(double minKelvin, double maxKelvin, std::uint32_t count) {
    BlackbodyTable table{};
    if (!std::isfinite(minKelvin) || !std::isfinite(maxKelvin) || minKelvin <= 0.0 ||
        maxKelvin <= minKelvin || count < 2U || count > 65536U) {
        return table;
    }
    table.minKelvin = minKelvin;
    table.maxKelvin = maxKelvin;
    table.count = count;
    table.rgba.resize(static_cast<std::size_t>(count) * 4U);

    const double logMin = std::log(minKelvin);
    const double logStep = (std::log(maxKelvin) - logMin) / static_cast<double>(count - 1U);
    for (std::uint32_t index = 0; index < count; ++index) {
        const double kelvin = std::exp(logMin + logStep * static_cast<double>(index));
        const BlackbodyColor color = blackbodyColor(kelvin);

        // Clip to the sRGB gamut, then renormalise so the clipped colour still
        // carries exactly one unit of luminance.
        const double red = std::max(color.linearSrgb.v[0], 0.0);
        const double green = std::max(color.linearSrgb.v[1], 0.0);
        const double blue = std::max(color.linearSrgb.v[2], 0.0);
        const double clippedLuma = kLumaR * red + kLumaG * green + kLumaB * blue;
        const double scale = clippedLuma > 0.0 ? 1.0 / clippedLuma : 0.0;

        const std::size_t offset = static_cast<std::size_t>(index) * 4U;
        table.rgba[offset + 0U] = static_cast<float>(red * scale);
        table.rgba[offset + 1U] = static_cast<float>(green * scale);
        table.rgba[offset + 2U] = static_cast<float>(blue * scale);
        table.rgba[offset + 3U] = static_cast<float>(
            color.luminance > 0.0 ? std::log(color.luminance)
                                  : static_cast<double>(std::numeric_limits<float>::lowest()));
    }
    return table;
}

double sampleBlackbodyLogLuminance(BlackbodyTable const& table, double kelvin) noexcept {
    const TableLookup lookup = locate(table, kelvin);
    if (lookup.belowTable) {
        return -std::numeric_limits<double>::infinity();
    }
    const std::size_t first = lookup.index * 4U + 3U;
    const double low = static_cast<double>(table.rgba[first]);
    const double high = static_cast<double>(table.rgba[first + 4U]);
    return low + (high - low) * lookup.fraction + lookup.logLuminanceOffset;
}

math::Vec3d sampleBlackbodyTable(BlackbodyTable const& table, double kelvin,
                                 double logReferenceLuminance) noexcept {
    const TableLookup lookup = locate(table, kelvin);
    if (lookup.belowTable) {
        return {};
    }
    const std::size_t first = lookup.index * 4U;
    auto channel = [&](std::size_t component) {
        const double low = static_cast<double>(table.rgba[first + component]);
        const double high = static_cast<double>(table.rgba[first + 4U + component]);
        return low + (high - low) * lookup.fraction;
    };
    const double scale = std::exp(channel(3U) + lookup.logLuminanceOffset - logReferenceLuminance);
    return math::Vec3d{channel(0U) * scale, channel(1U) * scale, channel(2U) * scale};
}

} // namespace helios::blackhole
