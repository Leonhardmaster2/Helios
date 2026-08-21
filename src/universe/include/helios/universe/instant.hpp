// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Instant: the final consumer shape for a point in time (docs/ARCHITECTURE.md).
// Split representation (large integer day count + fractional seconds within
// the day) keeps microsecond-level resolution across the full v1 date range
// where a single binary64 of total seconds would not. M1 uses one synthetic
// time scale; M7 pins the real scales and conversion data.
#pragma once

#include <cstdint>
#include <limits>

namespace helios::universe {

enum class TimeScale : std::uint8_t {
    Synthetic = 0, // M1 synthetic uniform scale; no leap seconds, no offsets
    // M7 adds the real pinned scales (e.g. TDB/Tcb-family and TT-family).
};

inline constexpr double kSecondsPerDay = 86400.0;

struct Instant {
    std::int64_t whole_days = 0; // large day count (may be negative)
    double seconds_of_day = 0.0; // [0, kSecondsPerDay), normalized
    TimeScale scale = TimeScale::Synthetic;

    // Total seconds as long double; exact well beyond any v1 range need.
    [[nodiscard]] long double totalSeconds() const noexcept {
        return static_cast<long double>(whole_days) * static_cast<long double>(kSecondsPerDay) +
               static_cast<long double>(seconds_of_day);
    }

    // Three-way comparison (-1, 0, +1). Precondition: both instants use the
    // same TimeScale; cross-scale comparison is a caller error (M7 will
    // provide explicit conversions between pinned scales).
    [[nodiscard]] static int compare(Instant const& a, Instant const& b) noexcept;

    // Advance by a duration in seconds, normalizing into days + seconds-of-day.
    [[nodiscard]] static Instant advancedBy(Instant const& t, double delta_seconds);
};

} // namespace helios::universe
