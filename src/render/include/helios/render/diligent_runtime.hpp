// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Vulkan-only Diligent runtime used by the v0.0.1 labs.  The public header
// deliberately hides Diligent types so planet/physics code stays independent
// of the graphics implementation.
#pragma once

#include "helios/core/error.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace helios::render {

struct DiligentRuntimeCreateInfo {
    bool enable_validation = true;
};

struct DiligentRuntimeInfo {
    std::string backend;
    std::string adapter_name;
    std::string diligent_commit;
    std::string diligent_core_commit;
    std::uint32_t vulkan_major = 0;
    std::uint32_t vulkan_minor = 0;
    std::uint64_t local_memory_bytes = 0;
    std::uint64_t host_visible_memory_bytes = 0;
    std::uint32_t warning_count = 0;
    std::uint32_t error_count = 0;
};

struct DiligentClearResult {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba8;
};

class DiligentRuntime {
  public:
    ~DiligentRuntime();
    DiligentRuntime(DiligentRuntime const&) = delete;
    DiligentRuntime& operator=(DiligentRuntime const&) = delete;

    [[nodiscard]] static core::Result<std::unique_ptr<DiligentRuntime>>
    create(DiligentRuntimeCreateInfo const& info = {});

    // Clears an offscreen RGBA8 render target through Diligent/Vulkan and
    // returns the GPU-readback bytes.  This is the first migration parity
    // gate and works without creating or touching a desktop window.
    [[nodiscard]] core::Result<DiligentClearResult>
    clearAndReadback(std::uint32_t width, std::uint32_t height, std::array<float, 4> color);

    [[nodiscard]] DiligentRuntimeInfo info() const;

  private:
    DiligentRuntime();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace helios::render
