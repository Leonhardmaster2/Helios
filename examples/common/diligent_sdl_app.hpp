// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Minimal SDL3 host for the Vulkan-only Diligent examples.  It intentionally
// owns exactly one factory/device/immediate-context/swap-chain tuple and does
// not depend on Helios's legacy helios_vk or helios_render modules.
#pragma once

#include "EngineFactoryVk.h"
#include "ImGuiImplSDL3.hpp"
#include "RefCntAutoPtr.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace helios::examples {

struct DiligentSdlCreateInfo {
    char const* title = "Helios Diligent lab";
    int width = 1600;
    int height = 900;
    bool enable_validation = true;
    // Creates the window without ever showing it. The swap chain, ImGui and
    // event paths still run, which lets a bounded smoke route exercise the
    // interactive code without putting anything on the desktop.
    bool hidden = false;
};

struct DiligentDiagnostic {
    Diligent::DEBUG_MESSAGE_SEVERITY severity = Diligent::DEBUG_MESSAGE_SEVERITY_INFO;
    std::string text;
};

class DiligentSdlApp {
  public:
    ~DiligentSdlApp();
    DiligentSdlApp(DiligentSdlApp const&) = delete;
    DiligentSdlApp& operator=(DiligentSdlApp const&) = delete;

    [[nodiscard]] static std::unique_ptr<DiligentSdlApp>
    create(DiligentSdlCreateInfo const& info, std::string& error);

    // Every SDL event must pass through this function before application input
    // handling.  ImGui's capture flags can then gate camera controls.
    void forwardEvent(SDL_Event const& event);
    [[nodiscard]] bool wantsMouseCapture() const noexcept;
    [[nodiscard]] bool wantsKeyboardCapture() const noexcept;

    // Synchronizes the Diligent swap chain to SDL's pixel size. Returns false
    // while the window is minimized or otherwise has a zero drawable extent.
    [[nodiscard]] bool synchronizeDrawableSize();

    void beginImGuiFrame();
    void bindAndClearBackBuffer(float const clear_color[4], float reversed_depth_clear = 0.0f);
    void setSceneViewport(float render_scale);
    void renderImGui();
    void present(bool v_sync);

    [[nodiscard]] SDL_Window* window() const noexcept { return window_; }
    [[nodiscard]] Diligent::IRenderDevice* device() const noexcept { return device_; }
    [[nodiscard]] Diligent::IDeviceContext* context() const noexcept { return context_; }
    [[nodiscard]] Diligent::ISwapChain* swapChain() const noexcept { return swap_chain_; }
    [[nodiscard]] Diligent::ImGuiImplSDL3* imgui() const noexcept { return imgui_.get(); }
    [[nodiscard]] std::uint32_t width() const noexcept;
    [[nodiscard]] std::uint32_t height() const noexcept;
    // Display content scale applied to ImGui's fonts and metrics (1 at 100 %).
    [[nodiscard]] float uiScale() const noexcept { return ui_scale_; }
    [[nodiscard]] std::string adapterName() const;
    [[nodiscard]] bool durationQueriesSupported() const noexcept;

    [[nodiscard]] std::uint32_t warningCount() const noexcept;
    [[nodiscard]] std::uint32_t errorCount() const noexcept;
    [[nodiscard]] std::vector<DiligentDiagnostic> diagnosticsSnapshot() const;

  private:
    DiligentSdlApp() = default;
    [[nodiscard]] bool initialize(DiligentSdlCreateInfo const& info, std::string& error);
    void recordDiagnostic(Diligent::DEBUG_MESSAGE_SEVERITY severity, char const* message);

    static void DILIGENT_CALL_TYPE messageCallback(Diligent::DEBUG_MESSAGE_SEVERITY severity,
                                                    Diligent::Char const* message,
                                                    Diligent::Char const* function,
                                                    Diligent::Char const* file,
                                                    Diligent::Int32 line);

    inline static DiligentSdlApp* active_instance_ = nullptr;

    bool owns_sdl_ = false;
    float ui_scale_ = 1.0f;
    SDL_Window* window_ = nullptr;
    Diligent::IEngineFactoryVk* factory_ = nullptr; // Diligent process singleton
    Diligent::RefCntAutoPtr<Diligent::IRenderDevice> device_;
    Diligent::RefCntAutoPtr<Diligent::IDeviceContext> context_;
    Diligent::RefCntAutoPtr<Diligent::ISwapChain> swap_chain_;
    std::unique_ptr<Diligent::ImGuiImplSDL3> imgui_;

    mutable std::mutex diagnostics_mutex_;
    std::vector<DiligentDiagnostic> diagnostics_;
    std::uint32_t warning_count_ = 0;
    std::uint32_t error_count_ = 0;
};

} // namespace helios::examples
