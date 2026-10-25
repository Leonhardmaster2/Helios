// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "fft.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace helios::examples {

Fft::Fft(std::size_t size) : size_(size) {
    std::uint32_t bits = 0;
    while ((std::size_t{1} << bits) < size) {
        ++bits;
    }
    reversed_.resize(size);
    for (std::size_t index = 0; index < size; ++index) {
        std::uint32_t reversed = 0;
        for (std::uint32_t bit = 0; bit < bits; ++bit) {
            if ((index >> bit) & 1U) {
                reversed |= 1U << (bits - 1U - bit);
            }
        }
        reversed_[index] = reversed;
    }
    twiddles_.resize(size / 2);
    for (std::size_t k = 0; k < size / 2; ++k) {
        double const angle = -2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(size);
        twiddles_[k] = {static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
    }
}

void Fft::transform(std::complex<float>* data, std::size_t stride, bool inverse) const {
    for (std::size_t index = 0; index < size_; ++index) {
        std::size_t const other = reversed_[index];
        if (index < other) {
            std::swap(data[index * stride], data[other * stride]);
        }
    }
    // Butterflies on the real and imaginary parts directly (std::complex
    // arithmetic is not always inlined), one twiddle factor at a time.
    float const sign = inverse ? -1.0f : 1.0f;
    for (std::size_t half = 1; half < size_; half *= 2) {
        std::size_t const twiddle_step = size_ / (2 * half);
        for (std::size_t k = 0; k < half; ++k) {
            float const wr = twiddles_[k * twiddle_step].real();
            float const wi = sign * twiddles_[k * twiddle_step].imag();
            for (std::size_t block = 0; block < size_; block += 2 * half) {
                auto* const a = reinterpret_cast<float*>(data + (block + k) * stride);
                auto* const b = reinterpret_cast<float*>(data + (block + k + half) * stride);
                float const tr = b[0] * wr - b[1] * wi;
                float const ti = b[0] * wi + b[1] * wr;
                b[0] = a[0] - tr;
                b[1] = a[1] - ti;
                a[0] += tr;
                a[1] += ti;
            }
        }
    }
}

namespace {

// Transforms `count` values spaced `stride` apart through a contiguous copy:
// a strided pencil of a large array would otherwise miss the cache on every
// butterfly of every stage.
void transformStrided(Fft const& fft, std::complex<float>* data, std::size_t stride, bool inverse) {
    thread_local std::vector<std::complex<float>> scratch;
    std::size_t const n = fft.size();
    scratch.resize(n);
    for (std::size_t index = 0; index < n; ++index) {
        scratch[index] = data[index * stride];
    }
    fft.transform(scratch.data(), 1, inverse);
    for (std::size_t index = 0; index < n; ++index) {
        data[index * stride] = scratch[index];
    }
}

// Columns are transformed eight at a time: one cache line of a row holds
// eight complex floats, so the copy in and out reads and writes whole lines.
constexpr std::size_t kColumnBlock = 8;

void transformColumns(Fft const& fft, std::complex<float>* data, std::size_t first, bool inverse) {
    thread_local std::vector<std::complex<float>> scratch;
    std::size_t const n = fft.size();
    scratch.resize(n * kColumnBlock);
    for (std::size_t row = 0; row < n; ++row) {
        std::complex<float> const* const source = data + row * n + first;
        for (std::size_t column = 0; column < kColumnBlock; ++column) {
            scratch[column * n + row] = source[column];
        }
    }
    for (std::size_t column = 0; column < kColumnBlock; ++column) {
        fft.transform(scratch.data() + column * n, 1, inverse);
    }
    for (std::size_t row = 0; row < n; ++row) {
        std::complex<float>* const target = data + row * n + first;
        for (std::size_t column = 0; column < kColumnBlock; ++column) {
            target[column] = scratch[column * n + row];
        }
    }
}

void columnPass(Fft const& fft, std::vector<std::complex<float>>& data, bool inverse, WorkerPool& pool) {
    std::size_t const n = fft.size();
    if (n % kColumnBlock != 0) {
        pool.run(n, [&](std::size_t column) { transformStrided(fft, data.data() + column, n, inverse); });
        return;
    }
    pool.run(n / kColumnBlock, [&](std::size_t block) {
        transformColumns(fft, data.data(), block * kColumnBlock, inverse);
    });
}

} // namespace

void transform2d(Fft const& fft, std::vector<std::complex<float>>& data, bool inverse,
                 WorkerPool& pool) {
    std::size_t const n = fft.size();
    pool.run(n, [&](std::size_t row) { fft.transform(data.data() + row * n, 1, inverse); });
    columnPass(fft, data, inverse, pool);
}

void transform2dPadded(Fft const& fft, std::vector<std::complex<float>>& data, bool inverse,
                       std::size_t rows, WorkerPool& pool) {
    std::size_t const n = fft.size();
    rows = std::min(rows, n);
    if (!inverse) {
        // Rows beyond `rows` are zero and stay zero under a row transform.
        pool.run(rows, [&](std::size_t row) { fft.transform(data.data() + row * n, 1, false); });
        columnPass(fft, data, false, pool);
        return;
    }
    // Columns first; then only the rows that are read.
    columnPass(fft, data, true, pool);
    std::size_t const count = rows + 1U < n ? rows + 1U : n;
    pool.run(count, [&](std::size_t index) {
        std::size_t const row = index < rows ? index : n - 1U;
        fft.transform(data.data() + row * n, 1, true);
    });
}

void transform3d(Fft const& fft, std::vector<std::complex<float>>& data, bool inverse,
                 WorkerPool& pool) {
    std::size_t const n = fft.size();
    // Along x, then y, then z; every pencil is independent of the others.
    pool.run(n * n, [&](std::size_t line) { fft.transform(data.data() + line * n, 1, inverse); });
    pool.run(n * n, [&](std::size_t line) {
        std::size_t const z = line / n;
        std::size_t const x = line % n;
        transformStrided(fft, data.data() + z * n * n + x, n, inverse);
    });
    pool.run(n * n, [&](std::size_t line) {
        transformStrided(fft, data.data() + line, n * n, inverse);
    });
}

} // namespace helios::examples
