// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/planet/residency.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace helios::planet {
namespace {

[[nodiscard]] core::Error invalid(std::string message) {
    return core::makeError(core::ErrorCode::InvalidArgument, std::move(message));
}

[[nodiscard]] bool requestHigherPriority(ResidencyRequest const& left,
                                         ResidencyRequest const& right) noexcept {
    if (left.priority != right.priority) {
        return left.priority > right.priority;
    }
    return left.key < right.key;
}

[[nodiscard]] bool recordHigherPriority(ResidencyRecord const& left,
                                        ResidencyRecord const& right) noexcept {
    if (left.priority != right.priority) {
        return left.priority > right.priority;
    }
    return left.key < right.key;
}

[[nodiscard]] bool recordLowerPriority(ResidencyRecord const& left,
                                       ResidencyRecord const& right) noexcept {
    return recordHigherPriority(right, left);
}

[[nodiscard]] core::Result<void> validateRequest(ResidencyRequest const& request) {
    if (!isValid(request.key)) {
        return invalid("residency request contains an invalid tile key");
    }
    if (!std::isfinite(request.priority) || request.priority < 0.0) {
        return invalid("residency request priority must be finite and non-negative");
    }
    if (request.estimated_cpu_bytes == 0 || request.estimated_upload_bytes == 0) {
        return invalid("residency request byte estimates must both be non-zero");
    }
    return {};
}

[[nodiscard]] core::Result<void> validateCompletion(ResidencyCompletion const& completion) {
    if (!isValid(completion.key)) {
        return invalid("residency completion contains an invalid tile key");
    }
    if (completion.succeeded &&
        (completion.actual_cpu_bytes == 0 || completion.upload_bytes == 0)) {
        return invalid("successful residency completion byte counts must both be non-zero");
    }
    return {};
}

void retire(ResidencyRecord& record, std::vector<TileKey>& retired_tiles) {
    record.status = ResidencyStatus::Retiring;
    record.cpu_bytes = 0;
    retired_tiles.push_back(record.key);
}

[[nodiscard]] std::uint32_t pendingCount(std::map<TileKey, ResidencyRecord> const& records) {
    return static_cast<std::uint32_t>(std::count_if(
        records.begin(), records.end(), [](auto const& item) {
            return item.second.status == ResidencyStatus::Requested ||
                   item.second.status == ResidencyStatus::Generating;
        }));
}

struct ResidentUsage {
    std::uint64_t count = 0;
    std::uint64_t bytes = 0;
    bool bytes_overflow = false;
};

[[nodiscard]] ResidentUsage
residentUsage(std::map<TileKey, ResidencyRecord> const& records) {
    ResidentUsage usage{};
    for (auto const& [key, record] : records) {
        (void)key;
        if (record.status == ResidencyStatus::Resident) {
            ++usage.count;
            if (record.cpu_bytes >
                std::numeric_limits<std::uint64_t>::max() - usage.bytes) {
                usage.bytes = std::numeric_limits<std::uint64_t>::max();
                usage.bytes_overflow = true;
            } else {
                usage.bytes += record.cpu_bytes;
            }
        }
    }
    return usage;
}

[[nodiscard]] bool makeRoomFor(ResidencyRecord const& incoming, std::uint64_t incoming_bytes,
                               ResidencyBudgets const& budgets,
                               std::map<TileKey, ResidencyRecord>& records,
                               std::vector<TileKey>& retired_tiles) {
    // updateResidency has already brought existing resident usage within the
    // active budgets, so these values are exact and subtraction is safe.
    ResidentUsage simulated = residentUsage(records);
    std::vector<TileKey> victims;
    auto fits = [&] {
        return !simulated.bytes_overflow &&
               simulated.count < budgets.max_resident_tiles &&
               incoming_bytes <= budgets.max_cpu_bytes - simulated.bytes;
    };
    if (fits()) {
        return true;
    }

    // Choose the complete eviction set before changing any record. Without
    // this transactional selection, an incoming tile that can displace one
    // low-priority resident but still cannot fit past a higher-priority one
    // would retire useful content and then fail admission anyway.
    std::vector<TileKey> candidates;
    for (auto const& [key, record] : records) {
        if (record.status == ResidencyStatus::Resident) {
            candidates.push_back(key);
        }
    }
    std::sort(candidates.begin(), candidates.end(), [&](TileKey const& left,
                                                         TileKey const& right) {
        return recordLowerPriority(records.find(left)->second,
                                   records.find(right)->second);
    });

    for (TileKey const& key : candidates) {
        ResidencyRecord const& candidate = records.find(key)->second;
        if (!recordHigherPriority(incoming, candidate)) {
            return false;
        }
        victims.push_back(key);
        --simulated.count;
        simulated.bytes -= candidate.cpu_bytes;
        if (fits()) {
            for (TileKey const& victim_key : victims) {
                retire(records.find(victim_key)->second, retired_tiles);
            }
            return true;
        }
    }

    return false;
}

} // namespace

