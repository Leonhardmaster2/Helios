// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// ImGui debug UI: device report, frame-time percentiles, validation stats.
#include "helios/render/renderer.hpp"

#include "helios/core/log.hpp"

#ifdef HELIOS_ENABLE_IMGUI
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>
#endif

namespace helios::render {

#ifdef HELIOS_ENABLE_IMGUI
namespace {
// Live frame-time history for a small sparkline.
struct FrameHistory {
    std::array<float, 120> samples{};
    std::size_t count = 0;
    void add(double ms) {
        if (count < samples.size()) samples[count++] = static_cast<float>(ms);
    }
};
FrameHistory g_history;

void drawPercentileTable(char const* title, core::FrameStats const& stats) {
    ImGui::Text("%s: p50 %.3f ms | p95 %.3f ms | p99 %.3f ms | max %.3f ms | avg %.3f ms", title,
                stats.percentile(50.0), stats.percentile(95.0), stats.percentile(99.0), stats.max(),
                stats.average());
}
} // namespace
#endif

void Renderer::drawDebugUi() {
#ifdef HELIOS_ENABLE_IMGUI
    const auto& ui = debug_ui_frame_;
    g_history.add(ui.cpu_frame_ms);

    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420, 300), ImGuiCond_FirstUseEver);
    ImGui::Begin("Helios M0 — capability spike");
    ImGui::Text("Device: %s", ui.device_name.c_str());
    ImGui::Text("API: %s | driver: %s", ui.api_version.c_str(), ui.driver_info.c_str());
    ImGui::Text("Depth: %s | validation errors: %u, warnings: %u", ui.depth_choice.c_str(),
                ui.validation_errors, ui.validation_warnings);
    ImGui::Separator();
    ImGui::Text("Frame %llu | CPU %.3f ms | GPU %.3f ms", static_cast<unsigned long long>(ui.frame_index),
                ui.cpu_frame_ms, ui.gpu_frame_ms);
    if (!g_history.samples.empty()) {
        ImGui::PlotLines("CPU ms", g_history.samples.data(), static_cast<int>(g_history.count), 0, nullptr,
                         FLT_MAX, FLT_MAX, ImVec2(0, 60));
    }
    if (!ui.note.empty()) {
        ImGui::Text("Note: %s", ui.note.c_str());
    }
    ImGui::TextWrapped("Controls: drag to orbit, wheel to zoom, ESC/Q to quit, F11 toggles fullscreen");
    ImGui::End();
#endif
}

} // namespace helios::render
