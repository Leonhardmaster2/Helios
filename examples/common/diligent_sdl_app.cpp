// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "diligent_sdl_app.hpp"

#include "NativeWindow.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <utility>

namespace helios::examples {

std::unique_ptr<DiligentSdlApp> DiligentSdlApp::create(DiligentSdlCreateInfo const& info,
                                                       std::string& error) {
    if (active_instance_ != nullptr) {
        error = "only one Diligent SDL device/context/swap chain is allowed per process";
        return nullptr;
    }

    auto result = std::unique_ptr<DiligentSdlApp>{new DiligentSdlApp{}};
    active_instance_ = result.get();
    if (!result->initialize(info, error)) {
        result.reset();
        return nullptr;
    }
    return result;
}

bool DiligentSdlApp::initialize(DiligentSdlCreateInfo const& info, std::string& error) {
#if !PLATFORM_WIN32
    error = "the v0.0.1 SDL/Diligent host currently implements the Win32 native-window bridge only";
    return false;
#else
    if (info.width <= 0 || info.height <= 0) {
        error = "initial window dimensions must be positive";
        return false;
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        error = std::string{"SDL_Init failed: "} + SDL_GetError();
        return false;
    }
    owns_sdl_ = true;

    // No SDL mouse capture, relative mouse mode, global hook, or screen capture
    // is used. Camera input is ordinary focused-window input only.
    SDL_WindowFlags window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (info.hidden) {
        window_flags |= SDL_WINDOW_HIDDEN;
    }
    window_ = SDL_CreateWindow(info.title, info.width, info.height, window_flags);
    if (window_ == nullptr) {
        error = std::string{"SDL_CreateWindow failed: "} + SDL_GetError();
        return false;
    }

    factory_ = Diligent::GetEngineFactoryVk();
    if (factory_ == nullptr) {
        error = "Diligent's statically linked Vulkan factory is unavailable";
        return false;
    }
    factory_->SetMessageCallback(messageCallback);

    Diligent::EngineVkCreateInfo engine_info;
    engine_info.EnableValidation = info.enable_validation;
    // Dynamic buffers take their full size from this heap at every map; the
    // planet's bodies each map a 2 MB patch buffer per frame and the gas
    // solvers map theirs, so the default 8 MB stalls or fails.
    engine_info.DynamicHeapSize = 64u << 20;
    factory_->CreateDeviceAndContextsVk(engine_info, &device_, &context_);
    if (device_ == nullptr || context_ == nullptr) {
        error = "Diligent failed to create its Vulkan render device and immediate context";
        return false;
    }
    if (device_->GetDeviceInfo().Type != Diligent::RENDER_DEVICE_TYPE_VULKAN) {
        error = "the Diligent lab was configured with a non-Vulkan render device";
        return false;
    }

    SDL_PropertiesID const properties = SDL_GetWindowProperties(window_);
    void* const hwnd = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (hwnd == nullptr) {
        error = std::string{"SDL did not expose the Win32 HWND: "} + SDL_GetError();
        return false;
    }

    int pixel_width = 0;
    int pixel_height = 0;
    if (!SDL_GetWindowSizeInPixels(window_, &pixel_width, &pixel_height) || pixel_width <= 0 ||
        pixel_height <= 0) {
        error = std::string{"SDL_GetWindowSizeInPixels failed: "} + SDL_GetError();
        return false;
    }

    Diligent::SwapChainDesc swap_desc;
    swap_desc.Width = static_cast<Diligent::Uint32>(pixel_width);
    swap_desc.Height = static_cast<Diligent::Uint32>(pixel_height);
    swap_desc.ColorBufferFormat = Diligent::TEX_FORMAT_RGBA8_UNORM_SRGB;
    swap_desc.DepthBufferFormat = Diligent::TEX_FORMAT_D32_FLOAT;
    swap_desc.BufferCount = 3;
    swap_desc.DefaultDepthValue = 0.0f; // reversed-Z contract

    Diligent::Win32NativeWindow const native_window{hwnd};
    factory_->CreateSwapChainVk(device_, context_, swap_desc, native_window, &swap_chain_);
    if (swap_chain_ == nullptr) {
        error = "Diligent failed to create the Vulkan swap chain for the SDL window";
        return false;
    }

    try {
        Diligent::ImGuiDiligentCreateInfo const imgui_info{device_, swap_chain_->GetDesc()};
        imgui_ = Diligent::ImGuiImplSDL3::Create(imgui_info, window_);
        ImGui::StyleColorsDark();
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        // The swap chain is sized in pixels, so without this the UI shrinks
        // with the display's content scale (unreadable at 150-200 %).
        float const display_scale = SDL_GetWindowDisplayScale(window_);
        if (std::isfinite(display_scale) && display_scale > 1.0f) {
            ui_scale_ = std::min(display_scale, 4.0f);
            ImGuiStyle& style = ImGui::GetStyle();
            style.ScaleAllSizes(ui_scale_);
            style.FontScaleDpi = ui_scale_;
        }
    } catch (std::exception const& exception) {
        error = std::string{"Diligent ImGui initialization failed: "} + exception.what();
        return false;
    }
    return true;
#endif
}

DiligentSdlApp::~DiligentSdlApp() {
    imgui_.reset();
    if (context_ != nullptr) {
        context_->SetRenderTargets(0, nullptr, nullptr,
                                   Diligent::RESOURCE_STATE_TRANSITION_MODE_NONE);
        context_->WaitForIdle();
    }
    swap_chain_.Release();
    context_.Release();
    device_.Release();
    if (factory_ != nullptr) {
        factory_->SetMessageCallback(nullptr);
        factory_ = nullptr;
    }
    if (window_ != nullptr) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    if (owns_sdl_) {
        SDL_Quit();
        owns_sdl_ = false;
    }
    if (active_instance_ == this) {
        active_instance_ = nullptr;
    }
}

void DiligentSdlApp::messageCallback(Diligent::DEBUG_MESSAGE_SEVERITY severity,
                                     Diligent::Char const* message,
                                     Diligent::Char const* function, Diligent::Char const* file,
                                     Diligent::Int32 line) {
    if (active_instance_ != nullptr) {
        active_instance_->recordDiagnostic(severity, message);
    }
    char const* label = "info";
    if (severity == Diligent::DEBUG_MESSAGE_SEVERITY_WARNING) {
        label = "warning";
    } else if (severity == Diligent::DEBUG_MESSAGE_SEVERITY_ERROR ||
               severity == Diligent::DEBUG_MESSAGE_SEVERITY_FATAL_ERROR) {
        label = "error";
    }
    std::fprintf(stderr, "Diligent %s: %s\n", label, message != nullptr ? message : "(no message)");
    (void)function;
    (void)file;
    (void)line;
}

void DiligentSdlApp::recordDiagnostic(Diligent::DEBUG_MESSAGE_SEVERITY severity,
                                      char const* message) {
    std::scoped_lock lock{diagnostics_mutex_};
    if (severity == Diligent::DEBUG_MESSAGE_SEVERITY_WARNING) {
        ++warning_count_;
    } else if (severity == Diligent::DEBUG_MESSAGE_SEVERITY_ERROR ||
               severity == Diligent::DEBUG_MESSAGE_SEVERITY_FATAL_ERROR) {
        ++error_count_;
    }
    diagnostics_.push_back({severity, message != nullptr ? message : "(no message)"});
    constexpr std::size_t kMessageLimit = 128;
    if (diagnostics_.size() > kMessageLimit) {
        diagnostics_.erase(diagnostics_.begin(),
                           diagnostics_.begin() + (diagnostics_.size() - kMessageLimit));
    }
}

void DiligentSdlApp::forwardEvent(SDL_Event const& event) {
    if (imgui_ != nullptr) {
        imgui_->HandleSDLEvent(&event);
    }
}

bool DiligentSdlApp::wantsMouseCapture() const noexcept {
    return ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse;
}

bool DiligentSdlApp::wantsKeyboardCapture() const noexcept {
    return ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureKeyboard;
}

bool DiligentSdlApp::synchronizeDrawableSize() {
    int pixel_width = 0;
    int pixel_height = 0;
    if (!SDL_GetWindowSizeInPixels(window_, &pixel_width, &pixel_height) || pixel_width <= 0 ||
        pixel_height <= 0) {
        return false;
    }
    auto const& current = swap_chain_->GetDesc();
    if (current.Width != static_cast<Diligent::Uint32>(pixel_width) ||
        current.Height != static_cast<Diligent::Uint32>(pixel_height)) {
        context_->SetRenderTargets(0, nullptr, nullptr,
                                   Diligent::RESOURCE_STATE_TRANSITION_MODE_NONE);
        swap_chain_->Resize(static_cast<Diligent::Uint32>(pixel_width),
                            static_cast<Diligent::Uint32>(pixel_height),
                            Diligent::SURFACE_TRANSFORM_OPTIMAL);
    }
    return swap_chain_->GetDesc().Width > 0 && swap_chain_->GetDesc().Height > 0;
}

void DiligentSdlApp::beginImGuiFrame() {
    auto const& desc = swap_chain_->GetDesc();
    imgui_->NewFrame(desc.Width, desc.Height, desc.PreTransform);
}

void DiligentSdlApp::bindAndClearBackBuffer(float const clear_color[4],
                                            float reversed_depth_clear) {
    Diligent::ITextureView* rtv = swap_chain_->GetCurrentBackBufferRTV();
    Diligent::ITextureView* dsv = swap_chain_->GetDepthBufferDSV();
    context_->SetRenderTargets(1, &rtv, dsv,
                               Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context_->ClearRenderTarget(rtv, clear_color,
                                Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    if (dsv != nullptr) {
        context_->ClearDepthStencil(dsv, Diligent::CLEAR_DEPTH_FLAG, reversed_depth_clear, 0,
                                    Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    }
}

void DiligentSdlApp::setSceneViewport(float render_scale) {
    auto const& desc = swap_chain_->GetDesc();
    float const scale = std::clamp(render_scale, 0.25f, 1.0f);
    float const viewport_width = std::max(1.0f, static_cast<float>(desc.Width) * scale);
    float const viewport_height = std::max(1.0f, static_cast<float>(desc.Height) * scale);
    Diligent::Viewport const viewport{(static_cast<float>(desc.Width) - viewport_width) * 0.5f,
                                      (static_cast<float>(desc.Height) - viewport_height) * 0.5f,
                                      viewport_width, viewport_height, 0.0f, 1.0f};
    context_->SetViewports(1, &viewport, desc.Width, desc.Height);
}

void DiligentSdlApp::renderImGui() {
    imgui_->Render(context_);
}

void DiligentSdlApp::present(bool v_sync) {
    swap_chain_->Present(v_sync ? 1U : 0U);
}

std::uint32_t DiligentSdlApp::width() const noexcept {
    return swap_chain_ != nullptr ? swap_chain_->GetDesc().Width : 0U;
}

std::uint32_t DiligentSdlApp::height() const noexcept {
    return swap_chain_ != nullptr ? swap_chain_->GetDesc().Height : 0U;
}

std::string DiligentSdlApp::adapterName() const {
    return device_ != nullptr ? std::string{device_->GetAdapterInfo().Description} : std::string{};
}

bool DiligentSdlApp::durationQueriesSupported() const noexcept {
    return device_ != nullptr && device_->GetDeviceInfo().Features.DurationQueries !=
                                     Diligent::DEVICE_FEATURE_STATE_DISABLED;
}

std::uint32_t DiligentSdlApp::warningCount() const noexcept {
    std::scoped_lock lock{diagnostics_mutex_};
    return warning_count_;
}

std::uint32_t DiligentSdlApp::errorCount() const noexcept {
    std::scoped_lock lock{diagnostics_mutex_};
    return error_count_;
}

std::vector<DiligentDiagnostic> DiligentSdlApp::diagnosticsSnapshot() const {
    std::scoped_lock lock{diagnostics_mutex_};
    return diagnostics_;
}

} // namespace helios::examples
