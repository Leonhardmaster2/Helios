// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Helios M0 entry point: headless smoke suite or windowed explorer.
#include "helios/app/cli.hpp"
#include "helios/app/headless.hpp"
#include "helios/core/log.hpp"
#include "helios/version.hpp"

#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    const helios::app::CliOptions options = helios::app::parseCli(argc, argv);
    if (options.show_help) {
        helios::app::printUsage(argv[0]);
        return 0;
    }

    helios::core::Log::instance().setLevel(helios::core::LogLevel::Info);
    helios::core::logInfo("Helios {} starting", helios::heliosVersionString());

    if (options.headless_smoke) {
        return helios::app::runHeadlessSmoke(options);
    }
    return helios::app::runWindowed(options);
}
