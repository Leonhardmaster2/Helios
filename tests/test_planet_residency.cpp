// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helios/planet/residency.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <vector>

namespace {

using namespace helios::planet;

ResidencySnapshot update(ResidencySnapshot const& previous, ResidencyBudgets const& budgets,
                         std::span<ResidencyRequest const> requests = {},
                         std::span<ResidencyCompletion const> completions = {}) {
    auto result = updateResidency(previous, budgets, requests, completions);
    REQUIRE(result.ok());
    return std::move(result.value());
}

ResidencyRecord const& record(ResidencySnapshot const& snapshot, TileKey const& key) {
    auto const* found = snapshot.find(key);
    REQUIRE(found != nullptr);
    return *found;
}

ResidencyRequest request(TileKey key, double priority, std::uint64_t bytes = 64) {
    return {key, priority, bytes, bytes};
}

bool hasStatus(ResidencyRecord const& value, ResidencyStatus expected) {
    // This pinned doctest release cannot stringify enums that have an
    // ADL-visible toString() returning std::string_view. Keep status
    // comparisons as a plain boolean so a failed assertion remains portable.
    return static_cast<std::uint8_t>(value.status) ==
           static_cast<std::uint8_t>(expected);
}

} // namespace

TEST_CASE("residency validation rejects unbounded or malformed work") {
    ResidencyBudgets budgets{};
    CHECK(validate(budgets).ok());
    budgets.max_cpu_bytes = 0;
    CHECK(validate(budgets).failed());
    budgets = {};
    budgets.max_pending_tiles = 0;
    CHECK(validate(budgets).failed());

    ResidencySnapshot const empty{};
    ResidencyRequest const bad_key{TileKey{CubeFace::PositiveX, 1, 2, 0}, 1.0, 1, 1};
    CHECK(updateResidency(empty, ResidencyBudgets{}, std::span{&bad_key, 1}).failed());
    ResidencyCompletion const unknown{TileKey{CubeFace::PositiveX, 0, 0, 0}, true, 1, 1};
    CHECK(updateResidency(empty, ResidencyBudgets{}, {}, std::span{&unknown, 1}).failed());
}

TEST_CASE("request admission is priority ordered stable and hard bounded") {
    ResidencyBudgets budgets{};
    budgets.max_new_requests_per_frame = 2;
    budgets.max_pending_tiles = 8;
    TileKey const low{CubeFace::PositiveX, 2, 0, 0};
    TileKey const high_first{CubeFace::PositiveX, 2, 1, 0};
    TileKey const high_second{CubeFace::PositiveX, 2, 2, 0};
    std::array<ResidencyRequest, 3> forward{
        request(low, 1.0), request(high_second, 5.0), request(high_first, 5.0)};
    std::array<ResidencyRequest, 3> reverse{
        request(high_first, 5.0), request(high_second, 5.0), request(low, 1.0)};

    ResidencySnapshot const first = update({}, budgets, forward);
    ResidencySnapshot const second = update({}, budgets, reverse);
    REQUIRE(first.admittedRequests().size() == 2);
    REQUIRE(second.admittedRequests().size() == 2);
    CHECK(first.admittedRequests()[0] == high_first);
    CHECK(first.admittedRequests()[1] == high_second);
    CHECK(first.admittedRequests() == second.admittedRequests());
    CHECK(first.stats().pending_tiles == 2);
    CHECK(first.stats().request_budget_limited);
    CHECK(first.find(low) == nullptr);
    CHECK(hasStatus(record(first, high_first), ResidencyStatus::Requested));
    CHECK(hasStatus(record(first, high_second), ResidencyStatus::Requested));
}