std::string_view toString(ResidencyStatus status) noexcept {
    switch (status) {
    case ResidencyStatus::Requested:
        return "requested";
    case ResidencyStatus::Generating:
        return "generating";
    case ResidencyStatus::Resident:
        return "resident";
    case ResidencyStatus::Retiring:
        return "retiring";
    case ResidencyStatus::Failed:
        return "failed";
    }
    return "invalid";
}

core::Result<void> validate(ResidencyBudgets const& budgets) {
    if (budgets.max_resident_tiles == 0) {
        return invalid("residency must allow at least one resident tile");
    }
    if (budgets.max_cpu_bytes == 0 || budgets.max_upload_bytes_per_frame == 0) {
        return invalid("residency CPU and upload byte budgets must be non-zero");
    }
    if (budgets.max_new_requests_per_frame == 0 || budgets.max_pending_tiles == 0) {
        return invalid("residency request budgets must be non-zero");
    }
    if (budgets.resident_grace_frames > 1'000'000U) {
        return invalid("residency grace period exceeds the bounded frame limit");
    }
    return {};
}

ResidencyRecord const* ResidencySnapshot::find(TileKey const& key) const noexcept {
    auto const it = std::lower_bound(
        records_.begin(), records_.end(), key,
        [](ResidencyRecord const& record, TileKey const& value) { return record.key < value; });
    return it != records_.end() && it->key == key ? &*it : nullptr;
}

core::Result<TileKey> ResidencySnapshot::residentFallback(TileKey const& desired) const {
    if (!isValid(desired)) {
        return invalid("cannot find a resident fallback for an invalid tile key");
    }
    TileKey cursor = desired;
    while (true) {
        ResidencyRecord const* record = find(cursor);
        if (record != nullptr && record->status == ResidencyStatus::Resident) {
            return cursor;
        }
        if (cursor.level == 0) {
            break;
        }
        auto parent = tileParent(cursor);
        if (parent.failed()) {
            return parent.error();
        }
        cursor = parent.value();
    }
    return core::makeError(core::ErrorCode::NotFound,
                           "no resident tile exists in the desired tile's ancestor chain");
}

