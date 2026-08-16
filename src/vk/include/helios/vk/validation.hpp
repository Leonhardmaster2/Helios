// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Debug-utils messenger state shared with Instance.
#pragma once

#include "helios/vk/instance.hpp"

namespace helios::vk {

// Internal: callback + state used by the instance debug messenger.
struct DebugMessengerState {
    ValidationStats stats;
};

} // namespace helios::vk
