// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Windowless Diligent/Vulkan device for offscreen lab runs: captures,
// benchmarks and reference comparisons.  No SDL video subsystem, window or
// swap chain is created, so nothing appears on or takes focus from the
// desktop.  Like DiligentSdlApp it owns exactly one device/context pair.
#pragma once

#include "DeviceContext.h"
#include "EngineFactoryVk.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "Texture.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace helios::examples {

class DiligentHeadless {
  public:
    ~DiligentHeadless();
    DiligentHeadless(DiligentHeadless const&) = delete;
    DiligentHeadless& operator=(DiligentHeadless const&) = delete;

    [[nodiscard]] static std::unique_ptr<DiligentHeadless> create(bool enable_validation,
                                                                  std::string& error);

    [[nodiscard]] Diligent::IRenderDevice* device() const noexcept { return device_; }
    [[nodiscard]] Diligent::IDeviceContext* context() const noexcept { return context_; }
    [[nodiscard]] std::string adapterName() const;

    [[nodiscard]] std::uint32_t warningCount() const noexcept;
    [[nodiscard]] std::uint32_t errorCount() const noexcept;
    [[nodiscard]] std::vector<std::string> warningAndErrorMessages() const;

  private:
    DiligentHeadless() = default;

    static void DILIGENT_CALL_TYPE messageCallback(Diligent::DEBUG_MESSAGE_SEVERITY severity,
                                                    Diligent::Char const* message,
                                                    Diligent::Char const* function,
                                                    Diligent::Char const* file,
                                                    Diligent::Int32 line);

    inline static DiligentHeadless* active_instance_ = nullptr;

    Diligent::IEngineFactoryVk* factory_ = nullptr; // Diligent process singleton
    Diligent::RefCntAutoPtr<Diligent::IRenderDevice> device_;
    Diligent::RefCntAutoPtr<Diligent::IDeviceContext> context_;

    mutable std::mutex diagnostics_mutex_;
    std::vector<std::string> messages_;
    std::uint32_t warning_count_ = 0;
    std::uint32_t error_count_ = 0;
};

// Creates a 2-D colour target usable as a render target and as a copy source.
[[nodiscard]] Diligent::RefCntAutoPtr<Diligent::ITexture>
createColorTarget(Diligent::IRenderDevice* device, char const* name, std::uint32_t width,
                  std::uint32_t height, Diligent::TEXTURE_FORMAT format, bool shader_resource);

// Copies mip 0 of `source` to a staging texture and returns its rows tightly
// packed (top-left origin).  `bytes_per_pixel` must match the texture format.
// Blocks until the GPU is idle; intended for captures, not per-frame use.
[[nodiscard]] bool readbackTexture(Diligent::IRenderDevice* device,
                                   Diligent::IDeviceContext* context, Diligent::ITexture* source,
                                   std::size_t bytes_per_pixel, std::vector<std::uint8_t>& bytes,
                                   std::string& error);

} // namespace helios::examples
