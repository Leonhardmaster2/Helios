// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Headless first parity gate for the independently pinned Diligent fork.
#include "helios/core/log.hpp"
#include "helios/render/diligent_runtime.hpp"

#include <array>
#include <cmath>
#include <cstdint>

namespace {

std::uint8_t unorm8(float value) {
    return static_cast<std::uint8_t>(std::lround(value * 255.0f));
}

} // namespace

int main() {
    helios::core::logInfo("Helios Diligent/Vulkan headless probe starting");
    auto runtime = helios::render::DiligentRuntime::create({.enable_validation = true});
    if (runtime.failed()) {
        helios::core::logError("runtime creation failed: {}", runtime.error().to_string());
        return 2;
    }

    constexpr std::array<float, 4> kColor{0.125f, 0.375f, 0.75f, 1.0f};
    auto clear = runtime.value()->clearAndReadback(64, 64, kColor);
    if (clear.failed()) {
        helios::core::logError("offscreen clear failed: {}", clear.error().to_string());
        return 3;
    }

    auto const& pixels = clear.value().rgba8;
    for (std::size_t offset = 0; offset < pixels.size(); offset += 4) {
        for (std::size_t channel = 0; channel < 4; ++channel) {
            const int difference = std::abs(static_cast<int>(pixels[offset + channel]) -
                                            static_cast<int>(unorm8(kColor[channel])));
            if (difference > 1) {
                helios::core::logError("readback mismatch at byte {}: got {}, expected {}", offset + channel,
                                       pixels[offset + channel], unorm8(kColor[channel]));
                return 4;
            }
        }
    }

    const auto info = runtime.value()->info();
    helios::core::logInfo("backend: {}; adapter: {}; Vulkan {}.{}", info.backend, info.adapter_name,
                          info.vulkan_major, info.vulkan_minor);
    helios::core::logInfo("Diligent commits: engine {}, core {}", info.diligent_commit,
                          info.diligent_core_commit);
    helios::core::logInfo("offscreen readback: {}x{}, {} bytes; validation: {} errors, {} warnings",
                          clear.value().width, clear.value().height, pixels.size(), info.error_count,
                          info.warning_count);
    return info.error_count == 0 ? 0 : 5;
}
