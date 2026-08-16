// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/app/cli.hpp"

#include <cstdio>

namespace helios::app {

namespace {
bool parseUInt(std::string const& value, std::uint32_t& out) {
    try {
        std::size_t pos = 0;
        const unsigned long v = std::stoul(value, &pos);
        if (pos != value.size()) return false;
        out = static_cast<std::uint32_t>(v);
        return true;
    } catch (...) {
        return false;
    }
}
} // namespace

CliOptions parseCli(int argc, char** argv) {
    CliOptions options;
    std::vector<std::string> args(argv + 1, argv + argc);
    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string const& arg = args[i];
        auto value = [&](std::string const& name, std::string& out) -> bool {
            if (arg == name && i + 1 < args.size()) {
                out = args[++i];
                return true;
            }
            if (arg.starts_with(name + "=")) {
                out = arg.substr(name.size() + 1);
                return true;
            }
            return false;
        };
        if (arg == "--help" || arg == "-h") {
            options.show_help = true;
        } else if (arg == "--headless-smoke") {
            options.headless_smoke = true;
        } else if (arg == "--smoke") {
            options.windowed_smoke = true;
        } else if (arg == "--disable-validation") {
            options.validation = false;
        } else if (arg == "--enable-validation") {
            options.validation = true;
        } else if (arg == "--no-clear") {
            options.smoke_clear = false;
        } else if (arg == "--no-triangle") {
            options.smoke_triangle = false;
        } else if (arg == "--no-depth") {
            options.smoke_depth = false;
        } else if (arg == "--no-compute") {
            options.smoke_compute = false;
        } else if (arg == "--no-transfer") {
            options.smoke_transfer = false;
        } else if (arg == "--no-timestamp") {
            options.smoke_timestamp = false;
        } else {
            std::string v;
            if (value("--width", v)) {
                if (!parseUInt(v, options.width)) std::fprintf(stderr, "bad --width: %s\n", v.c_str());
            } else if (value("--height", v)) {
                if (!parseUInt(v, options.height)) std::fprintf(stderr, "bad --height: %s\n", v.c_str());
            } else if (value("--frames", v)) {
                if (!parseUInt(v, options.frames)) std::fprintf(stderr, "bad --frames: %s\n", v.c_str());
            } else if (value("--iterations", v)) {
                if (!parseUInt(v, options.iterations)) std::fprintf(stderr, "bad --iterations: %s\n", v.c_str());
            } else if (value("--report-dir", v)) {
                options.report_dir = v;
            } else if (value("--config", v)) {
                options.config_file = v;
            } else {
                std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
            }
        }
    }
    return options;
}

void printUsage(char const* program) {
    std::printf(R"(Helios — Milestone 0 capability spike
Usage: %s [options]

Modes:
  (default)                 Windowed explorer: SDL3 window, Vulkan smoke scene,
                            ImGui debug UI, validation layers.
  --headless-smoke          Swapchain-free Vulkan smoke suite + reports, no window.
  --smoke                   Windowed smoke scene (same as default).

Options:
  --width N --height N      Window/resolution size (default 1280x720).
  --frames N                Render N frames then exit (0 = unlimited).
  --iterations N            Headless smoke repetitions per path (default 3).
  --report-dir DIR          Evidence output directory (default evidence/m0/local).
  --enable-validation       Enable VK_LAYER_KHRONOS_validation (default on).
  --disable-validation      Disable validation layers.
  --no-clear --no-triangle --no-depth --no-compute --no-transfer --no-timestamp
                            Disable individual headless smoke paths.
  --config FILE             JSON config file (layered under CLI args).
  --help                    Show this message.
)", program);
}

} // namespace helios::app
