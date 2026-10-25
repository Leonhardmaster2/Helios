// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/render/png_writer.hpp"

#include "helios/core/io.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <vector>

namespace helios::render {
namespace {

// ---- CRC32 (PNG chunk checksums) ----
std::uint32_t crc32(std::uint8_t const* data, std::size_t size) {
    static std::uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            table[n] = c;
        }
        init = true;
    }
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

void be32(std::uint8_t* out, std::uint32_t v) {
    out[0] = static_cast<std::uint8_t>(v >> 24);
    out[1] = static_cast<std::uint8_t>(v >> 16);
    out[2] = static_cast<std::uint8_t>(v >> 8);
    out[3] = static_cast<std::uint8_t>(v);
}

struct ChunkWriter {
    std::vector<std::uint8_t> out;

    void bytes(std::uint8_t const* p, std::size_t n) {
        out.insert(out.end(), p, p + n);
    }
    void u32(std::uint32_t v) {
        std::uint8_t b[4];
        be32(b, v);
        bytes(b, 4);
    }
    void chunk(char const* type, std::vector<std::uint8_t> const& payload) {
        u32(static_cast<std::uint32_t>(payload.size()));
        std::size_t start = out.size();
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<std::uint8_t>(type[i]));
        }
        bytes(payload.data(), payload.size());
        u32(crc32(out.data() + start, out.size() - start));
    }
};

