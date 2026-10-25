// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Minimal power-of-two complex FFT for the deep-sky lab's simulations: the
// particle-mesh gravity of the galaxy and the random fields that seed
// turbulence.  Iterative radix-2 with precomputed twiddles; deterministic.
#pragma once

#include "worker_pool.hpp"

#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace helios::examples {

class Fft {
  public:
    // `size` must be a power of two.
    explicit Fft(std::size_t size);

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    // In-place transform of `size` values spaced `stride` apart.  The inverse
    // is unnormalised (divide by size yourself).
    void transform(std::complex<float>* data, std::size_t stride, bool inverse) const;

  private:
    std::size_t size_ = 0;
    std::vector<std::uint32_t> reversed_;
    std::vector<std::complex<float>> twiddles_; // exp(-2 pi i k / size), k < size / 2
};

// In-place 2-D transform of a row-major size x size array.
void transform2d(Fft const& fft, std::vector<std::complex<float>>& data, bool inverse,
                 WorkerPool& pool);

// The same for a zero-padded convolution, skipping work whose result is known
// or unused.  Forward: only the first `rows` rows may be non-zero on input.
// Inverse: only rows [0, rows) and the last row are needed on output (the
// others are left partially transformed).
void transform2dPadded(Fft const& fft, std::vector<std::complex<float>>& data, bool inverse,
                       std::size_t rows, WorkerPool& pool);

// In-place 3-D transform of a size^3 array indexed (z * size + y) * size + x.
void transform3d(Fft const& fft, std::vector<std::complex<float>>& data, bool inverse,
                 WorkerPool& pool);

} // namespace helios::examples
