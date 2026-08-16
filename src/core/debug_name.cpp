// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/core/debug_name.hpp"

#include <atomic>
#include <format>
#include <mutex>
#include <unordered_map>

namespace helios::core {

std::string makeDebugName(std::string_view category, std::uint32_t index) {
    std::string name = std::format("{}[{}]", category, index);
    if (name.size() > kMaxDebugNameLength) {
        name.resize(kMaxDebugNameLength);
    }
    return name;
}

std::string makeAutoDebugName(std::string_view category) {
    static std::mutex mutex;
    static std::unordered_map<std::string, std::uint32_t> counters;
    std::lock_guard lock(mutex);
    std::uint32_t& counter = counters[std::string(category)];
    return makeDebugName(category, counter++);
}

std::string sanitizeDebugName(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '_' || c == '-' || c == '.';
        out.push_back(ok ? c : '_');
    }
    while (!out.empty() && (out.front() >= '0' && out.front() <= '9')) {
        out.erase(out.begin());
    }
    if (out.empty()) {
        out = "unnamed";
    }
    if (out.size() > kMaxDebugNameLength) {
        out.resize(kMaxDebugNameLength);
    }
    return out;
}

} // namespace helios::core
