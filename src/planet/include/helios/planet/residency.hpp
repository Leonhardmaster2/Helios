// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Deterministic, GPU-API-free terrain residency state. Each update derives a
// new immutable value from the previous frame, request set, and completion
// set. Old snapshots remain valid for render-thread consumption and tests.
#pragma once

#include "helios/core/error.hpp"
#include "helios/planet/cube_sphere.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace helios::planet {

enum class ResidencyStatus : std::uint8_t {
    Requested = 0,
    Generating,
    Resident,
    Retiring,
    Failed,
};

[[nodiscard]] std::string_view toString(ResidencyStatus status) noexcept;

struct ResidencyBudgets {
    std::uint32_t max_resident_tiles = 2'048;
    std::uint64_t max_cpu_bytes = 512ULL * 1024ULL * 1024ULL;
    std::uint64_t max_upload_bytes_per_frame = 32ULL * 1024ULL * 1024ULL;

    // New work is bounded both per frame and in aggregate. Re-requesting an
    // already pending/resident tile does not consume the per-frame admission
    // budget; it only refreshes priority and last-use metadata.
    std::uint32_t max_new_requests_per_frame = 64;
    std::uint32_t max_pending_tiles = 256;

    // A resident becomes a one-frame Retiring tombstone only after this many
    // complete unrequested frames. Its resources are considered released in
    // the transition to Retiring, keeping byte/tile budgets hard.
    std::uint32_t resident_grace_frames = 2;
};

[[nodiscard]] core::Result<void> validate(ResidencyBudgets const& budgets);

struct ResidencyRequest {
    TileKey key{};
    double priority = 0.0; // Greater values are admitted/retained first.
    std::uint64_t estimated_cpu_bytes = 0;
    std::uint64_t estimated_upload_bytes = 0;
};

struct ResidencyCompletion {
    TileKey key{};
    bool succeeded = true;
    std::uint64_t actual_cpu_bytes = 0;
    std::uint64_t upload_bytes = 0;
};

struct ResidencyRecord {
    TileKey key{};
    ResidencyStatus status = ResidencyStatus::Requested;
    double priority = 0.0;
    std::uint64_t estimated_cpu_bytes = 0;
    std::uint64_t estimated_upload_bytes = 0;
    std::uint64_t cpu_bytes = 0;
    std::uint64_t first_requested_frame = 0;
    std::uint64_t last_requested_frame = 0;
};

struct ResidencyFrameStats {
    std::uint32_t resident_tiles = 0;
    std::uint32_t pending_tiles = 0;
    std::uint64_t resident_cpu_bytes = 0;
    std::uint64_t uploaded_bytes = 0;
    bool request_budget_limited = false;
    bool upload_budget_limited = false;
    bool resident_budget_limited = false;
};

class ResidencySnapshot {
  public:
    ResidencySnapshot() = default;

    [[nodiscard]] std::uint64_t frameIndex() const noexcept {
        return frame_index_;
    }

    // Always sorted by TileKey, independent of input order or allocation.
    [[nodiscard]] std::vector<ResidencyRecord> const& records() const noexcept {
        return records_;
    }

    [[nodiscard]] ResidencyFrameStats const& stats() const noexcept {
        return stats_;
    }

    // These action lists are deterministic and scoped to this update only.
    [[nodiscard]] std::vector<TileKey> const& admittedRequests() const noexcept {
        return admitted_requests_;
    }
    [[nodiscard]] std::vector<TileKey> const& completedUploads() const noexcept {
        return completed_uploads_;
    }
    [[nodiscard]] std::vector<TileKey> const& retiredTiles() const noexcept {
        return retired_tiles_;
    }

    [[nodiscard]] ResidencyRecord const* find(TileKey const& key) const noexcept;

    // Returns `desired` when it is resident, otherwise the nearest resident
    // quadtree ancestor. Requested/generating/retiring/failed nodes are skipped.
    [[nodiscard]] core::Result<TileKey> residentFallback(TileKey const& desired) const;

  private:
    friend core::Result<ResidencySnapshot>
    updateResidency(ResidencySnapshot const&, ResidencyBudgets const&,
                    std::span<ResidencyRequest const>,
                    std::span<ResidencyCompletion const>);

    std::uint64_t frame_index_ = 0;
    std::vector<ResidencyRecord> records_;
    ResidencyFrameStats stats_{};
    std::vector<TileKey> admitted_requests_;
    std::vector<TileKey> completed_uploads_;
    std::vector<TileKey> retired_tiles_;
};

// State transitions are intentionally frame-stepped:
//   new -> Requested -> Generating -> Resident
// A failed completion enters Failed. A failed key may be requested again. A
// Retiring/Failed tombstone disappears on the next unrequested update.
[[nodiscard]] core::Result<ResidencySnapshot>
updateResidency(ResidencySnapshot const& previous, ResidencyBudgets const& budgets,
                std::span<ResidencyRequest const> requests = {},
                std::span<ResidencyCompletion const> completions = {});

} // namespace helios::planet
