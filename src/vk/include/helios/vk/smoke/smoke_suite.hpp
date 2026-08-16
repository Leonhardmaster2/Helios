// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// M0 smoke suite: clear, triangle, depth, compute, transfer/upload and GPU
// timestamp paths. Each path returns pass/fail with machine-readable
// metrics. Numeric verification runs headless (offscreen + readback);
// windowed mode renders the same paths to the swapchain for visual and
// validation-layer checks.
#pragma once

#include "helios/core/error.hpp"
#include "helios/vk/device.hpp"
#include "helios/vk/smoke/smoke_context.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <vulkan/vulkan.h>

namespace helios::vk::smoke {

struct SmokeOptions {
    bool clear = true;
    bool triangle = true;
    bool depth = true;
    bool compute = true;
    bool transfer = true;
    bool timestamp = true;
    std::uint32_t iterations = 3;   // repetitions per path
    std::uint32_t width = 640;
    std::uint32_t height = 480;
};

struct SmokePathResult {
    std::string name;
    bool pass = false;
    std::string detail;       // evidence summary
    nlohmann::json metrics;   // measurements (timings, checks)
};

// Runs the full suite headless (offscreen target; no window/swapchain).
class HeadlessSmoke {
public:
    ~HeadlessSmoke();
    HeadlessSmoke(HeadlessSmoke const&) = delete;
    HeadlessSmoke& operator=(HeadlessSmoke const&) = delete;

    [[nodiscard]] static core::Result<std::unique_ptr<HeadlessSmoke>> create(
        Instance& instance, Device& device, SmokeOptions const& options);

    // Runs all requested paths; returns per-path results (never throws).
    [[nodiscard]] std::vector<SmokePathResult> run();

    [[nodiscard]] SmokeContext& context() noexcept { return *context_; }

private:
    HeadlessSmoke() = default;
    void destroy();

    Device* device_ = nullptr;
    SmokeOptions options_;
    std::unique_ptr<SmokeContext> context_;
    std::vector<SmokePathResult> results_;
};

// Records the shared windowed smoke scene (clear + triangle + depth probes)
// into an already-started dynamic-rendering pass on the given image.
// Used by helios_render for the interactive window path.
class WindowedSmokeScene {
public:
    ~WindowedSmokeScene();
    WindowedSmokeScene(WindowedSmokeScene const&) = delete;
    WindowedSmokeScene& operator=(WindowedSmokeScene const&) = delete;

    [[nodiscard]] static core::Result<std::unique_ptr<WindowedSmokeScene>> create(
        Device& device, VkFormat color_format, VkFormat depth_format);

    // Records clear + triangle + depth probes. Pass must already have
    // color+depth attachments bound via dynamic rendering.
    void record(VkCommandBuffer cmd, VkExtent2D extent, std::uint32_t frame_index);

private:
    WindowedSmokeScene() = default;
    void destroy();

    Device* device_ = nullptr;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace helios::vk::smoke
