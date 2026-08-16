// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// CPU-only helios_vk tests (no device required). GPU behavior is covered
// by the headless smoke suite (helios --headless-smoke).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helios/vk/device.hpp"

#include <nlohmann/json.hpp>

TEST_CASE("device report serialization") {
    helios::vk::DeviceReport report;
    report.device_name = "test-gpu";
    report.vendor_id = 0x10DE;
    report.device_id = 0x1234;
    report.api_version = VK_API_VERSION_1_3;
    report.driver_info = "test driver";
    report.timestamp_period_ns = 1.0f;
    report.timestamp_compute_and_graphics = true;
    report.depth_d32_sfloat = true;
    report.depth_choice = helios::vk::DepthFormatChoice::D32Sfloat;
    report.depth_choice_name = "D32_SFLOAT";
    report.used_queue_family = 0;
    report.queue_families.push_back({0, 1,
                                     VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT,
                                     true, true, true, true, 64});
    report.selection_reason = "test";

    const nlohmann::json json = helios::vk::deviceReportToJson(report);
    CHECK(json["device_name"] == "test-gpu");
    CHECK(json["vendor_id"] == 0x10DE);
    CHECK(json["features"]["dynamic_rendering"] == false);
    CHECK(json["depth_formats"]["D32_SFLOAT"] == true);
    CHECK(json["depth_formats"]["choice"] == "D32_SFLOAT");
    CHECK(json["queue_families"][0]["graphics"] == true);
    CHECK(json["timestamp_period_ns"] == 1.0f);
}

TEST_CASE("instance api version formatting") {
    CHECK(helios::vk::Instance::apiVersionString(VK_API_VERSION_1_3) == "1.3.0");
    CHECK(helios::vk::Instance::apiVersionString(VK_MAKE_API_VERSION(0, 1, 4, 357)) == "1.4.357");
}
