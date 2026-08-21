// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/universe/instant.hpp"

#include <cmath>

namespace helios::universe {

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
