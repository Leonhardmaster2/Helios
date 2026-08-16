// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helios/core/config.hpp"
#include "helios/core/debug_name.hpp"
#include "helios/core/error.hpp"
#include "helios/core/hash.hpp"
#include "helios/core/io.hpp"
#include "helios/core/timer.hpp"
#include "helios/version.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace helios::core;

TEST_CASE("version basics") {
    const helios::HeliosVersion v = helios::heliosVersion();
    CHECK(v.major == 0);
    CHECK(v.minor == 1);
    CHECK_FALSE(helios::heliosGitHash().empty());
    CHECK_FALSE(helios::heliosVersionString().empty());
}

TEST_CASE("result error propagation") {
    Result<int> ok = 42;
    CHECK(ok.ok());
    CHECK(ok.value() == 42);
    CHECK(ok.valueOr(7) == 42);

    Result<int> err = makeError(ErrorCode::NotFound, "missing");
    CHECK(err.failed());
    CHECK(static_cast<int>(err.error().code()) == static_cast<int>(ErrorCode::NotFound));
    CHECK(err.valueOr(7) == 7);

    Result<void> void_ok;
    CHECK(void_ok.ok());
    Result<void> void_err = makeError(ErrorCode::Io, "io failed");
    CHECK(void_err.failed());
    CHECK(static_cast<int>(void_err.error().code()) == static_cast<int>(ErrorCode::Io));
}

TEST_CASE("sha256 NIST vectors") {
    // FIPS 180-4 test vectors.
    CHECK(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // Two-block message (exactly 56 bytes) — NIST FIPS 180-4 vector.
    // Built from chunks to avoid transcription errors.
    const std::string two_block = "abcd"
                                  "bcde"
                                  "cdef"
                                  "defg"
                                  "efgh"
                                  "fghi"
                                  "ghij"
                                  "hijk"
                                  "ijkl"
                                  "jklm"
                                  "klmn"
                                  "lmno"
                                  "mnop"
                                  "nopq";
    CHECK(two_block.size() == 56);
    CHECK(sha256Hex(two_block) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // "a" x 1,000,000
    std::string million_a(1'000'000, 'a');
    CHECK(sha256Hex(million_a) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    // Streaming equivalence.
    Sha256 streamed;
    streamed.update("ab", 2);
    streamed.update("c", 1);
    const auto digest = streamed.finalize();
    std::string hex;
    for (std::uint8_t b : digest) {
        static constexpr char kHex[] = "0123456789abcdef";
        hex.push_back(kHex[b >> 4]);
        hex.push_back(kHex[b & 0x0f]);
    }
    CHECK(hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("config layered parsing") {
    const std::vector<std::string> cli = {"--headless-smoke", "--width=640", "--frames",    "120",
                                          "--flag-only",      "--ratio=0.5", "--name=hello"};
    auto cfg = Config::load("", cli);
    REQUIRE(cfg.ok());
    CHECK(cfg.value().getBool("headless-smoke", false).value());
    CHECK(cfg.value().getUInt("width", 0).value() == 640);
    CHECK(cfg.value().getUInt("frames", 0).value() == 120);
    CHECK(cfg.value().getBool("flag-only", false).value());
    CHECK(cfg.value().getDouble("ratio", 0.0).value() == doctest::Approx(0.5));
    CHECK(cfg.value().getString("name", "").value() == "hello");
    CHECK(cfg.value().getInt("missing", 5).value() == 5);
    // wrong type -> error
    auto bad = cfg.value().getInt("name", 0);
    CHECK(bad.failed());
    CHECK(static_cast<int>(bad.error().code()) == static_cast<int>(ErrorCode::InvalidArgument));
}

TEST_CASE("config from file") {
    const fs::path tmp = fs::temp_directory_path() / "helios-config-test.json";
    {
        std::ofstream out(tmp);
        out << R"({"window": {"width": 800}, "enabled": true})";
    }
    auto cfg = Config::load(tmp.string(), {"--window.height=600"});
    REQUIRE(cfg.ok());
    CHECK(cfg.value().getUInt("window.width", 0).value() == 800);
    CHECK(cfg.value().getUInt("window.height", 0).value() == 600);
    CHECK(cfg.value().getBool("enabled", false).value());
    fs::remove(tmp);

    auto bad = Config::load(tmp.string(), {});
    CHECK(bad.failed()); // file removed -> Io error
}

TEST_CASE("checked io") {
    const fs::path tmp = fs::temp_directory_path() / "helios-io-test.bin";
    const std::string payload = "hello helios\n\x01\x02";
    REQUIRE(writeTextFileAtomic(tmp.string(), payload).ok());
    auto text = readTextFile(tmp.string());
    REQUIRE(text.ok());
    CHECK(text.value() == payload);
    // size limit enforcement
    auto limited = readTextFile(tmp.string(), 4);
    CHECK(limited.failed());
    CHECK(static_cast<int>(limited.error().code()) == static_cast<int>(ErrorCode::OutOfRange));
    fs::remove(tmp);
    auto missing = readTextFile(tmp.string());
    CHECK(missing.failed());
    CHECK(static_cast<int>(missing.error().code()) == static_cast<int>(ErrorCode::Io));
}

TEST_CASE("debug names") {
    const std::string n1 = makeDebugName("image", 7);
    CHECK(n1 == "image[7]");
    const std::string n2 = makeAutoDebugName("buffer");
    const std::string n3 = makeAutoDebugName("buffer");
    CHECK(n2 != n3);
    CHECK(n2.starts_with("buffer["));
    CHECK(sanitizeDebugName("9bad name!") == "bad_name_");
    CHECK(sanitizeDebugName("") == "unnamed");
}

TEST_CASE("frame stats percentiles") {
    FrameStats stats;
    for (int i = 1; i <= 100; ++i) {
        stats.add(static_cast<double>(i) / 1000.0);
    }
    CHECK(stats.count() == 100);
    CHECK(stats.percentile(50.0) == doctest::Approx(0.050));
    CHECK(stats.percentile(95.0) == doctest::Approx(0.095));
    CHECK(stats.percentile(99.0) == doctest::Approx(0.099));
    CHECK(stats.max() == doctest::Approx(0.100));
    CHECK(stats.min() == doctest::Approx(0.001));
    CHECK(stats.average() == doctest::Approx(0.0505));
    CHECK(FrameStats().count() == 0);
    CHECK(FrameStats().max() == doctest::Approx(0.0));
}
