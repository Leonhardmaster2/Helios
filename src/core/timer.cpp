// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/core/timer.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace helios::core {

Clock::time_point Clock::now() noexcept {
#ifdef _WIN32
    static const long long frequency = [] {
        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        return freq.QuadPart;
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    const long long nanos = (counter.QuadPart * 1000000000LL) / frequency;
    return time_point(duration(nanos));
#else
    return time_point(std::chrono::duration_cast<duration>(std::chrono::steady_clock::now().time_since_epoch()));
#endif
}

double FrameStats::percentile(double pct) const {
    if (count_ == 0) {
        return 0.0;
    }
    std::vector<double> sorted(samples_.begin(), samples_.begin() + static_cast<std::ptrdiff_t>(count_));
    std::sort(sorted.begin(), sorted.end());
    const std::size_t idx = static_cast<std::size_t>(
        std::clamp(pct / 100.0, 0.0, 1.0) * static_cast<double>(count_ - 1) + 0.5);
    return sorted[idx];
}

double FrameStats::min() const {
    if (count_ == 0) return 0.0;
    return *std::min_element(samples_.begin(), samples_.begin() + static_cast<std::ptrdiff_t>(count_));
}

double FrameStats::max() const {
    if (count_ == 0) return 0.0;
    return *std::max_element(samples_.begin(), samples_.begin() + static_cast<std::ptrdiff_t>(count_));
}

double FrameStats::average() const {
    if (count_ == 0) return 0.0;
    return sum_ / static_cast<double>(count_);
}

} // namespace helios::core
