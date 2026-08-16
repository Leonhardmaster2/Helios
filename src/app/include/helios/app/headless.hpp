// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#pragma once

#include "helios/app/cli.hpp"

namespace helios::app {

// Runs the swapchain-free smoke suite and writes evidence reports.
// Returns process exit code (0 = all smoke paths passed).
int runHeadlessSmoke(CliOptions const& options);

// Runs the windowed explorer loop. Returns process exit code.
int runWindowed(CliOptions const& options);

} // namespace helios::app
