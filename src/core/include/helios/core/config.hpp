// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Layered configuration: defaults < JSON file < command line.
// All values are typed on read; unknown keys are reported, not ignored.
#pragma once

#include "helios/core/error.hpp"

#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace helios::core {

class Config {
public:
    // Build config from defaults, an optional JSON file, then CLI overrides.
    // CLI syntax: --key=value or --key value (both supported); booleans accept
    // true/false/1/0.
    static Result<Config> load(std::string_view json_file, std::vector<std::string> const& cli_args);

    // Typed accessors. Returns default when the key is absent; returns an
    // error when the value has the wrong type.
    [[nodiscard]] Result<std::string> getString(std::string_view key, std::string fallback = "") const;
    [[nodiscard]] Result<int> getInt(std::string_view key, int fallback = 0) const;
    [[nodiscard]] Result<std::uint32_t> getUInt(std::string_view key, std::uint32_t fallback = 0) const;
    [[nodiscard]] Result<double> getDouble(std::string_view key, double fallback = 0.0) const;
    [[nodiscard]] Result<bool> getBool(std::string_view key, bool fallback = false) const;

    // Set a value (used by tests and programmatic setup).
    void set(std::string key, nlohmann::json value);

    // Keys explicitly provided on the command line (for evidence reporting).
    [[nodiscard]] std::vector<std::string> const& cliKeys() const noexcept { return cli_keys_; }

    [[nodiscard]] nlohmann::json const& raw() const noexcept { return values_; }

private:
    nlohmann::json values_ = nlohmann::json::object();
    std::vector<std::string> cli_keys_;
};

} // namespace helios::core
