// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#pragma once

#include "helios/core/config.hpp"

#include <string>
#include <vector>

namespace helios::app {

struct CliOptions {
    bool show_help = false;
    bool headless_smoke = false;   // --headless-smoke (no window/swapchain)
    bool windowed_smoke = false;   // --smoke (windowed capability scene)
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    std::uint32_t frames = 0;          // 0 = unlimited (windowed)
    std::string report_dir = "evidence/m0/local";
    bool validation = true;            // --enable-validation / --disable-validation
    std::uint32_t iterations = 3;      // headless smoke iterations per path
    bool smoke_clear = true;
    bool smoke_triangle = true;
    bool smoke_depth = true;
    bool smoke_compute = true;
    bool smoke_transfer = true;
    bool smoke_timestamp = true;
    std::string config_file;           // optional JSON config
};

// Parses argv into CliOptions; --help prints usage and sets show_help.
[[nodiscard]] CliOptions parseCli(int argc, char** argv);

void printUsage(char const* program);

} // namespace helios::app