// ---- Adler-32 (zlib trailer) ----
std::uint32_t adler32(std::uint8_t const* data, std::size_t size) {
    std::uint32_t a = 1, b = 0;
    for (std::size_t i = 0; i < size; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

// zlib stream with stored (uncompressed) deflate blocks.
std::vector<std::uint8_t> zlibStore(std::uint8_t const* data, std::size_t size) {
    std::vector<std::uint8_t> out;
    out.push_back(0x78);
    out.push_back(0x01); // CMF/FLG: fastest, no dict
    std::size_t off = 0;
    while (off < size || off == 0) {
        const std::size_t block = std::min<std::size_t>(65535, size - off);
        const bool last = (off + block >= size);
        out.push_back(last ? 1 : 0);
        out.push_back(static_cast<std::uint8_t>(block & 0xFF));
        out.push_back(static_cast<std::uint8_t>(block >> 8));
        out.push_back(static_cast<std::uint8_t>(~block & 0xFF));
        out.push_back(static_cast<std::uint8_t>((~block >> 8) & 0xFF));
        out.insert(out.end(), data + off, data + off + block);
        off += block;
        if (block == 0 && last) {
            break;
        }
        if (off >= size && last) {
            break;
        }
    }
    // Handle zero-size input edge case: emit one final empty stored block.
    if (size == 0) {
        out.resize(2);
        out.push_back(1);
        out.push_back(0);
        out.push_back(0);
        out.push_back(0xFF);
        out.push_back(0xFF);
    }
    std::uint8_t ad[4];
    be32(ad, adler32(data, size));
    out.insert(out.end(), ad, ad + 4);
    return out;
}

} // namespace

// ---- 4x6 bitmap font (columns packed as rows of 4 bits, MSB = leftmost) ----
namespace {

constexpr int kFontW = 4;
constexpr int kFontH = 6;

// Each glyph: 6 rows x 4-bit row patterns.
struct Glyph {
    char ch;
    std::uint8_t rows[kFontH];
};

constexpr Glyph kGlyphs[] = {
    {'A', {0b0110, 0b1001, 0b1111, 0b1001, 0b1001, 0b0000}},
    {'B', {0b1110, 0b1001, 0b1110, 0b1001, 0b1110, 0b0000}},
    {'C', {0b0111, 0b1000, 0b1000, 0b1000, 0b0111, 0b0000}},
    {'D', {0b1110, 0b1001, 0b1001, 0b1001, 0b1110, 0b0000}},
    {'E', {0b1111, 0b1000, 0b1110, 0b1000, 0b1111, 0b0000}},
    {'F', {0b1111, 0b1000, 0b1110, 0b1000, 0b1000, 0b0000}},
    {'G', {0b0111, 0b1000, 0b1011, 0b1001, 0b0111, 0b0000}},
    {'H', {0b1001, 0b1001, 0b1111, 0b1001, 0b1001, 0b0000}},
    {'I', {0b1110, 0b0100, 0b0100, 0b0100, 0b1110, 0b0000}},
    {'J', {0b0011, 0b0001, 0b0001, 0b1001, 0b0110, 0b0000}},
    {'K', {0b1001, 0b1010, 0b1100, 0b1010, 0b1001, 0b0000}},
    {'L', {0b1000, 0b1000, 0b1000, 0b1000, 0b1111, 0b0000}},
    {'M', {0b1001, 0b1111, 0b1111, 0b1001, 0b1001, 0b0000}},
    {'N', {0b1001, 0b1101, 0b1011, 0b1001, 0b1001, 0b0000}},
    {'O', {0b0110, 0b1001, 0b1001, 0b1001, 0b0110, 0b0000}},
    {'P', {0b1110, 0b1001, 0b1110, 0b1000, 0b1000, 0b0000}},
    {'Q', {0b0110, 0b1001, 0b1001, 0b1010, 0b0101, 0b0000}},
    {'R', {0b1110, 0b1001, 0b1110, 0b1010, 0b1001, 0b0000}},
    {'S', {0b0111, 0b1000, 0b0110, 0b0001, 0b1110, 0b0000}},
    {'T', {0b1110, 0b0100, 0b0100, 0b0100, 0b0100, 0b0000}},
    {'U', {0b1001, 0b1001, 0b1001, 0b1001, 0b0110, 0b0000}},
    {'V', {0b1001, 0b1001, 0b1001, 0b0110, 0b0110, 0b0000}},
    {'W', {0b1001, 0b1001, 0b1111, 0b1111, 0b1001, 0b0000}},
    {'X', {0b1001, 0b0110, 0b0110, 0b0110, 0b1001, 0b0000}},
    {'Y', {0b1001, 0b1001, 0b0110, 0b0100, 0b0100, 0b0000}},
    {'Z', {0b1111, 0b0001, 0b0110, 0b1000, 0b1111, 0b0000}},
    {'0', {0b0110, 0b1001, 0b1001, 0b1001, 0b0110, 0b0000}},
    {'1', {0b0010, 0b0110, 0b0010, 0b0010, 0b0111, 0b0000}},
    {'2', {0b0110, 0b1001, 0b0010, 0b0100, 0b1111, 0b0000}},
    {'3', {0b1110, 0b0001, 0b0110, 0b0001, 0b1110, 0b0000}},
    {'4', {0b1001, 0b1001, 0b1111, 0b0001, 0b0001, 0b0000}},
    {'5', {0b1111, 0b1000, 0b1110, 0b0001, 0b1110, 0b0000}},
    {'6', {0b0111, 0b1000, 0b1110, 0b1001, 0b0110, 0b0000}},
    {'7', {0b1111, 0b0001, 0b0010, 0b0100, 0b0100, 0b0000}},
    {'8', {0b0110, 0b1001, 0b0110, 0b1001, 0b0110, 0b0000}},
    {'9', {0b0110, 0b1001, 0b0111, 0b0001, 0b1110, 0b0000}},
    {' ', {0, 0, 0, 0, 0, 0}},
    {'.', {0, 0, 0, 0, 0b0100, 0b0000}},
    {':', {0, 0b0100, 0, 0b0100, 0, 0}},
    {'-', {0, 0, 0b1110, 0, 0, 0}},
    {'/', {0b0001, 0b0010, 0b0100, 0b1000, 0b0000, 0b0000}},
    {'(', {0b0010, 0b0100, 0b0100, 0b0100, 0b0010, 0b0000}},
    {')', {0b0100, 0b0010, 0b0010, 0b0010, 0b0100, 0b0000}},
    {'+', {0, 0b0100, 0b1110, 0b0100, 0, 0}},
};

Glyph const* findGlyph(char c) {
    const char upper = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    for (auto const& g : kGlyphs) {
        if (g.ch == upper) {
            return &g;
        }
    }
    return nullptr; // caller draws '?'
}

} // namespace

void drawOverlayText(RgbaImage& image, int x, int y, std::string_view text, std::uint8_t r,
                     std::uint8_t g, std::uint8_t b) {
    int cursor = x;
    for (char c : text) {
        const Glyph* glyph = findGlyph(c);
        const std::uint8_t unknown[6] = {0b1110, 0b0011, 0b0110, 0b1100, 0b1110, 0};
        for (int row = 0; row < kFontH; ++row) {
            const std::uint8_t bits = glyph ? glyph->rows[row] : unknown[row];
            for (int col = 0; col < kFontW; ++col) {
                if ((bits >> (kFontW - 1 - col)) & 1u) {
                    const int px = cursor + col;
                    const int py = y + row;
                    if (px < 0 || py < 0 || px >= static_cast<int>(image.width) ||
                        py >= static_cast<int>(image.height)) {
                        continue;
                    }
                    std::uint8_t* dst =
                        &image.pixels[(static_cast<size_t>(py) * image.width + px) * 4];
                    dst[0] = r;
                    dst[1] = g;
                    dst[2] = b;
                    dst[3] = 255;
                }
            }
        }
        cursor += kFontW + 1;
    }
}

core::Result<void> writePngRgba(std::string_view path, RgbaImage const& image) {
    if (image.width == 0 || image.height == 0 || image.pixels.size() !=
                                                      static_cast<size_t>(image.width) *
                                                          image.height * 4) {
        return core::makeError(core::ErrorCode::InvalidArgument, "png: invalid image dimensions");
    }

    // Raw scanlines with filter byte 0 (None).
    std::vector<std::uint8_t> raw;
    raw.reserve((static_cast<size_t>(image.width) * 4 + 1) * image.height);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), &image.pixels[static_cast<size_t>(y) * image.width * 4],
                   &image.pixels[(static_cast<size_t>(y) + 1) * image.width * 4]);
    }

    ChunkWriter w;
    // Signature.
    const std::uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    w.bytes(sig, 8);

    // IHDR.
    std::vector<std::uint8_t> ihdr(13);
    be32(ihdr.data(), image.width);
    be32(ihdr.data() + 4, image.height);
    ihdr[8] = 8;  // bit depth
    ihdr[9] = 6;  // RGBA
    ihdr[10] = 0; // deflate
    ihdr[11] = 0; // filter
    ihdr[12] = 0; // interlace
    w.chunk("IHDR", ihdr);

    // IDAT.
    w.chunk("IDAT", zlibStore(raw.data(), raw.size()));

    // IEND.
    w.chunk("IEND", {});

    std::ofstream file(std::string(path), std::ios::binary | std::ios::trunc);
    if (!file) {
        return core::makeError(core::ErrorCode::Io, "png: cannot open " + std::string(path));
    }
    file.write(reinterpret_cast<char const*>(w.out.data()),
               static_cast<std::streamsize>(w.out.size()));
    if (!file) {
        return core::makeError(core::ErrorCode::Io, "png: write failed for " + std::string(path));
    }
    return core::Result<void>{};
}

} // namespace helios::render
