// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// M1 GPU precision lab: offscreen Vulkan renderer consuming GpuFramePlan
// draw lists through two push-constant pipelines:
//   - mesh pipeline: near geometry, drawing NearRenderRecord float32 offsets
//     directly (rotation-only view; see gpu_precision_plan.hpp);
//   - impostor pipeline: fullscreen triangle + analytic disc from a
//     DistantBodyRecord direction/angular-radius/depth.
// Color and depth (D32_SFLOAT) are read back after each frame. Also hosts a
// minimal SDL viewer so developers can watch the deterministic route.
#pragma once

#include "helios/core/error.hpp"
#include "helios/render/gpu_precision_plan.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace helios::render {

struct HostFrame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> color;  // RGBA8, tight rows
    std::vector<float> depth;         // D32_SFLOAT values, tight rows
};

class GPUPrecisionTarget {
  public:
    ~GPUPrecisionTarget();
    GPUPrecisionTarget(GPUPrecisionTarget const&) = delete;
    GPUPrecisionTarget& operator=(GPUPrecisionTarget const&) = delete;

    [[nodiscard]] static core::Result<std::unique_ptr<GPUPrecisionTarget>>
    create(bool enable_validation, std::uint32_t width, std::uint32_t height);

    // Renders one frame: clear (reversed-Z: depth cleared to 0), draws all
    // impostors and meshes with depth test GREATER / write on, waits, then
    // reads back color+depth into out_frame.
    [[nodiscard]] core::Result<void> renderAndReadback(GpuFramePlan const& plan,
                                                       std::array<float, 4> const& clear_color,
                                                       HostFrame& out_frame);

    [[nodiscard]] std::uint32_t validationErrors() const noexcept;
    [[nodiscard]] std::uint32_t validationWarnings() const noexcept;
    [[nodiscard]] std::string deviceName() const;

  private:
    GPUPrecisionTarget() = default;
    struct Impl;
    Impl* impl_ = nullptr;
};

// Minimal SDL window that displays HostFrame buffers (blitted via a
// streaming texture). SPACE advances the route manually, ESC closes.
class FrameViewer {
  public:
    ~FrameViewer();
    FrameViewer(FrameViewer const&) = delete;
    FrameViewer& operator=(FrameViewer const&) = delete;

    [[nodiscard]] static core::Result<std::unique_ptr<FrameViewer>> create(std::string_view title,
                                                                           std::uint32_t width,
                                                                           std::uint32_t height);
    // Presents the frame and pumps events. Returns false when closed.
    [[nodiscard]] bool show(HostFrame const& frame);
    [[nodiscard]] bool closeRequested() const noexcept;

  private:
    FrameViewer() = default;
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace helios::render
