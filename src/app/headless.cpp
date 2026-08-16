// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// --headless-smoke entry: delegates to helios_render::runHeadlessSmoke
// (the app has no helios_vk link edge — docs/ARCHITECTURE.md).
#include "helios/app/headless.hpp"

#include "helios/core/log.hpp"
#include "helios/render/headless.hpp"

namespace helios::app {

int runHeadlessSmoke(CliOptions const& options) {
    render::HeadlessSmokeOptions smoke_options;
    smoke_options.width = options.width;
    smoke_options.height = options.height;
    smoke_options.iterations = options.iterations;
    smoke_options.enable_validation = options.validation;
    smoke_options.clear = options.smoke_clear;
    smoke_options.triangle = options.smoke_triangle;
    smoke_options.depth = options.smoke_depth;
    smoke_options.compute = options.smoke_compute;
    smoke_options.transfer = options.smoke_transfer;
    smoke_options.timestamp = options.smoke_timestamp;
    smoke_options.report_dir = options.report_dir;

    auto outcome = render::runHeadlessSmoke(smoke_options);
    if (outcome.failed()) {
        core::logError("headless smoke failed: {}", outcome.error().to_string());
        return 2;
    }
    return outcome.value().all_pass ? 0 : 1;
}

} // namespace helios::app
