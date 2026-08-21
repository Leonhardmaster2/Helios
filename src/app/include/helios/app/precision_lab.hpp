// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// M1 precision laboratory (docs/VALIDATION.md "Coordinate and precision
// suite" + "Depth suite"): a deterministic CPU route that measures binary64
// behaviour, frame-transform fidelity, camera reparenting stability, and
// screen-space jitter across the v1 spatial range, and records everything as
// a machine-readable evidence bundle. No number in the report is invented:
// every value is computed by this run.
#pragma once

#include "helios/app/cli.hpp"

namespace helios::app {

// Runs the precision lab and writes evidence under options.report_dir.
// Returns 0 when all thresholds pass, 3 on threshold failures, 2 on errors.
int runPrecisionLab(CliOptions const& options);

} // namespace helios::app