TEST_CASE("frame updates are immutable and expose a resident parent fallback") {
    ResidencyBudgets budgets{};
    budgets.resident_grace_frames = 20;
    TileKey const root{CubeFace::PositiveZ, 0, 0, 0};
    TileKey const child{CubeFace::PositiveZ, 3, 5, 2};
    std::array<ResidencyRequest, 2> requests{request(root, 100.0), request(child, 200.0)};

    ResidencySnapshot const requested = update({}, budgets, requests);
    ResidencySnapshot const generating = update(requested, budgets);
    CHECK(hasStatus(record(requested, root), ResidencyStatus::Requested));
    CHECK(hasStatus(record(requested, child), ResidencyStatus::Requested));
    CHECK(hasStatus(record(generating, root), ResidencyStatus::Generating));
    CHECK(hasStatus(record(generating, child), ResidencyStatus::Generating));

    ResidencyCompletion const root_done{root, true, 64, 32};
    ResidencySnapshot const resident_root =
        update(generating, budgets, {}, std::span{&root_done, 1});
    CHECK(hasStatus(record(generating, root), ResidencyStatus::Generating));
    CHECK(hasStatus(record(resident_root, root), ResidencyStatus::Resident));
    CHECK(hasStatus(record(resident_root, child), ResidencyStatus::Generating));
    auto fallback = resident_root.residentFallback(child);
    REQUIRE(fallback.ok());
    CHECK(fallback.value() == root);
    CHECK(resident_root.residentFallback(TileKey{CubeFace::NegativeZ, 2, 1, 1}).failed());

    ResidencyCompletion const child_failed{child, false, 0, 0};
    ResidencySnapshot const failed =
        update(resident_root, budgets, {}, std::span{&child_failed, 1});
    CHECK(hasStatus(record(failed, child), ResidencyStatus::Failed));
    CHECK(toString(ResidencyStatus::Failed) == "failed");
}

TEST_CASE("upload bandwidth budget defers lower-priority completions") {
    ResidencyBudgets budgets{};
    budgets.max_upload_bytes_per_frame = 100;
    budgets.max_cpu_bytes = 1'000;
    budgets.max_resident_tiles = 8;
    std::array<TileKey, 3> keys{
        TileKey{CubeFace::PositiveY, 2, 0, 0},
        TileKey{CubeFace::PositiveY, 2, 1, 0},
        TileKey{CubeFace::PositiveY, 2, 2, 0},
    };
    std::array<ResidencyRequest, 3> requests{
        request(keys[0], 1.0), request(keys[1], 3.0), request(keys[2], 2.0)};
    ResidencySnapshot const requested = update({}, budgets, requests);
    ResidencySnapshot const generating = update(requested, budgets);
    std::array<ResidencyCompletion, 3> completions{
        ResidencyCompletion{keys[0], true, 60, 60},
        ResidencyCompletion{keys[1], true, 60, 60},
        ResidencyCompletion{keys[2], true, 60, 60},
    };
    ResidencySnapshot const completed = update(generating, budgets, {}, completions);

    REQUIRE(completed.completedUploads().size() == 1);
    CHECK(completed.completedUploads()[0] == keys[1]);
    CHECK(completed.stats().uploaded_bytes == 60);
    CHECK(completed.stats().upload_budget_limited);
    CHECK(completed.stats().resident_tiles == 1);
    CHECK(completed.stats().pending_tiles == 2);
    CHECK(hasStatus(record(completed, keys[1]), ResidencyStatus::Resident));
    CHECK(hasStatus(record(completed, keys[0]), ResidencyStatus::Generating));
    CHECK(hasStatus(record(completed, keys[2]), ResidencyStatus::Generating));
}

TEST_CASE("resident count and CPU byte budgets evict only lower-priority content") {
    ResidencyBudgets budgets{};
    budgets.max_resident_tiles = 2;
    budgets.max_cpu_bytes = 200;
    budgets.max_upload_bytes_per_frame = 500;
    budgets.resident_grace_frames = 100;
    TileKey const low{CubeFace::NegativeY, 2, 0, 0};
    TileKey const medium{CubeFace::NegativeY, 2, 1, 0};
    TileKey const high{CubeFace::NegativeY, 2, 2, 0};

    std::array<ResidencyRequest, 2> initial_requests{request(low, 1.0, 100),
                                                     request(medium, 2.0, 100)};
    ResidencySnapshot snapshot = update({}, budgets, initial_requests);
    snapshot = update(snapshot, budgets, initial_requests);
    std::array<ResidencyCompletion, 2> initial_completions{
        ResidencyCompletion{low, true, 100, 50},
        ResidencyCompletion{medium, true, 100, 50},
    };
    snapshot = update(snapshot, budgets, initial_requests, initial_completions);
    REQUIRE(snapshot.stats().resident_tiles == 2);
    REQUIRE(snapshot.stats().resident_cpu_bytes == 200);

    std::array<ResidencyRequest, 3> all_requests{
        request(low, 1.0, 100), request(medium, 2.0, 100), request(high, 10.0, 100)};
    snapshot = update(snapshot, budgets, all_requests); // High: Requested.
    snapshot = update(snapshot, budgets, all_requests); // High: Generating.
    ResidencyCompletion const high_done{high, true, 100, 50};
    snapshot = update(snapshot, budgets, all_requests, std::span{&high_done, 1});

    CHECK(snapshot.stats().resident_tiles == 2);
    CHECK(snapshot.stats().resident_cpu_bytes == 200);
    CHECK(hasStatus(record(snapshot, high), ResidencyStatus::Resident));
    CHECK(hasStatus(record(snapshot, medium), ResidencyStatus::Resident));
    CHECK(hasStatus(record(snapshot, low), ResidencyStatus::Retiring));
    REQUIRE(snapshot.retiredTiles().size() == 1);
    CHECK(snapshot.retiredTiles()[0] == low);
}

