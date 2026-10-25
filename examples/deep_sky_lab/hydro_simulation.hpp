// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Gas dynamics on the GPU for the deep-sky lab: the host side of the compute
// passes in shaders/hydro_*.slang (see shaders/lib/hydro.slang for the
// scheme).  It owns the state volumes, chooses time steps from the Courant
// condition, and produces the emission volume that the display pass marches
// through.  Scenarios (what the gas is, and what else acts on it) are
// described by HydroSetup; gas_scene.hpp builds the nebula and the remnant.
#pragma once

#include "DeviceContext.h"
#include "Fence.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"

#include "shaders/abi.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace helios::examples {

enum class HydroScenario : std::uint8_t {
    Nebula = 0,
    Remnant = 1,
    ShockTube = 2,
    Blast = 3,
};

struct HydroSetup {
    HydroScenario scenario = HydroScenario::ShockTube;
    std::uint32_t resolution = 128; // cells per edge, a multiple of 16
    std::uint32_t seed = 1;
    double half_extent = 0.5;
    // Stretching of the grid (lib/hydro.slang): 0 is uniform; 2 makes the
    // edge cells 3.8 times the central ones.
    double stretch = 0.0;
    double gamma = 5.0 / 3.0;
    double density_floor = 1.0e-6;
    double pressure_floor = 1.0e-9; // of pressure / density
    double start_time = 0.0;
    // Fraction of the Courant limit used for a step.
    double courant = 0.35;
    // HydroParams::scenario.zw, source and shade rows (shaders/lib/hydro.slang).
    float scenario_z = 0.0f;
    float scenario_w = 0.0f;
    std::array<std::array<float, 4>, 5> source{};
    std::array<std::array<float, 4>, 6> shade{};
};

struct HydroTotals {
    double mass = 0.0;
    double energy = 0.0;
    double scalar = 0.0;
    double fastest_signal = 0.0;
};

class HydroSimulation {
  public:
    [[nodiscard]] bool initialize(Diligent::IRenderDevice* device,
                                  Diligent::IDeviceContext* context, std::string& error);

    // Allocates the volumes for `setup` and writes the initial state.
    [[nodiscard]] bool start(HydroSetup const& setup, std::uint32_t threads, std::string& error);

    [[nodiscard]] bool started() const noexcept { return resolution_ != 0; }

    // Sources and emission parameters may change while a run is in progress
    // (a star switches on, a pulsar spins down).
    [[nodiscard]] HydroSetup& setup() noexcept { return setup_; }
    [[nodiscard]] HydroSetup const& setup() const noexcept { return setup_; }

    // Takes up to `steps` time steps, stopping at `target_time`.  Signal
    // speeds below `signal_floor` are ignored when choosing the step.
    //
    // The Courant condition needs the fastest signal on the grid, which has
    // to be read back from the GPU.  With `synchronous` the host waits for it
    // (exact, for captures and validation).  Without, it never waits: steps
    // use the most recent measurement that has arrived, a frame or two old,
    // with a margin, so a frame's time does not depend on the simulation.
    [[nodiscard]] bool advance(std::uint32_t steps, double target_time, double signal_floor,
                               bool synchronous, std::string& error);

    // Integrates the star's radiation field through the current state
    // (nebula only).
    void traceRays();

    // Writes the emission volume from the current state.
    void shade();

    [[nodiscard]] double time() const noexcept { return time_; }
    [[nodiscard]] std::uint64_t steps() const noexcept { return steps_; }
    [[nodiscard]] double timeStep() const noexcept { return dt_; }
    [[nodiscard]] std::uint32_t resolution() const noexcept { return resolution_; }
    [[nodiscard]] HydroTotals const& totals() const noexcept { return totals_; }
    [[nodiscard]] Diligent::ITexture* emission() const noexcept { return emission_; }

    // Measures mass, energy and the fastest signal now (also done as part of
    // advance()).
    [[nodiscard]] bool measure(std::string& error);

    // Copies the conserved state to the host: four floats per cell for each
    // of the two state textures, x fastest.  For validation.
    [[nodiscard]] bool readState(std::vector<float>& a, std::vector<float>& b, std::string& error);

  private:
    struct Pass {
        Diligent::RefCntAutoPtr<Diligent::IPipelineState> pipeline;
        Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> resources;
    };
    using Params = shaders::HydroParams;

    [[nodiscard]] bool createPass(Pass& pass, char const* shader, char const* name, std::string& error);
    void bind(Pass& pass, char const* variable, Diligent::IDeviceObject* object);
    void dispatch(Pass& pass, std::uint32_t x, std::uint32_t y, std::uint32_t z);
    void writeParams(double dt, int stage);
    [[nodiscard]] bool readVolume(Diligent::ITexture* texture, std::vector<float>& out,
                                  std::string& error);
    void dispatchSummary(Diligent::ITexture* staging);
    [[nodiscard]] bool readSummary(Diligent::ITexture* staging, std::string& error);
    void requestMeasurement();
    [[nodiscard]] bool pollMeasurement(std::string& error);
    [[nodiscard]] Diligent::ITextureView* stateView(std::size_t set, std::size_t texture) const;
    [[nodiscard]] Diligent::ITextureView* stateTarget(std::size_t set, std::size_t texture) const;

    Diligent::RefCntAutoPtr<Diligent::IRenderDevice> device_;
    Diligent::RefCntAutoPtr<Diligent::IDeviceContext> context_;
    Pass init_{};
    Pass advance_{};
    Pass speed_{};
    Pass rays_{};
    Pass shade_{};
    Pass smooth_{};
    Diligent::RefCntAutoPtr<Diligent::IBuffer> params_;
    Diligent::RefCntAutoPtr<Diligent::ISampler> clamp_sampler_;
    Diligent::RefCntAutoPtr<Diligent::ISampler> wrap_sampler_;
    Diligent::RefCntAutoPtr<Diligent::ISampler> ray_sampler_;

    // Three copies of the state (a, b): the current one and two that the
    // Runge-Kutta stages write.
    std::array<std::array<Diligent::RefCntAutoPtr<Diligent::ITexture>, 2>, 3> state_{};
    Diligent::RefCntAutoPtr<Diligent::ITexture> seed_;
    Diligent::RefCntAutoPtr<Diligent::ITexture> summary_;
    Diligent::RefCntAutoPtr<Diligent::ITexture> summary_staging_;
    // Readback that is collected when the GPU has got to it.
    Diligent::RefCntAutoPtr<Diligent::ITexture> summary_pending_;
    Diligent::RefCntAutoPtr<Diligent::IFence> summary_fence_;
    std::uint64_t fence_value_ = 0;
    std::uint64_t pending_value_ = 0; // 0: no measurement under way
    Diligent::RefCntAutoPtr<Diligent::ITexture> ray_field_;
    Diligent::RefCntAutoPtr<Diligent::ITexture> emission_raw_; // one value per cell
    Diligent::RefCntAutoPtr<Diligent::ITexture> emission_;     // filtered, for display

    HydroSetup setup_{};
    std::uint32_t resolution_ = 0;
    std::size_t current_ = 0; // index of the current state copy
    double time_ = 0.0;
    double dt_ = 0.0;
    std::uint64_t steps_ = 0;
    HydroTotals totals_{};
};

} // namespace helios::examples
