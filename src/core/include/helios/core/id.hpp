// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Typed generational ID handles. Helios avoids raw pointers/index handles
// across ownership boundaries; Id<Tag> provides a distinct type per
// domain without per-object allocation.
#pragma once

#include <cstdint>
#include <functional>

namespace helios::core {

template <typename Tag, typename Storage = std::uint32_t> class Id {
  public:
    using StorageType = Storage;

    constexpr Id() noexcept = default;

    explicit constexpr Id(Storage value) noexcept : value_(value) {}

    [[nodiscard]] constexpr Storage value() const noexcept {
        return value_;
    }
    [[nodiscard]] constexpr bool valid() const noexcept {
        return value_ != invalid_value;
    }

    static constexpr Storage invalid_value = static_cast<Storage>(-1);

    [[nodiscard]] constexpr bool operator==(Id const&) const noexcept = default;

  private:
    Storage value_ = invalid_value;
};

} // namespace helios::core

namespace std {
template <typename Tag, typename Storage> struct hash<helios::core::Id<Tag, Storage>> {
    std::size_t operator()(helios::core::Id<Tag, Storage> const& id) const noexcept {
        return std::hash<Storage>{}(id.value());
    }
};
} // namespace std
