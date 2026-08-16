// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// helios_render::Renderer — the scene-assembly boundary. The app hands it
// an opaque SDL window; helios_vk owns all Vulkan/WSI lifetime. The app
// has no direct Vulkan link edge (docs/ARCHITECTURE.md).
#pragma once

#include "helios/core/error.hpp"
#include "helios/core/timer.hpp"

#include <cstdint>
#include <memory>
#include <string>

struct SDL_Window;

namespace helios::render {

struct RendererCreateInfo {
    SDL_Window* window = nullptr;   // opaque surface source
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    bool enable_validation = true;
    bool enable_imgui = true;
    std::string shader_dir;         // where compiled .spv files live
    std::string pipeline_cache_file; // optional persistent pipeline cache
    std::uint32_t max_frames = 0;   // 0 = run until window closes
    std::uint32_t swapchain_image_count = 2; // frames in flight
};

struct RendererStats {
    core::FrameStats cpu_frame_ms;   // CPU time per rendered frame
    core::FrameStats gpu_frame_ms;   // GPU time (timestamp queries)
    std::uint64_t presented_frames = 0;
};

class Renderer {
public:
    ~Renderer();
    Renderer(Renderer const&) = delete;
    Renderer& operator=(Renderer const&) = delete;

    [[nodiscard]] static core::Result<std::unique_ptr<Renderer>> create(RendererCreateInfo const& info);

    // Renders one frame; returns false when the loop should stop
    // (window closed, max_frames reached, device loss).
    [[nodiscard]] bool renderFrame();

    [[nodiscard]] RendererStats const& stats() const noexcept { return stats_; }

    // JSON device report (for the capability report file).
    [[nodiscard]] std::string deviceReportJson() const;

    // Populated per-frame by the app via debugUi() (ImGui context).
    struct DebugUiFrame {
        double cpu_frame_ms = 0.0;
        double gpu_frame_ms = 0.0;
        std::uint64_t frame_index = 0;
        std::string device_name;
        std::string api_version;
        std::string driver_info;
        std::uint32_t validation_errors = 0;
        std::uint32_t validation_warnings = 0;
        std::string depth_choice;
        std::string note; // e.g. benchmark mode
    };
    void setDebugUiFrame(DebugUiFrame frame) { debug_ui_frame_ = frame; }

    // Validation-layer stats since creation.
    [[nodiscard]] std::uint32_t validationErrorCount() const;
    [[nodiscard]] std::uint32_t validationWarningCount() const;

    [[nodiscard]] std::string const& deviceName() const noexcept { return device_name_; }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

private:
    Renderer() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    RendererStats stats_;
    DebugUiFrame debug_ui_frame_;
    std::string device_name_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};

} // namespace helios::render