core::Result<ResidencySnapshot>
updateResidency(ResidencySnapshot const& previous, ResidencyBudgets const& budgets,
                std::span<ResidencyRequest const> requests,
                std::span<ResidencyCompletion const> completions) {
    auto valid_budgets = validate(budgets);
    if (valid_budgets.failed()) {
        return valid_budgets.error();
    }
    if (previous.frame_index_ == std::numeric_limits<std::uint64_t>::max()) {
        return core::makeError(core::ErrorCode::OutOfRange,
                               "residency frame index cannot be incremented");
    }

    ResidencySnapshot next{};
    next.frame_index_ = previous.frame_index_ + 1U;
    std::map<TileKey, ResidencyRecord> records;
    for (ResidencyRecord record : previous.records_) {
        if (!isValid(record.key)) {
            return core::makeError(core::ErrorCode::CorruptData,
                                   "previous residency snapshot contains an invalid key");
        }
        if (record.status == ResidencyStatus::Retiring) {
            continue;
        }
        if (record.status == ResidencyStatus::Requested) {
            record.status = ResidencyStatus::Generating;
        }
        if (!records.emplace(record.key, record).second) {
            return core::makeError(core::ErrorCode::CorruptData,
                                   "previous residency snapshot contains duplicate keys");
        }
    }

    // Canonicalize duplicate requests by taking their greatest priority and
    // largest (most conservative) byte estimates. The result is input-order
    // independent.
    std::map<TileKey, ResidencyRequest> canonical_requests;
    for (ResidencyRequest const& request : requests) {
        auto valid = validateRequest(request);
        if (valid.failed()) {
            return valid.error();
        }
        auto [it, inserted] = canonical_requests.emplace(request.key, request);
        if (!inserted) {
            it->second.priority = std::max(it->second.priority, request.priority);
            it->second.estimated_cpu_bytes =
                std::max(it->second.estimated_cpu_bytes, request.estimated_cpu_bytes);
            it->second.estimated_upload_bytes =
                std::max(it->second.estimated_upload_bytes, request.estimated_upload_bytes);
        }
    }

    std::set<TileKey> requested_this_frame;
    std::vector<ResidencyRequest> new_requests;
    for (auto const& [key, request] : canonical_requests) {
        requested_this_frame.insert(key);
        auto existing = records.find(key);
        if (existing != records.end() && existing->second.status != ResidencyStatus::Failed) {
            existing->second.priority = request.priority;
            existing->second.estimated_cpu_bytes = request.estimated_cpu_bytes;
            existing->second.estimated_upload_bytes = request.estimated_upload_bytes;
            existing->second.last_requested_frame = next.frame_index_;
        } else {
            new_requests.push_back(request);
        }
    }
    std::sort(new_requests.begin(), new_requests.end(), requestHigherPriority);

    std::uint32_t admitted = 0;
    std::uint32_t pending = pendingCount(records);
    for (ResidencyRequest const& request : new_requests) {
        if (admitted >= budgets.max_new_requests_per_frame || pending >= budgets.max_pending_tiles) {
            next.stats_.request_budget_limited = true;
            continue;
        }
        ++admitted;
        auto existing = records.find(request.key);
        if (existing != records.end()) {
            records.erase(existing); // Failed retry.
        }
        ResidencyRecord record{request.key,
                               ResidencyStatus::Requested,
                               request.priority,
                               request.estimated_cpu_bytes,
                               request.estimated_upload_bytes,
                               0,
                               next.frame_index_,
                               next.frame_index_};
        if (request.estimated_cpu_bytes > budgets.max_cpu_bytes ||
            request.estimated_upload_bytes > budgets.max_upload_bytes_per_frame) {
            record.status = ResidencyStatus::Failed;
        } else {
            ++pending;
        }
        records.emplace(record.key, record);
        next.admitted_requests_.push_back(record.key);
    }

    // Failed diagnostics are one-frame tombstones unless explicitly retried.
    for (auto it = records.begin(); it != records.end();) {
        if (it->second.status == ResidencyStatus::Failed &&
            !requested_this_frame.contains(it->first)) {
            it = records.erase(it);
        } else {
            ++it;
        }
    }

    // Resident grace is evaluated after request refresh, so an in-use tile is
    // never retired because of request input ordering.
    for (auto& [key, record] : records) {
        (void)key;
        if (record.status != ResidencyStatus::Resident ||
            requested_this_frame.contains(record.key)) {
            continue;
        }
        if (next.frame_index_ - record.last_requested_frame > budgets.resident_grace_frames) {
            retire(record, next.retired_tiles_);
        }
    }

    // A caller may tighten budgets between frames. Shed the least-important
    // resident records immediately so even that transition produces a hard-
    // bounded snapshot.
    while (true) {
        auto const usage = residentUsage(records);
        if (!usage.bytes_overflow && usage.count <= budgets.max_resident_tiles &&
            usage.bytes <= budgets.max_cpu_bytes) {
            break;
        }
        auto victim = records.end();
        for (auto it = records.begin(); it != records.end(); ++it) {
            if (it->second.status != ResidencyStatus::Resident) {
                continue;
            }
            if (victim == records.end() || recordLowerPriority(it->second, victim->second)) {
                victim = it;
            }
        }
        if (victim == records.end()) {
            break;
        }
        retire(victim->second, next.retired_tiles_);
        next.stats_.resident_budget_limited = true;
    }

    std::map<TileKey, ResidencyCompletion> canonical_completions;
    for (ResidencyCompletion const& completion : completions) {
        auto valid = validateCompletion(completion);
        if (valid.failed()) {
            return valid.error();
        }
        if (!canonical_completions.emplace(completion.key, completion).second) {
            return invalid("residency update contains duplicate completions for one tile");
        }
    }

    std::vector<ResidencyCompletion> ordered_completions;
    ordered_completions.reserve(canonical_completions.size());
    for (auto const& [key, completion] : canonical_completions) {
        auto const record = records.find(key);
        if (record == records.end() || record->second.status != ResidencyStatus::Generating) {
            return invalid("residency completion does not match a generating tile");
        }
        ordered_completions.push_back(completion);
    }
    std::sort(ordered_completions.begin(), ordered_completions.end(),
              [&](ResidencyCompletion const& left, ResidencyCompletion const& right) {
                  ResidencyRecord const& left_record = records.find(left.key)->second;
                  ResidencyRecord const& right_record = records.find(right.key)->second;
                  return recordHigherPriority(left_record, right_record);
              });

    std::uint64_t uploaded_bytes = 0;
    for (ResidencyCompletion const& completion : ordered_completions) {
        ResidencyRecord& record = records.find(completion.key)->second;
        if (!completion.succeeded) {
            record.status = ResidencyStatus::Failed;
            record.cpu_bytes = 0;
            continue;
        }
        if (completion.actual_cpu_bytes > budgets.max_cpu_bytes ||
            completion.upload_bytes > budgets.max_upload_bytes_per_frame) {
            record.status = ResidencyStatus::Failed;
            record.cpu_bytes = 0;
            continue;
        }
        if (completion.upload_bytes > budgets.max_upload_bytes_per_frame - uploaded_bytes) {
            next.stats_.upload_budget_limited = true;
            continue; // Remains Generating for a later completion/update.
        }
        if (!makeRoomFor(record, completion.actual_cpu_bytes, budgets, records,
                         next.retired_tiles_)) {
            next.stats_.resident_budget_limited = true;
            continue; // Remains Generating; a more important tile may retire later.
        }
        record.status = ResidencyStatus::Resident;
        record.cpu_bytes = completion.actual_cpu_bytes;
        uploaded_bytes += completion.upload_bytes;
        next.completed_uploads_.push_back(record.key);
    }

    std::sort(next.retired_tiles_.begin(), next.retired_tiles_.end());
    auto const usage = residentUsage(records);
    if (usage.bytes_overflow || usage.count > std::numeric_limits<std::uint32_t>::max()) {
        return core::makeError(core::ErrorCode::OutOfRange,
                               "residency usage cannot be represented in frame statistics");
    }
    next.stats_.resident_tiles = static_cast<std::uint32_t>(usage.count);
    next.stats_.resident_cpu_bytes = usage.bytes;
    next.stats_.pending_tiles = pendingCount(records);
    next.stats_.uploaded_bytes = uploaded_bytes;

    next.records_.reserve(records.size());
    for (auto const& [key, record] : records) {
        (void)key;
        next.records_.push_back(record);
    }
    return next;
}

} // namespace helios::planet
