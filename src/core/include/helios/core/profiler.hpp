// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Profiling facade. With HELIOS_ENABLE_TRACY these map to Tracy zones and
// frame marks; otherwise they compile to nothing. Do not use Tracy directly
// outside this header.
#pragma once

#ifdef HELIOS_ENABLE_TRACY
#include <tracy/Tracy.hpp>
#define HELIOS_ZONE_SCOPED ZoneScoped
#define HELIOS_ZONE_SCOPED_NAMED(name) ZoneScopedN(name)
#define HELIOS_FRAME_MARK FrameMark
#define HELIOS_PLOT(name, value) TracyPlot(name, value)
#else
#define HELIOS_ZONE_SCOPED
#define HELIOS_ZONE_SCOPED_NAMED(name)
#define HELIOS_FRAME_MARK
#define HELIOS_PLOT(name, value)
#endif
