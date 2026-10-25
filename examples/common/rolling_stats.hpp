// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <deque>
#include <limits>
#include <numeric>
#include <vector>

namespace helios::examples {

// Small, allocation-bounded history used by the interactive labs.  These are
// presentation statistics, not benchmark evidence: the UI and desktop
// compositor are included in the observed frame time.
class RollingStats {
  public:
    explicit RollingStats(std::size_t capacity = 240) : capacity_(std::max<std::size_t>(1, capacity)) {}

    void push(double value) {
        if (!std::isfinite(value)) {
            return;
        }
        values_.push_back(value);
        if (values_.size() > capacity_) {
            values_.pop_front();
        }
    }

    [[nodiscard]] bool empty() const noexcept { return values_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return values_.size(); }
    [[nodiscard]] double latest() const noexcept { return values_.empty() ? 0.0 : values_.back(); }

    [[nodiscard]] double average() const noexcept {
        if (values_.empty()) {
            return 0.0;
        }
        return std::accumulate(values_.begin(), values_.end(), 0.0) /
               static_cast<double>(values_.size());
    }

    [[nodiscard]] double minimum() const noexcept {
        return values_.empty() ? 0.0 : *std::min_element(values_.begin(), values_.end());
    }

    [[nodiscard]] double maximum() const noexcept {
        return values_.empty() ? 0.0 : *std::max_element(values_.begin(), values_.end());
    }

    [[nodiscard]] double percentile(double percentile_value) const {
        if (values_.empty()) {
            return 0.0;
        }
        std::vector<double> sorted{values_.begin(), values_.end()};
        std::sort(sorted.begin(), sorted.end());
        double const clamped = std::clamp(percentile_value, 0.0, 100.0) * 0.01;
        double const position = clamped * static_cast<double>(sorted.size() - 1U);
        auto const lower = static_cast<std::size_t>(std::floor(position));
        auto const upper = static_cast<std::size_t>(std::ceil(position));
        double const fraction = position - static_cast<double>(lower);
        return sorted[lower] + (sorted[upper] - sorted[lower]) * fraction;
    }

    [[nodiscard]] std::vector<float> orderedFloatValues() const {
        std::vector<float> result;
        result.reserve(values_.size());
        for (double value : values_) {
            result.push_back(static_cast<float>(value));
        }
        return result;
    }

  private:
    std::size_t capacity_ = 1;
    std::deque<double> values_;
};

} // namespace helios::examples
