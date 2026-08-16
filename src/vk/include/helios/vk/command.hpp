// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// One-shot command buffer helper (single-threaded recording; submit + wait).
#pragma once

#include "helios/core/error.hpp"
#include "helios/vk/device.hpp"

#include <functional>

#include <vulkan/vulkan.h>

namespace helios::vk {

// Allocates a command buffer from a transient pool, runs record() between
// begin/end, submits on the graphics queue and waits for completion.
// Used for one-shot initialization and readback operations.
[[nodiscard]] core::Result<void> runOneShot(Device& device,
                                            std::function<void(VkCommandBuffer)> const& record);

} // namespace helios::vk
