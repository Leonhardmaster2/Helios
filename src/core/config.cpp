// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/core/config.hpp"

#include "helios/core/io.hpp"

#include <fstream>
#include <sstream>

namespace helios::core {

Result<Config> Config::load(std::string_view json_file, std::vector<std::string> const& cli_args) {
    Config cfg;

    if (!json_file.empty()) {
        auto data = readTextFile(json_file);
        if (data.failed()) {
            return data.error();
        }
        auto parsed = nlohmann::json::parse(data.value(), nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            return makeError(ErrorCode::CorruptData,
                             "config file is not a JSON object: " + std::string(json_file));
        }
        cfg.values_ = std::move(parsed);
    }

    for (std::size_t i = 0; i < cli_args.size(); ++i) {
        std::string arg = cli_args[i];
        if (!arg.starts_with("--")) {
            continue;
        }
        std::string key = arg.substr(2);
        std::string value;
        bool has_value = false;
        if (auto eq = key.find('='); eq != std::string::npos) {
            value = key.substr(eq + 1);
            key = key.substr(0, eq);
            has_value = true;
        } else if (i + 1 < cli_args.size() && !cli_args[i + 1].starts_with("--")) {
            value = cli_args[i + 1];
            ++i;
            has_value = true;
        }
        if (key.empty()) {
            continue;
        }
        if (!has_value) {
            value = "true"; // bare flag
        }
        // Interpret value as JSON when possible, else keep as string.
        auto parsed = nlohmann::json::parse(value, nullptr, false);
        if (!parsed.is_discarded()) {
            cfg.values_[key] = std::move(parsed);
        } else {
            cfg.values_[key] = value;
        }
        cfg.cli_keys_.push_back(key);
    }

    return cfg;
}

Result<std::string> Config::getString(std::string_view key, std::string fallback) const {
    auto it = values_.find(std::string(key));
    if (it == values_.end() || it->is_null()) {
        return fallback;
    }
    if (!it->is_string()) {
        return makeError(ErrorCode::InvalidArgument,
                         "config key '" + std::string(key) + "' is not a string");
    }
    return it->get<std::string>();
}

Result<int> Config::getInt(std::string_view key, int fallback) const {
    auto it = values_.find(std::string(key));
    if (it == values_.end() || it->is_null()) {
        return fallback;
    }
    if (!it->is_number_integer()) {
        return makeError(ErrorCode::InvalidArgument,
                         "config key '" + std::string(key) + "' is not an integer");
    }
    return it->get<int>();
}

Result<std::uint32_t> Config::getUInt(std::string_view key, std::uint32_t fallback) const {
    auto r = getInt(key, static_cast<int>(fallback));
    if (r.failed()) {
        return r.error();
    }
    if (r.value() < 0) {
        return makeError(ErrorCode::InvalidArgument,
                         "config key '" + std::string(key) + "' is negative");
    }
    return static_cast<std::uint32_t>(r.value());
}

Result<double> Config::getDouble(std::string_view key, double fallback) const {
    auto it = values_.find(std::string(key));
    if (it == values_.end() || it->is_null()) {
        return fallback;
    }
    if (!it->is_number()) {
        return makeError(ErrorCode::InvalidArgument,
                         "config key '" + std::string(key) + "' is not a number");
    }
    return it->get<double>();
}

Result<bool> Config::getBool(std::string_view key, bool fallback) const {
    auto it = values_.find(std::string(key));
    if (it == values_.end() || it->is_null()) {
        return fallback;
    }
    if (it->is_boolean()) {
        return it->get<bool>();
    }
    if (it->is_string()) {
        const std::string s = it->get<std::string>();
        if (s == "true" || s == "1") return true;
        if (s == "false" || s == "0") return false;
    }
    if (it->is_number_integer()) {
        return it->get<int>() != 0;
    }
    return makeError(ErrorCode::InvalidArgument,
                     "config key '" + std::string(key) + "' is not a boolean");
}

void Config::set(std::string key, nlohmann::json value) {
    values_[std::move(key)] = std::move(value);
}

} // namespace helios::core
