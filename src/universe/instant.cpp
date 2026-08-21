// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/universe/instant.hpp"

#include <cmath>

namespace helios::universe {

bool Instant::isValid() const noexcept {
    return std::isfinite(seconds_of_day) && seconds_of_day >= 0.0 && seconds_of_day < kSecondsPerDay;
}

core::Result<Instant> Instant::make(std::int64_t whole_days, double seconds_of_day, TimeScale scale) {
    if (!std::isfinite(seconds_of_day)) {
        return core::makeError(core::ErrorCode::InvalidArgument, "instant seconds_of_day must be finite");
    }
    Instant out;
    // Normalize out-of-range seconds instead of rejecting them: callers
    // computing with doubles legitimately produce 86400.0-sized values.
    if (seconds_of_day < 0.0 || seconds_of_day >= kSecondsPerDay) {
        double days = std::floor(seconds_of_day / kSecondsPerDay);
        seconds_of_day -= days * kSecondsPerDay;
        if (seconds_of_day < 0.0) { // FP drift guard
            seconds_of_day += kSecondsPerDay;
            days -= 1.0;
        }
        whole_days += static_cast<std::int64_t>(days);
    }
    out.whole_days = whole_days;
    out.seconds_of_day = seconds_of_day;
    out.scale = scale;
    return out;
}

int Instant::compare(Instant const& a, Instant const& b) noexcept {
    // Compare day counts first (exact), then seconds of day.
    if (a.whole_days < b.whole_days)
        return -1;
    if (a.whole_days > b.whole_days)
        return +1;
    if (a.seconds_of_day < b.seconds_of_day)
        return -1;
    if (a.seconds_of_day > b.seconds_of_day)
        return +1;
    return 0;
}

Instant Instant::advancedBy(Instant const& t, double delta_seconds) {
    Instant out = t;
    double seconds = t.seconds_of_day + delta_seconds;
    double days = std::floor(seconds / kSecondsPerDay);
    seconds -= days * kSecondsPerDay;
    // Guard against FP drift pushing the value back out of range.
    if (seconds < 0.0) {
        seconds += kSecondsPerDay;
        days -= 1.0;
    } else if (seconds >= kSecondsPerDay) {
        seconds -= kSecondsPerDay;
        days += 1.0;
    }
    out.whole_days += static_cast<std::int64_t>(days);
    out.seconds_of_day = seconds;
    return out;
}

} // namespace helios::universe
