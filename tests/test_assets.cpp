// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helios/assets/manifest.hpp"
#include "helios/assets/report.hpp"
#include "helios/assets/schema.hpp"
#include "helios/core/hash.hpp"
#include "helios/core/io.hpp"
#include "helios/core/log.hpp"

#include <filesystem>

namespace fs = std::filesystem;
namespace core = helios::core;
using namespace helios::assets;
using namespace helios::core;

TEST_CASE("manifest round trip") {
    AssetManifest manifest;
    manifest.scope_note = "test assets";
    ManifestEntry entry;
    entry.path = "shaders/triangle.spv";
    entry.sha256 = std::string(64, 'a');
    entry.size_bytes = 1234;
    entry.data_class = DataClass::ModelDerived;
    entry.role = "compiled-shader";
    entry.generator = "slangc v2026.14.1";
    manifest.addEntry(entry);

    const nlohmann::json json = manifest.toJson();
    CHECK(json["format"].get<std::string>() == std::string(kFormatAssetManifest));
    CHECK(json["schema_version"] == 1);
    CHECK(json["entries"].size() == 1);

    auto parsed = AssetManifest::fromJson(json);
    REQUIRE(parsed.ok());
    CHECK(parsed.value().entries().size() == 1);
    CHECK(parsed.value().entries()[0].path == "shaders/triangle.spv");
    CHECK(static_cast<int>(parsed.value().entries()[0].data_class) ==
          static_cast<int>(DataClass::ModelDerived));
    CHECK(parsed.value().find("shaders/triangle.spv").has_value());
    CHECK_FALSE(parsed.value().find("nope").has_value());

    // File round trip + atomic write.
    const fs::path tmp = fs::temp_directory_path() / "helios-manifest-test.json";
    REQUIRE(writeManifestFile(tmp.string(), manifest).ok());
    auto text = core::readTextFile(tmp.string());
    REQUIRE(text.ok());
    auto from_file = AssetManifest::fromJson(nlohmann::json::parse(text.value()));
    REQUIRE(from_file.ok());
    CHECK(from_file.value().entries().size() == 1);
    fs::remove(tmp);
}

TEST_CASE("manifest rejects malformed input") {
    // bad magic
    auto bad_magic =
        AssetManifest::fromJson(nlohmann::json{{"format", "nope"}, {"schema_version", 1}, {"entries", {}}});
    CHECK(bad_magic.failed());
    CHECK(static_cast<int>(bad_magic.error().code()) == static_cast<int>(core::ErrorCode::CorruptData));
    // future schema version
    auto future = AssetManifest::fromJson(
        nlohmann::json{{"format", kFormatAssetManifest}, {"schema_version", 999}, {"entries", {}}});
    CHECK(future.failed());
    CHECK(static_cast<int>(future.error().code()) == static_cast<int>(core::ErrorCode::Unsupported));
    // missing entries
    auto no_entries =
        AssetManifest::fromJson(nlohmann::json{{"format", kFormatAssetManifest}, {"schema_version", 1}});
    CHECK(no_entries.failed());
    // bad hash length
    auto bad_hash = AssetManifest::fromJson(nlohmann::json{
        {"format", kFormatAssetManifest},
        {"schema_version", 1},
        {"entries",
         {{"path", "x"}, {"sha256", "short"}, {"size_bytes", 1}, {"class", "measured"}, {"role", "r"}}}});
    CHECK(bad_hash.failed());
    // unknown class
    auto bad_class = AssetManifest::fromJson(nlohmann::json{{"format", kFormatAssetManifest},
                                                            {"schema_version", 1},
                                                            {"entries",
                                                             {{"path", "x"},
                                                              {"sha256", std::string(64, 'a')},
                                                              {"size_bytes", 1},
                                                              {"class", "magic"},
                                                              {"role", "r"}}}});
    CHECK(bad_class.failed());
    // overflow guard: entries count limit
    {
        nlohmann::json huge = nlohmann::json{{"format", kFormatAssetManifest}, {"schema_version", 1}};
        huge["entries"] = nlohmann::json::array_t(1'000'001);
        CHECK(AssetManifest::fromJson(huge).failed());
    }
}

TEST_CASE("manifest verifyAgainst hashes") {
    const fs::path dir = fs::temp_directory_path() / "helios-manifest-verify";
    fs::create_directories(dir);
    const std::string content = "payload-42";
    REQUIRE(core::writeTextFileAtomic((dir / "a.bin").string(), content).ok());

    AssetManifest manifest;
    ManifestEntry good;
    good.path = "a.bin";
    good.sha256 = core::sha256Hex(content);
    good.size_bytes = content.size();
    good.data_class = DataClass::Measured;
    good.role = "test";
    manifest.addEntry(good);

    ManifestEntry missing;
    missing.path = "b.bin";
    missing.sha256 = std::string(64, 'b');
    missing.size_bytes = 1;
    missing.role = "test";
    manifest.addEntry(missing);

    const auto mismatches = manifest.verifyAgainst(dir.string());
    CHECK(mismatches.size() == 1);
    CHECK(mismatches[0].starts_with("b.bin"));
    fs::remove_all(dir);
}

TEST_CASE("data class strings") {
    CHECK(toString(DataClass::Measured) == "measured");
    CHECK(toString(DataClass::Procedural) == "procedural");
    DataClass out{};
    CHECK(fromString("model-derived", out));
    CHECK(static_cast<int>(out) == static_cast<int>(DataClass::ModelDerived));
    CHECK_FALSE(fromString("bogus", out));
}

TEST_CASE("report schemas serialize") {
    EnvironmentInfo env;
    env.helios_version = "0.0.1";
    env.helios_git_hash = "deadbeef";
    env.os_name = "test-os";
    env.resolution = "640x480";
    env.validation_layers = "enabled";
    env.dependency_lock_hash = std::string(64, 'c');

    CapabilityReport cap;
    cap.environment = env;
    cap.body = nlohmann::json{{"device_name", "test-device"}};
    const nlohmann::json cap_json = capabilityReportToJson(cap);
    CHECK(cap_json["format"].get<std::string>() == std::string(kFormatCapabilityReport));
    CHECK(cap_json["schema_version"] == kSchemaVersionCapabilityReport);
    CHECK(cap_json["device"]["device_name"] == "test-device");

    PerformanceReport perf;
    perf.environment = env;
    perf.cpu_frame_ms = {100, 5.0, 8.0, 10.0, 20.0, 4.0, 6.0};
    const nlohmann::json perf_json = performanceReportToJson(perf);
    CHECK(perf_json["cpu_frame_ms"]["p50_ms"] == 5.0);
    CHECK(perf_json["cpu_frame_ms"]["frame_count"] == 100);

    EvidenceReport evidence;
    evidence.environment = env;
    SmokePathResult r;
    r.name = "clear";
    r.status = "pass";
    r.detail = "ok";
    r.metrics = {{"ms", 1.0}};
    evidence.smoke_paths.push_back(r);
    evidence.validation_error_count = 0;
    const nlohmann::json ev_json = evidenceReportToJson(evidence);
    CHECK(ev_json["smoke_paths"][0]["status"] == "pass");
    CHECK(ev_json["validation_error_count"] == 0);
}
