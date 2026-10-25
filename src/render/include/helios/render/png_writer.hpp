// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Minimal dependency-free PNG encoder (RGBA8, store-mode zlib) plus a tiny
// 4x6 bitmap font for deterministic diagnostic overlays. Used by the M1 GPU
// precision lab to produce reproducible captures; not a general image
// library.
#pragma once

#include "helios/core/error.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace helios::render {

struct RgbaImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels; // RGBA8, tight rows, top-left origin
};

// Encodes and writes an 8-bit RGBA PNG (zlib stored blocks — valid, large,
// fully deterministic).
[[nodiscard]] core::Result<void> writePngRgba(std::string_view path, RgbaImage const& image);

// Draws one line of uppercase text with the embedded 4x6 font. Characters
// outside the glyph set render as '?'.
void drawOverlayText(RgbaImage& image, int x, int y, std::string_view text,
                     std::uint8_t r, std::uint8_t g, std::uint8_t b);

} // namespace helios::render
