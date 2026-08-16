// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Windowed explorer: SDL3 window + input, Vulkan smoke scene, ImGui debug
// UI, Tracy frame marks, and an optional bounded benchmark run.
#include "helios/app/headless.hpp"

#include "helios/assets/report.hpp"
#include "helios/core/log.hpp"
#include "helios/core/profiler.hpp"
#include "helios/math/mat4.hpp"
#include "helios/render/renderer.hpp"
#include "helios/render/reports.hpp"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <string>

#include <SDL3/SDL.h>

#ifdef HELIOS_ENABLE_IMGUI
#include <imgui.h>
#endif

namespace fs = std::filesystem;

namespace helios::app {

namespace {

struct OrbitCamera {
    float yaw = 0.6f;
    float pitch = 0.35f;
    float distance = 2.2f;
    math::Mat4f view() const {
        const float cp = std::cos(pitch), sp = std::sin(pitch);
        const float cy = std::cos(yaw), sy = std::sin(yaw);
        const math::Vec3f eye{distance * cp * sy, distance * sp, distance * cp * cy};
        return math::lookAt(eye, math::Vec3f{0, 0, 0}, math::Vec3f{0, 1, 0});
    }
};

} // namespace

int runWindowed(CliOptions const& options) {
    core::logInfo("=== windowed smoke ===");

    // SDL3: SDL_Init returns true on success.
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        core::logError("SDL_Init failed: {}", SDL_GetError());
        return 2;
    }

    SDL_Window* window = SDL_CreateWindow("Helios M0 — capability spike", options.width, options.height,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window == nullptr) {
        core::logError("SDL_CreateWindow failed: {}", SDL_GetError());
        SDL_Quit();
        return 2;
    }

    render::RendererCreateInfo renderer_info;
    renderer_info.window = window;
    renderer_info.width = options.width;
    renderer_info.height = options.height;
    renderer_info.enable_validation = options.validation;
    renderer_info.enable_imgui = true;
    renderer_info.max_frames = options.frames;

    auto renderer_result = render::Renderer::create(renderer_info);
    if (renderer_result.failed()) {
        core::logError("renderer creation failed: {}", renderer_result.error().to_string());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 2;
    }
    auto renderer = std::move(renderer_result.value());

    OrbitCamera camera;
    bool running = true;
    std::uint64_t frame_count = 0;
    bool show_help = true;
    bool fullscreen = false;
    double last_cpu_ms = 0.0, last_gpu_ms = 0.0;

    while (running) {
        HELIOS_ZONE_SCOPED_NAMED("app-loop");
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN) {
                if (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_Q) {
                    running = false;
                } else if (event.key.key == SDLK_F11) {
                    fullscreen = !fullscreen;
                    SDL_SetWindowFullscreen(window, fullscreen);
                }
            } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
                camera.distance *= std::pow(0.9f, event.wheel.y);
                camera.distance = std::clamp(camera.distance, 0.5f, 20.0f);
            }
        }

        // Orbit controls (drag + wheel).
        {
            float xrel = 0.0f, yrel = 0.0f;
            bool dragging = (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK) != 0;
            if (dragging) {
                SDL_GetRelativeMouseState(&xrel, &yrel);
                camera.yaw += xrel * 0.008f;
                camera.pitch = std::clamp(camera.pitch + yrel * 0.008f, -1.5f, 1.5f);
            }
        }

        render::Renderer::DebugUiFrame ui;
        ui.cpu_frame_ms = last_cpu_ms;
        ui.gpu_frame_ms = last_gpu_ms;
        ui.frame_index = frame_count;
        ui.device_name = renderer->deviceName();
        ui.validation_errors = renderer->validationErrorCount();
        ui.validation_warnings = renderer->validationWarningCount();
        ui.depth_choice = "D32_SFLOAT";
        if (options.frames > 0) {
            ui.note = std::format("benchmark run: frame {}/{}", frame_count + 1, options.frames);
        }
        renderer->setDebugUiFrame(ui);

        const bool keep_going = renderer->renderFrame();
        if (!keep_going) {
            running = false;
        }
        last_cpu_ms = renderer->stats().cpu_frame_ms.max();
        last_gpu_ms = renderer->stats().gpu_frame_ms.count() > 0 ? renderer->stats().gpu_frame_ms.max() : 0.0;
        ++frame_count;
        HELIOS_FRAME_MARK;
    }

    // Final evidence: performance summary.
    core::logInfo("rendered {} frames; closing", frame_count);
    const auto& stats = renderer->stats();
    core::logInfo("CPU frame time: p50 {:.3f} ms, p95 {:.3f} ms, p99 {:.3f} ms, max {:.3f} ms",
                  stats.cpu_frame_ms.percentile(50.0), stats.cpu_frame_ms.percentile(95.0),
                  stats.cpu_frame_ms.percentile(99.0), stats.cpu_frame_ms.max());
    if (stats.gpu_frame_ms.count() > 0) {
        core::logInfo("GPU frame time: p50 {:.3f} ms, p95 {:.3f} ms, p99 {:.3f} ms, max {:.3f} ms",
                      stats.gpu_frame_ms.percentile(50.0), stats.gpu_frame_ms.percentile(95.0),
                      stats.gpu_frame_ms.percentile(99.0), stats.gpu_frame_ms.max());
    }

    if (options.frames > 0) {
        assets::EnvironmentInfo env = render::collectEnvironment(
            std::format("{}x{}", options.width, options.height), options.validation);
        env.gpu_name = renderer->deviceName();
        assets::PerformanceReport performance;
        performance.environment = env;
        performance.cpu_frame_ms = {stats.cpu_frame_ms.count(),
                                    stats.cpu_frame_ms.percentile(50.0) * 1000.0,
                                    stats.cpu_frame_ms.percentile(95.0) * 1000.0,
                                    stats.cpu_frame_ms.percentile(99.0) * 1000.0,
                                    stats.cpu_frame_ms.max() * 1000.0,
                                    stats.cpu_frame_ms.min() * 1000.0,
                                    stats.cpu_frame_ms.average() * 1000.0};
        performance.gpu_frame_ms = {stats.gpu_frame_ms.count(),
                                    stats.gpu_frame_ms.percentile(50.0) * 1000.0,
                                    stats.gpu_frame_ms.percentile(95.0) * 1000.0,
                                    stats.gpu_frame_ms.percentile(99.0) * 1000.0,
                                    stats.gpu_frame_ms.max() * 1000.0,
                                    stats.gpu_frame_ms.min() * 1000.0,
                                    stats.gpu_frame_ms.average() * 1000.0};
        assets::EvidenceReport evidence;
        evidence.environment = env;
        evidence.validation_error_count = renderer->validationErrorCount();
        evidence.validation_warning_count = renderer->validationWarningCount();
        auto write_result = render::writeReports(options.report_dir, env, renderer->deviceReportJson(),
                                                 performance, evidence);
        if (write_result.failed()) {
            core::logError("report writing failed: {}", write_result.error().to_string());
        }
        core::logInfo("validation: {} errors, {} warnings", evidence.validation_error_count,
                      evidence.validation_warning_count);
    }

    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

} // namespace helios::app
