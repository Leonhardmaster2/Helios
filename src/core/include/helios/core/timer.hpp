// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// High-resolution timing and frame-time statistics.
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <numeric>
#include <vector>

namespace helios::core {

// Monotonic high-resolution clock (QPC on Windows).
class Clock {
  public:
    using rep = std::int64_t;
    using period = std::nano;
    using duration = std::chrono::duration<rep, period>;
    using time_point = std::chrono::time_point<Clock, duration>;

    static time_point now() noexcept;
};

// Ring-buffer frame-time statistics: p50/p95/p99/max/avg in seconds.
class FrameStats {
  public:
    explicit FrameStats(std::size_t capacity = 4096) : samples_(capacity) {}

    void add(double seconds) {
        if (count_ < samples_.size()) {
            samples_[count_++] = seconds;
        } else {
            samples_[next_ % samples_.size()] = seconds;
            next_ = (next_ + 1) % samples_.size();
            wrapped_ = true;
        }
        sum_ += seconds;
        if (count_ > 0 && !wrapped_) {
            // no-op; sum maintained for avg below
        }
    }

    [[nodiscard]] std::size_t count() const noexcept {
        return count_;
    }

    // Percentile of recorded samples (0..100). Requires count() > 0.
    [[nodiscard]] double percentile(double pct) const;

    [[nodiscard]] double min() const;
    [[nodiscard]] double max() const;
    [[nodiscard]] double average() const;

    // Reset all samples.
    void reset() {
        count_ = 0;
        next_ = 0;
        wrapped_ = false;
        sum_ = 0.0;
    }

  private:
    std::vector<double> samples_;
    std::size_t count_ = 0;
    std::size_t next_ = 0;
    bool wrapped_ = false;
    double sum_ = 0.0;
};

// RAII scope timer; records elapsed seconds on destruction into FrameStats.
class ScopedTimer {
  public:
    explicit ScopedTimer(FrameStats& stats) : stats_(stats), start_(Clock::now()) {}
    ~ScopedTimer() {
        stats_.add(elapsed());
    }
    [[nodiscard]] double elapsed() const {
        return std::chrono::duration<double>(Clock::now() - start_).count();
    }

  private:
    FrameStats& stats_;
    Clock::time_point start_;
};

} // namespace helios::core
