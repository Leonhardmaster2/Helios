// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Asset manifest: versioned list of assets with SHA-256, size, provenance
// class and role. Written atomically; readers validate magic, schema
// version, counts and hash strings (schemas/asset-manifest.schema.json).
#pragma once

#include "helios/assets/schema.hpp"
#include "helios/core/error.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace helios::assets {

struct ManifestEntry {
    std::string path;   // relative path within the manifest scope
    std::string sha256; // lowercase hex, 64 chars
    std::uint64_t size_bytes = 0;
    DataClass data_class = DataClass::ModelDerived;
    std::string role;                     // e.g. "compiled-shader", "terrain-tile"
    std::optional<std::string> generator; // tool/version that produced it
};

class AssetManifest {
  public:
    static constexpr std::uint32_t kCurrentVersion = kSchemaVersionAssetManifest;

    [[nodiscard]] std::uint32_t schemaVersion() const noexcept {
        return version_;
    }
    void setSchemaVersion(std::uint32_t v) noexcept {
        version_ = v;
    }

    // Generic provenance note for the manifest scope (e.g. dataset DOI).
    std::string scope_note;

    void addEntry(ManifestEntry entry);
    [[nodiscard]] std::vector<ManifestEntry> const& entries() const noexcept {
        return entries_;
    }

    // Serialize to JSON (indented, stable key order).
    [[nodiscard]] nlohmann::json toJson() const;

    // Parse from JSON; validates magic/schema version and entry fields.
    [[nodiscard]] static core::Result<AssetManifest> fromJson(nlohmann::json const& json);

    // Find an entry by relative path.
    [[nodiscard]] std::optional<ManifestEntry> find(std::string_view path) const;

    // Verify every entry hash against the actual files under base_dir.
    // Returns the list of mismatched entries (empty == all match).
    [[nodiscard]] std::vector<std::string> verifyAgainst(std::string_view base_dir) const;

  private:
    std::uint32_t version_ = kCurrentVersion;
    std::vector<ManifestEntry> entries_;
};

// Convenience: write manifest JSON atomically.
[[nodiscard]] core::Result<void> writeManifestFile(std::string_view path, AssetManifest const& manifest);

} // namespace helios::assets