TEST_CASE("an incoming tile cannot partially evict residents when it still cannot fit") {
    ResidencyBudgets budgets{};
    budgets.max_resident_tiles = 3;
    budgets.max_cpu_bytes = 200;
    budgets.max_upload_bytes_per_frame = 500;
    budgets.resident_grace_frames = 100;
    TileKey const low{CubeFace::PositiveX, 2, 0, 0};
    TileKey const protected_high{CubeFace::PositiveX, 2, 1, 0};
    TileKey const incoming{CubeFace::PositiveX, 2, 2, 0};

    std::array<ResidencyRequest, 2> initial{
        request(low, 1.0, 100), request(protected_high, 10.0, 100)};
    ResidencySnapshot snapshot = update({}, budgets, initial);
    snapshot = update(snapshot, budgets, initial);
    std::array<ResidencyCompletion, 2> ready{
        ResidencyCompletion{low, true, 100, 50},
        ResidencyCompletion{protected_high, true, 100, 50},
    };
    snapshot = update(snapshot, budgets, initial, ready);

    std::array<ResidencyRequest, 3> desired{
        request(low, 1.0, 100), request(protected_high, 10.0, 100),
        request(incoming, 5.0, 150)};
    snapshot = update(snapshot, budgets, desired);
    snapshot = update(snapshot, budgets, desired);
    ResidencyCompletion const incoming_ready{incoming, true, 150, 50};
    snapshot = update(snapshot, budgets, desired,
                      std::span{&incoming_ready, 1});

    CHECK(snapshot.stats().resident_budget_limited);
    CHECK(snapshot.retiredTiles().empty());
    CHECK(snapshot.stats().resident_tiles == 2);
    CHECK(snapshot.stats().resident_cpu_bytes == 200);
    CHECK(hasStatus(record(snapshot, low), ResidencyStatus::Resident));
    CHECK(hasStatus(record(snapshot, protected_high), ResidencyStatus::Resident));
    CHECK(hasStatus(record(snapshot, incoming), ResidencyStatus::Generating));
}

TEST_CASE("retirement is observable for one frame and releases budget immediately") {
    ResidencyBudgets budgets{};
    budgets.resident_grace_frames = 0;
    TileKey const key{CubeFace::NegativeX, 1, 0, 1};
    ResidencyRequest const keep = request(key, 1.0);
    ResidencySnapshot snapshot = update({}, budgets, std::span{&keep, 1});
    snapshot = update(snapshot, budgets, std::span{&keep, 1});
    ResidencyCompletion const done{key, true, 64, 64};
    snapshot = update(snapshot, budgets, std::span{&keep, 1}, std::span{&done, 1});
    REQUIRE(hasStatus(record(snapshot, key), ResidencyStatus::Resident));

    ResidencySnapshot const resident_copy = snapshot;
    snapshot = update(snapshot, budgets);
    CHECK(hasStatus(record(resident_copy, key), ResidencyStatus::Resident));
    CHECK(hasStatus(record(snapshot, key), ResidencyStatus::Retiring));
    CHECK(snapshot.stats().resident_tiles == 0);
    CHECK(snapshot.stats().resident_cpu_bytes == 0);
    CHECK(snapshot.residentFallback(key).failed());

    snapshot = update(snapshot, budgets);
    CHECK(snapshot.find(key) == nullptr);
}
