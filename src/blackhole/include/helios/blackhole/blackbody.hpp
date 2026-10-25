// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Black-body colourimetry for the black-hole labs.
//
// Planck's spectral radiance is integrated against the CIE 1931 2-degree
// colour-matching functions (Wyman, Sloan & Shirley 2013 multi-lobe fit) and
// converted to linear Rec.709/sRGB primaries with a D65 white point.  The
// result is an absolute quantity: `luminance` is in cd/m^2, so brightness
// ratios between two temperatures are physical rather than tuned.
//
// The CPU reference lab and the real-time shader both read the same table
// produced by makeBlackbodyTable(), so they cannot drift apart.
#pragma once

#include "helios/math/vec.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace helios::blackhole {

struct BlackbodyColor final {
    // CIE 1931 tristimulus values of the Planck spectral radiance (W sr^-1 m^-2).
    math::Vec3d xyz{};

    // Linear Rec.709/sRGB primaries, D65.  Components may be negative for
    // temperatures whose chromaticity lies outside the sRGB gamut.
    math::Vec3d linearSrgb{};

    // Photometric luminance in cd/m^2 (683.002 lm/W times Y).
    double luminance = 0.0;
};

// Returns zeros for non-finite or non-positive temperatures.
[[nodiscard]] BlackbodyColor blackbodyColor(double kelvin) noexcept;

// Linear Rec.709/sRGB response to unit radiance at a single wavelength (an
// emission line), from the same colour-matching fit.  Monochromatic light is
// outside the sRGB gamut, so components may be negative; callers add lines
// together first and clip the sum.  Zero outside the fit's 360-830 nm support.
[[nodiscard]] math::Vec3d monochromaticLinearSrgb(double wavelengthNm) noexcept;

// Log-spaced lookup table.  Each texel stores a gamut-clipped linear sRGB
// colour normalised to unit luminance in rgb, and ln(luminance / (cd/m^2)) in
// alpha; storing the logarithm keeps forty decades of brightness inside one
// float32 texture.
struct BlackbodyTable final {
    double minKelvin = 0.0;
    double maxKelvin = 0.0;
    std::uint32_t count = 0;
    std::vector<float> rgba{}; // count * 4

    [[nodiscard]] bool valid() const noexcept {
        return count >= 2 && rgba.size() == static_cast<std::size_t>(count) * 4U && minKelvin > 0.0 &&
               maxKelvin > minKelvin;
    }
};

// Throws nothing; returns an invalid (empty) table when the arguments are not
// finite, not ordered, or `count` is outside [2, 65536].
[[nodiscard]] BlackbodyTable makeBlackbodyTable(double minKelvin, double maxKelvin, std::uint32_t count);

// Evaluates the table exactly as the shader does: linear interpolation in
// ln(T), zero below the table, and a Rayleigh-Jeans continuation (luminance
// proportional to T at fixed chromaticity) above it.  The returned colour is
// scaled so that a luminance of exp(logReferenceLuminance) maps to 1.
[[nodiscard]] math::Vec3d sampleBlackbodyTable(BlackbodyTable const& table, double kelvin,
                                               double logReferenceLuminance) noexcept;

// ln(luminance) from the table with the same interpolation rules, or negative
// infinity below the table.
[[nodiscard]] double sampleBlackbodyLogLuminance(BlackbodyTable const& table, double kelvin) noexcept;

} // namespace helios::blackhole
