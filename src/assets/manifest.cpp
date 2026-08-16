// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/assets/manifest.hpp"

#include "helios/core/hash.hpp"
#include "helios/core/io.hpp"

#include <algorithm>
#include <filesystem>
#include <format>

namespace helios::assets {

namespace fs = std::filesystem;

std::string_view toString(DataClass cls) noexcept {
    switch (cls) {
    case DataClass::Measured:
        return "measured";
    case DataClass::ModelDerived:
        return "model-derived";
    case DataClass::Procedural:
        return "procedural";
    case DataClass::Display:
        return "display";
    }
    return "unknown";
}

bool fromString(std::string_view s, DataClass& out) noexcept {
    if (s == "measured") {
        out = DataClass::Measured;
        return true;
    }
    if (s == "model-derived") {
        out = DataClass::ModelDerived;
        return true;
    }
    if (s == "procedural") {
        out = DataClass::Procedural;
        return true;
    }
    if (s == "display") {
        out = DataClass::Display;
        return true;
    }
    return false;
}

void AssetManifest::addEntry(ManifestEntry entry) {
    entries_.push_back(std::move(entry));
}

nlohmann::json AssetManifest::toJson() const {
    nlohmann::json out = nlohmann::json::object();
    out["format"] = kFormatAssetManifest;
    out["schema_version"] = version_;
    if (!scope_note.empty()) {
        out["scope_note"] = scope_note;
    }
    nlohmann::json arr = nlohmann::json::array();
    for (auto const& e : entries_) {
        nlohmann::json j = nlohmann::json::object();
        j["path"] = e.path;
        j["sha256"] = e.sha256;
        j["size_bytes"] = e.size_bytes;
        j["class"] = toString(e.data_class);
        j["role"] = e.role;
        if (e.generator) {
            j["generator"] = *e.generator;
        }
        arr.push_back(std::move(j));
    }
    out["entries"] = std::move(arr);
    return out;
}

core::Result<AssetManifest> AssetManifest::fromJson(nlohmann::json const& json) {
    if (!json.is_object()) {
        return core::makeError(core::ErrorCode::CorruptData, "asset manifest: root is not an object");
    }
    auto fmt = json.find("format");
    if (fmt == json.end() || !fmt->is_string() || fmt->get<std::string>() != kFormatAssetManifest) {
        return core::makeError(core::ErrorCode::CorruptData, "asset manifest: bad or missing format magic");
    }
    auto ver = json.find("schema_version");
    if (ver == json.end() || !(ver->is_number_integer() || ver->is_number_unsigned())) {
        return core::makeError(core::ErrorCode::CorruptData, "asset manifest: missing schema_version");
    }
    const std::uint32_t version = ver->get<std::uint32_t>();
    if (version > kCurrentVersion) {
        return core::makeError(core::ErrorCode::Unsupported,
                               std::format("asset manifest: schema version {} is newer than supported {}",
                                           version, kCurrentVersion));
    }
    AssetManifest manifest;
    manifest.version_ = version;
    auto note = json.find("scope_note");
    if (note != json.end() && note->is_string()) {
        manifest.scope_note = note->get<std::string>();
    }
    auto entries = json.find("entries");
    if (entries == json.end() || !entries->is_array()) {
        return core::makeError(core::ErrorCode::CorruptData, "asset manifest: missing entries array");
    }
    if (entries->size() > 1'000'000) {
        return core::makeError(core::ErrorCode::OutOfRange, "asset manifest: entries count exceeds limit");
    }
    for (auto const& e : *entries) {
        if (!e.is_object()) {
            return core::makeError(core::ErrorCode::CorruptData, "asset manifest: entry is not an object");
        }
        ManifestEntry entry;
        auto path = e.find("path");
        auto sha = e.find("sha256");
        auto size = e.find("size_bytes");
        auto cls = e.find("class");
        auto role = e.find("role");
        if (path == e.end() || !path->is_string() || sha == e.end() || !sha->is_string() || size == e.end() ||
            !size->is_number_unsigned() || cls == e.end() || !cls->is_string() || role == e.end() ||
            !role->is_string()) {
            return core::makeError(core::ErrorCode::CorruptData, "asset manifest: malformed entry");
        }
        entry.path = path->get<std::string>();
        entry.sha256 = sha->get<std::string>();
        entry.size_bytes = size->get<std::uint64_t>();
        if (!fromString(cls->get<std::string>(), entry.data_class)) {
            return core::makeError(core::ErrorCode::CorruptData,
                                   "asset manifest: unknown class '" + cls->get<std::string>() + "'");
        }
        entry.role = role->get<std::string>();
        if (entry.sha256.size() != 64) {
            return core::makeError(core::ErrorCode::CorruptData,
                                   "asset manifest: sha256 must be 64 hex chars");
        }
        auto gen = e.find("generator");
        if (gen != e.end() && gen->is_string()) {
            entry.generator = gen->get<std::string>();
        }
        manifest.entries_.push_back(std::move(entry));
    }
    return manifest;
}

std::optional<ManifestEntry> AssetManifest::find(std::string_view path) const {
    for (auto const& e : entries_) {
        if (e.path == path) {
            return e;
        }
    }
    return std::nullopt;
}

std::vector<std::string> AssetManifest::verifyAgainst(std::string_view base_dir) const {
    std::vector<std::string> mismatches;
    for (auto const& e : entries_) {
        const fs::path full = fs::path(std::string(base_dir)) / e.path;
        std::error_code ec;
        if (!fs::exists(full, ec)) {
            mismatches.push_back(e.path + ": missing");
            continue;
        }
        auto hash = core::sha256File(full.string());
        const auto size = fs::file_size(full, ec);
        if (hash.failed() || hash.value() != e.sha256 || size != e.size_bytes) {
            mismatches.push_back(e.path + ": hash/size mismatch");
        }
    }
    return mismatches;
}

core::Result<void> writeManifestFile(std::string_view path, AssetManifest const& manifest) {
    const std::string text = manifest.toJson().dump(2);
    return core::writeTextFileAtomic(path, text);
}

} // namespace helios::assets
