// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Versioned binary/JSON schema identity for Helios assets and reports.
// Every format carries magic + schema_version; readers must reject
// unsupported versions (docs/ARCHITECTURE.md — Data and file formats).
#pragma once

#include <cstdint>
#include <string_view>

namespace helios::assets {

// Current report/manifest schema versions (bump on incompatible change).
inline constexpr std::uint32_t kSchemaVersionCapabilityReport = 1;
inline constexpr std::uint32_t kSchemaVersionPerformanceReport = 1;
inline constexpr std::uint32_t kSchemaVersionEvidenceReport = 1;
inline constexpr std::uint32_t kSchemaVersionAssetManifest = 1;
inline constexpr std::uint32_t kSchemaVersionShaderManifest = 1;

inline constexpr std::string_view kFormatCapabilityReport = "helios-capability-report";
inline constexpr std::string_view kFormatPerformanceReport = "helios-performance-report";
inline constexpr std::string_view kFormatEvidenceReport = "helios-evidence-report";
inline constexpr std::string_view kFormatAssetManifest = "helios-asset-manifest";

// Provenance classes (docs/DATA_AND_PROVENANCE.md).
enum class DataClass : std::uint8_t {
    Measured = 0,      // instrument/survey product with recorded provenance
    ModelDerived = 1,  // evaluated from a named mathematical model
    Procedural = 2,    // generated plausible detail — never "measured"
    Display = 3,       // presentation transform, no source accuracy added
};

[[nodiscard]] std::string_view toString(DataClass cls) noexcept;
[[nodiscard]] bool fromString(std::string_view s, DataClass& out) noexcept;

} // namespace helios::assets
