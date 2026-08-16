// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/core/config.hpp"

#include "helios/core/io.hpp"

#include <fstream>
#include <sstream>

namespace {

// Sets a dotted key ("window.width") creating nested objects as needed.
void setNested(nlohmann::json& root, std::string_view key, nlohmann::json value) {
    nlohmann::json* node = &root;
    std::size_t start = 0;
    while (true) {
        const std::size_t dot = key.find('.', start);
        const std::string part(key.substr(start, dot == std::string_view::npos ? key.size() - start : dot - start));
        if (dot == std::string_view::npos) {
            (*node)[part] = std::move(value);
            return;
        }
        if (!(*node)[part].is_object()) {
            (*node)[part] = nlohmann::json::object();
        }
        node = &(*node)[part];
        start = dot + 1;
    }
}

// Resolves dotted keys ("window.width") through nested objects.
nlohmann::json const* resolveKey(nlohmann::json const& root, std::string_view key) {
    nlohmann::json const* node = &root;
    std::size_t start = 0;
    while (true) {
        const std::size_t dot = key.find('.', start);
        const std::string_view part = key.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
        if (!node->is_object()) {
            return nullptr;
        }
        auto it = node->find(std::string(part));
        if (it == node->end()) {
            return nullptr;
        }
        node = &*it;
        if (dot == std::string_view::npos) {
            return node;
        }
        start = dot + 1;
    }
}
} // namespace


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
            setNested(cfg.values_, key, std::move(parsed));
        } else {
            setNested(cfg.values_, key, value);
        }
        cfg.cli_keys_.push_back(key);
    }

    return cfg;
}

Result<std::string> Config::getString(std::string_view key, std::string fallback) const {
    nlohmann::json const* node = resolveKey(values_, key);
    if (node == nullptr || node->is_null()) {
        return fallback;
    }
    if (!node->is_string()) {
        return makeError(ErrorCode::InvalidArgument,
                         "config key '" + std::string(key) + "' is not a string");
    }
    return node->get<std::string>();
}

Result<int> Config::getInt(std::string_view key, int fallback) const {
    nlohmann::json const* node = resolveKey(values_, key);
    if (node == nullptr || node->is_null()) {
        return fallback;
    }
    if (!node->is_number_integer()) {
        return makeError(ErrorCode::InvalidArgument,
                         "config key '" + std::string(key) + "' is not an integer");
    }
    return node->get<int>();
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
    nlohmann::json const* node = resolveKey(values_, key);
    if (node == nullptr || node->is_null()) {
        return fallback;
    }
    if (!node->is_number()) {
        return makeError(ErrorCode::InvalidArgument,
                         "config key '" + std::string(key) + "' is not a number");
    }
    return node->get<double>();
}

Result<bool> Config::getBool(std::string_view key, bool fallback) const {
    nlohmann::json const* node = resolveKey(values_, key);
    if (node == nullptr || node->is_null()) {
        return fallback;
    }
    if (node->is_boolean()) {
        return node->get<bool>();
    }
    if (node->is_string()) {
        const std::string s = node->get<std::string>();
        if (s == "true" || s == "1") return true;
        if (s == "false" || s == "0") return false;
    }
    if (node->is_number_integer()) {
        return node->get<int>() != 0;
    }
    return makeError(ErrorCode::InvalidArgument,
                     "config key '" + std::string(key) + "' is not a boolean");
}

void Config::set(std::string key, nlohmann::json value) {
    values_[std::move(key)] = std::move(value);
}

} // namespace helios::core
