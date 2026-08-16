// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// The debug-utils messenger callback writes directly into
// Instance::validation_stats_ (owned by the Instance). Kept as a separate
// header so the callback declaration stays close to the stats type.
#pragma once

#include "helios/vk/instance.hpp"
