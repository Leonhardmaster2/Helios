// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "hydro_simulation.hpp"

#include "fft.hpp"
#include "fullscreen_pass.hpp"
#include "worker_pool.hpp"

#include "MapHelper.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <numbers>

namespace helios::examples {
namespace {

constexpr std::uint32_t kSeedSize = 64;
// Spherical grid of the nebula's radiation field, per cell of the gas grid's
// edge: directions along each side of the octahedral map, and radii.
// Neighbouring rays are then about one cell apart at the far side of the box.
constexpr double kRayDirectionsPerCell = 2.0;
constexpr double kRayRadiiPerCell = 1.25;
// Time steps between measurements of the fastest signal, and between
// retracings of the nebula's radiation field (the front moves a small fraction
// of a cell per step).
constexpr std::uint64_t kMeasureInterval = 2;
constexpr std::uint64_t kRayInterval = 2;
// Margin on a signal speed that was measured a frame or two ago.
constexpr double kStaleMargin = 1.25;

[[nodiscard]] std::uint64_t mix(std::uint64_t z) {
    z += 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

[[nodiscard]] double uniform(std::uint64_t seed, std::uint64_t index, std::uint64_t stream) {
    std::uint64_t const h = mix(mix(seed * 0xD1B54A32D192ED03ULL + stream) ^ (index * 0x8CB92BA72F3D8DD7ULL));
    return (static_cast<double>(h >> 11) + 0.5) * (1.0 / 9007199254740992.0);
}

// A periodic Gaussian random field of kSeedSize^3 values with power spectrum
// k^slope between the wavenumbers `low` and `high` (in periods per box),
// scaled to unit rms.
[[nodiscard]] std::vector<float> randomField(std::uint32_t seed, std::uint64_t stream, double slope,
                                             double low, double high, Fft const& fft,
                                             WorkerPool& pool) {
    std::size_t const n = kSeedSize;
    std::vector<std::complex<float>> modes(n * n * n);
    auto wavenumber = [n](std::size_t index) {
        return static_cast<double>(index <= n / 2 ? index : n - index);
    };
    for (std::size_t z = 0; z < n; ++z) {
        for (std::size_t y = 0; y < n; ++y) {
            for (std::size_t x = 0; x < n; ++x) {
                double const k = std::sqrt(wavenumber(x) * wavenumber(x) + wavenumber(y) * wavenumber(y) +
                                           wavenumber(z) * wavenumber(z));
                if (k < low || k > high) {
                    continue;
                }
                std::size_t const index = (z * n + y) * n + x;
                // Amplitude sqrt(P(k)), Gaussian by Box-Muller.
                double const magnitude = std::pow(k, 0.5 * slope) *
                                         std::sqrt(-2.0 * std::log(uniform(seed, index, stream)));
                double const phase = 2.0 * std::numbers::pi * uniform(seed, index, stream + 1);
                modes[index] = {static_cast<float>(magnitude * std::cos(phase)),
                                static_cast<float>(magnitude * std::sin(phase))};
            }
        }
    }
    transform3d(fft, modes, true, pool);
    std::vector<float> field(n * n * n);
    double sum2 = 0.0;
    for (std::size_t index = 0; index < field.size(); ++index) {
        // The real part of a random complex field is a real random field with
        // the same spectrum.
        field[index] = modes[index].real();
        sum2 += static_cast<double>(field[index]) * field[index];
    }
    auto const scale = static_cast<float>(1.0 / std::sqrt(std::max(sum2 / static_cast<double>(field.size()), 1.0e-30)));
    for (float& value : field) {
        value *= scale;
    }
    return field;
}

[[nodiscard]] Diligent::RefCntAutoPtr<Diligent::ITexture>
createVolume(Diligent::IRenderDevice* device, char const* name, std::uint32_t width,
             std::uint32_t height, std::uint32_t depth, Diligent::TEXTURE_FORMAT format,
             Diligent::BIND_FLAGS bind, Diligent::ISampler* sampler, void const* data,
             std::size_t bytes_per_texel) {
    Diligent::TextureDesc desc;
    desc.Name = name;
    desc.Type = Diligent::RESOURCE_DIM_TEX_3D;
    desc.Width = width;
    desc.Height = height;
    desc.Depth = depth;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.Usage = Diligent::USAGE_DEFAULT;
    desc.BindFlags = bind;
    Diligent::TextureSubResData level;
    level.pData = data;
    level.Stride = static_cast<Diligent::Uint64>(width) * bytes_per_texel;
    level.DepthStride = level.Stride * height;
    Diligent::TextureData initial;
    initial.pSubResources = &level;
    initial.NumSubresources = 1;
    Diligent::RefCntAutoPtr<Diligent::ITexture> texture;
    device->CreateTexture(desc, data != nullptr ? &initial : nullptr, &texture);
    if (texture != nullptr && sampler != nullptr) {
        texture->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE)->SetSampler(sampler);
    }
    return texture;
}

} // namespace

bool HydroSimulation::createPass(Pass& pass, char const* shader, char const* name,
                                 std::string& error) {
    auto module = createEmbeddedShader(device_, shader, name, Diligent::SHADER_TYPE_COMPUTE, error);
    if (module == nullptr) {
        return false;
    }
    Diligent::ComputePipelineStateCreateInfo info;
    info.PSODesc.Name = name;
    info.PSODesc.PipelineType = Diligent::PIPELINE_TYPE_COMPUTE;
    // Every pass reads and writes different copies of the state from one
    // dispatch to the next.
    info.PSODesc.ResourceLayout.DefaultVariableType = Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
    info.pCS = module;
    device_->CreateComputePipelineState(info, &pass.pipeline);
    if (pass.pipeline == nullptr) {
        error = std::string{"Diligent failed to create the compute pipeline "} + name;
        return false;
    }
    pass.pipeline->CreateShaderResourceBinding(&pass.resources, true);
    if (pass.resources == nullptr) {
        error = std::string{"Diligent failed to create the resource binding of "} + name;
        return false;
    }
    bind(pass, "hydro", params_);
    return true;
}

void HydroSimulation::bind(Pass& pass, char const* variable, Diligent::IDeviceObject* object) {
    if (auto* const target =
            pass.resources->GetVariableByName(Diligent::SHADER_TYPE_COMPUTE, variable)) {
        target->Set(object);
    }
}

void HydroSimulation::dispatch(Pass& pass, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    context_->SetPipelineState(pass.pipeline);
    context_->CommitShaderResources(pass.resources,
                                    Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Diligent::DispatchComputeAttribs attribs;
    attribs.ThreadGroupCountX = x;
    attribs.ThreadGroupCountY = y;
    attribs.ThreadGroupCountZ = z;
    context_->DispatchCompute(attribs);
}

bool HydroSimulation::initialize(Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                                 std::string& error) {
    device_ = device;
    context_ = context;
    params_ = createDynamicUniformBuffer(device_, "Helios gas-dynamics constants", sizeof(Params));

    Diligent::SamplerDesc clamp;
    clamp.Name = "Helios gas-dynamics trilinear clamp sampler";
    clamp.MinFilter = clamp.MagFilter = Diligent::FILTER_TYPE_LINEAR;
    clamp.MipFilter = Diligent::FILTER_TYPE_POINT;
    clamp.AddressU = clamp.AddressV = clamp.AddressW = Diligent::TEXTURE_ADDRESS_CLAMP;
    device_->CreateSampler(clamp, &clamp_sampler_);
    Diligent::SamplerDesc wrap = clamp;
    wrap.Name = "Helios gas-dynamics trilinear wrap sampler";
    wrap.AddressU = wrap.AddressV = wrap.AddressW = Diligent::TEXTURE_ADDRESS_WRAP;
    device_->CreateSampler(wrap, &wrap_sampler_);
    Diligent::SamplerDesc ray = clamp;
    ray.Name = "Helios gas-dynamics ray-grid sampler";
    device_->CreateSampler(ray, &ray_sampler_);
    Diligent::FenceDesc fence;
    fence.Name = "Helios gas-dynamics readback fence";
    device_->CreateFence(fence, &summary_fence_);
    if (params_ == nullptr || clamp_sampler_ == nullptr || wrap_sampler_ == nullptr ||
        ray_sampler_ == nullptr || summary_fence_ == nullptr) {
        error = "Diligent failed to create the gas-dynamics buffer or samplers";
        return false;
    }
    return createPass(init_, "hydro_init.slang.compute.spv", "Helios gas initial conditions", error) &&
           createPass(advance_, "hydro_advance.slang.compute.spv", "Helios gas time step", error) &&
           createPass(speed_, "hydro_speed.slang.compute.spv", "Helios gas signal speed", error) &&
           createPass(rays_, "hydro_rays.slang.compute.spv", "Helios gas radiation field", error) &&
           createPass(shade_, "hydro_shade.slang.compute.spv", "Helios gas emission", error) &&
           createPass(smooth_, "hydro_smooth.slang.compute.spv", "Helios gas emission filter", error);
}

Diligent::ITextureView* HydroSimulation::stateView(std::size_t set, std::size_t texture) const {
    return state_[set][texture]->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
}

Diligent::ITextureView* HydroSimulation::stateTarget(std::size_t set, std::size_t texture) const {
    return state_[set][texture]->GetDefaultView(Diligent::TEXTURE_VIEW_UNORDERED_ACCESS);
}

void HydroSimulation::writeParams(double dt, int stage) {
    Diligent::MapHelper<Params> mapped{context_, params_, Diligent::MAP_WRITE,
                                       Diligent::MAP_FLAG_DISCARD};
    Params& params = *mapped;
    double const cell = 2.0 * setup_.half_extent / static_cast<double>(resolution_);
    params.grid[0] = static_cast<float>(resolution_);
    params.grid[1] = static_cast<float>(cell);
    params.grid[2] = static_cast<float>(dt);
    params.grid[3] = static_cast<float>(time_);
    params.gas[0] = static_cast<float>(setup_.gamma);
    params.gas[1] = static_cast<float>(setup_.density_floor);
    params.gas[2] = static_cast<float>(setup_.pressure_floor);
    params.gas[3] = static_cast<float>(stage);
    params.scenario[0] = static_cast<float>(setup_.scenario);
    params.scenario[1] = static_cast<float>(setup_.half_extent);
    params.scenario[2] = setup_.scenario_z;
    params.scenario[3] = setup_.scenario_w;
    std::memcpy(params.source, setup_.source.data(), sizeof(params.source));
    std::memcpy(params.shade, setup_.shade.data(), sizeof(params.shade));
    double const a = std::max(setup_.stretch, 0.0);
    params.mesh[0] = static_cast<float>(a);
    params.mesh[1] = static_cast<float>(a > 0.0 ? std::sinh(a) : 1.0);
    params.mesh[2] = static_cast<float>(a > 0.0 ? a / std::sinh(a) : 1.0);
    params.mesh[3] = 0.0f;
}

bool HydroSimulation::start(HydroSetup const& setup, std::uint32_t threads, std::string& error) {
    if (setup.resolution < 32 || setup.resolution > 512 || setup.resolution % 16 != 0) {
        error = "the gas grid must be a multiple of 16 cells between 32 and 512";
        return false;
    }
    setup_ = setup;
    std::uint32_t const n = setup.resolution;
    if (n != resolution_) {
        for (auto& set : state_) {
            for (auto& texture : set) {
                texture.Release();
            }
        }
        emission_.Release();
        emission_raw_.Release();
        summary_.Release();
        summary_staging_.Release();
        summary_pending_.Release();
        auto const bind = Diligent::BIND_SHADER_RESOURCE | Diligent::BIND_UNORDERED_ACCESS;
        for (auto& set : state_) {
            for (auto& texture : set) {
                texture = createVolume(device_, "Helios gas state", n, n, n,
                                       Diligent::TEX_FORMAT_RGBA32_FLOAT, bind, clamp_sampler_,
                                       nullptr, 16);
                if (texture == nullptr) {
                    error = "Diligent failed to create a gas state volume (out of GPU memory?)";
                    return false;
                }
            }
        }
        emission_ = createVolume(device_, "Helios gas emission volume", n, n, n,
                                 Diligent::TEX_FORMAT_RGBA16_FLOAT, bind, clamp_sampler_, nullptr, 8);
        emission_raw_ = createVolume(device_, "Helios gas emission per cell", n, n, n,
                                     Diligent::TEX_FORMAT_RGBA16_FLOAT, bind, clamp_sampler_, nullptr,
                                     8);
        std::uint32_t const blocks = n / 8U;
        Diligent::TextureDesc summary;
        summary.Name = "Helios gas block summary";
        summary.Type = Diligent::RESOURCE_DIM_TEX_2D;
        summary.Width = blocks * blocks;
        summary.Height = blocks;
        summary.MipLevels = 1;
        summary.Format = Diligent::TEX_FORMAT_RGBA32_FLOAT;
        summary.Usage = Diligent::USAGE_DEFAULT;
        summary.BindFlags = bind;
        device_->CreateTexture(summary, nullptr, &summary_);
        Diligent::TextureDesc staging = summary;
        staging.Name = "Helios gas block summary readback";
        staging.Usage = Diligent::USAGE_STAGING;
        staging.BindFlags = Diligent::BIND_NONE;
        staging.CPUAccessFlags = Diligent::CPU_ACCESS_READ;
        device_->CreateTexture(staging, nullptr, &summary_staging_);
        staging.Name = "Helios gas block summary deferred readback";
        device_->CreateTexture(staging, nullptr, &summary_pending_);
        if (emission_ == nullptr || emission_raw_ == nullptr || summary_ == nullptr ||
            summary_staging_ == nullptr || summary_pending_ == nullptr) {
            error = "Diligent failed to create the gas emission volume or block summary";
            return false;
        }
        resolution_ = n;
    }

    // The radiation field is only traced for the nebula; the other scenarios
    // get a one-texel stand-in so that every pass has all of its inputs.
    bool const nebula = setup.scenario == HydroScenario::Nebula;
    ray_field_.Release();
    auto rays = [&](double per_cell) {
        return nebula ? static_cast<std::uint32_t>(per_cell * n) / 8U * 8U : 1U;
    };
    ray_field_ = createVolume(device_, "Helios gas radiation field", rays(kRayDirectionsPerCell),
                              rays(kRayDirectionsPerCell), rays(kRayRadiiPerCell),
                              Diligent::TEX_FORMAT_RG16_FLOAT,
                              Diligent::BIND_SHADER_RESOURCE | Diligent::BIND_UNORDERED_ACCESS,
                              ray_sampler_, nullptr, 4);

    // Random fields of this run: velocity (three components) and density.
    {
        WorkerPool pool{threads};
        Fft const fft{kSeedSize};
        std::vector<float> const vx = randomField(setup.seed, 10, -4.0, 1.0, 20.0, fft, pool);
        std::vector<float> const vy = randomField(setup.seed, 20, -4.0, 1.0, 20.0, fft, pool);
        std::vector<float> const vz = randomField(setup.seed, 30, -4.0, 1.0, 20.0, fft, pool);
        std::vector<float> const density = randomField(setup.seed, 40, -1.0, 4.0, 31.0, fft, pool);
        std::vector<float> texels(vx.size() * 4U);
        for (std::size_t index = 0; index < vx.size(); ++index) {
            texels[index * 4U + 0U] = vx[index];
            texels[index * 4U + 1U] = vy[index];
            texels[index * 4U + 2U] = vz[index];
            texels[index * 4U + 3U] = density[index];
        }
        seed_.Release();
        seed_ = createVolume(device_, "Helios gas random fields", kSeedSize, kSeedSize, kSeedSize,
                             Diligent::TEX_FORMAT_RGBA32_FLOAT, Diligent::BIND_SHADER_RESOURCE,
                             wrap_sampler_, texels.data(), 16);
    }
    if (ray_field_ == nullptr || seed_ == nullptr) {
        error = "Diligent failed to create the gas radiation field or random fields";
        return false;
    }

    current_ = 0;
    time_ = setup.start_time;
    steps_ = 0;
    dt_ = 0.0;
    pending_value_ = 0; // a measurement of the previous run is of no use
    writeParams(0.0, 0);
    bind(init_, "seed_field", seed_->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE));
    bind(init_, "next_a", stateTarget(0, 0));
    bind(init_, "next_b", stateTarget(0, 1));
    std::uint32_t const groups = n / 8U;
    dispatch(init_, groups, groups, groups);
    return measure(error);
}

void HydroSimulation::dispatchSummary(Diligent::ITexture* staging) {
    std::uint32_t const blocks = resolution_ / 8U;
    writeParams(dt_, 0);
    bind(speed_, "state_a", stateView(current_, 0));
    bind(speed_, "state_b", stateView(current_, 1));
    bind(speed_, "block_summary", summary_->GetDefaultView(Diligent::TEXTURE_VIEW_UNORDERED_ACCESS));
    std::uint32_t const groups = (blocks + 3U) / 4U;
    dispatch(speed_, groups, groups, groups);

    Diligent::CopyTextureAttribs copy;
    copy.pSrcTexture = summary_;
    copy.SrcTextureTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    copy.pDstTexture = staging;
    copy.DstTextureTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    context_->CopyTexture(copy);
}

bool HydroSimulation::readSummary(Diligent::ITexture* staging, std::string& error) {
    std::uint32_t const blocks = resolution_ / 8U;
    Diligent::MappedTextureSubresource mapped;
    context_->MapTextureSubresource(staging, 0, 0, Diligent::MAP_READ, Diligent::MAP_FLAG_DO_NOT_WAIT,
                                    nullptr, mapped);
    if (mapped.pData == nullptr) {
        error = "failed to read back the gas block summary";
        return false;
    }
    HydroTotals totals;
    std::size_t const width = static_cast<std::size_t>(blocks) * blocks;
    for (std::uint32_t row = 0; row < blocks; ++row) {
        auto const* const texels = reinterpret_cast<float const*>(
            static_cast<std::uint8_t const*>(mapped.pData) + static_cast<std::size_t>(row) * mapped.Stride);
        for (std::size_t column = 0; column < width; ++column) {
            float const* const texel = texels + column * 4U;
            totals.fastest_signal = std::max(totals.fastest_signal, static_cast<double>(texel[0]));
            totals.mass += texel[1];
            totals.energy += texel[2];
            totals.scalar += texel[3];
        }
    }
    context_->UnmapTextureSubresource(staging, 0, 0);
    double const cell = 2.0 * setup_.half_extent / static_cast<double>(resolution_);
    double const volume = cell * cell * cell;
    totals.mass *= volume;
    totals.energy *= volume;
    totals.scalar *= volume;
    if (!std::isfinite(totals.fastest_signal) || !std::isfinite(totals.energy)) {
        error = "the gas simulation produced a non-finite state";
        return false;
    }
    totals_ = totals;
    return true;
}

bool HydroSimulation::measure(std::string& error) {
    dispatchSummary(summary_staging_);
    context_->Flush();
    context_->WaitForIdle();
    return readSummary(summary_staging_, error);
}

void HydroSimulation::requestMeasurement() {
    if (pending_value_ != 0) {
        return;
    }
    dispatchSummary(summary_pending_);
    // Signalled once the GPU has executed everything up to here; the commands
    // go out with the frame, not now.
    pending_value_ = ++fence_value_;
    context_->EnqueueSignal(summary_fence_, pending_value_);
}

bool HydroSimulation::pollMeasurement(std::string& error) {
    if (pending_value_ == 0 || summary_fence_->GetCompletedValue() < pending_value_) {
        return true;
    }
    pending_value_ = 0;
    return readSummary(summary_pending_, error);
}

bool HydroSimulation::advance(std::uint32_t steps, double target_time, double signal_floor,
                              bool synchronous, std::string& error) {
    std::uint32_t const groups = resolution_ / 8U;
    // The Courant condition is set by the smallest (central) cell.
    double const a = std::max(setup_.stretch, 0.0);
    double const cell = 2.0 * setup_.half_extent / static_cast<double>(resolution_) *
                        (a > 0.0 ? a / std::sinh(a) : 1.0);
    auto* const rays = ray_field_->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
    if (!synchronous && !pollMeasurement(error)) {
        return false;
    }
    bool stepped = false;
    for (std::uint32_t step = 0; step < steps && time_ < target_time; ++step) {
        if (synchronous && steps_ % kMeasureInterval == 0 && !measure(error)) {
            return false;
        }
        double const signal =
            std::max(totals_.fastest_signal * (synchronous ? 1.0 : kStaleMargin), signal_floor);
        if (!(signal > 0.0)) {
            error = "the gas simulation has no signal speed to choose a time step from";
            return false;
        }
        dt_ = std::min(setup_.courant * cell / signal, target_time - time_);

        std::size_t const first = (current_ + 1U) % 3U;
        std::size_t const second = (current_ + 2U) % 3U;
        // Stage 0: current -> first.
        writeParams(dt_, 0);
        bind(advance_, "state_a", stateView(current_, 0));
        bind(advance_, "state_b", stateView(current_, 1));
        bind(advance_, "base_a", stateView(current_, 0));
        bind(advance_, "base_b", stateView(current_, 1));
        bind(advance_, "next_a", stateTarget(first, 0));
        bind(advance_, "next_b", stateTarget(first, 1));
        bind(advance_, "rays", rays);
        dispatch(advance_, groups, groups, groups);
        // Stage 1: first (and current) -> second.
        writeParams(dt_, 1);
        bind(advance_, "state_a", stateView(first, 0));
        bind(advance_, "state_b", stateView(first, 1));
        bind(advance_, "next_a", stateTarget(second, 0));
        bind(advance_, "next_b", stateTarget(second, 1));
        dispatch(advance_, groups, groups, groups);
        current_ = second;
        time_ += dt_;
        ++steps_;
        stepped = true;

        if (setup_.scenario == HydroScenario::Nebula && setup_.source[0][3] > 0.0f &&
            steps_ % kRayInterval == 0) {
            traceRays();
        }
    }
    if (!synchronous && stepped) {
        requestMeasurement();
    }
    return true;
}

void HydroSimulation::traceRays() {
    writeParams(dt_, 0);
    bind(rays_, "density_field", stateView(current_, 0));
    bind(rays_, "rays_out", ray_field_->GetDefaultView(Diligent::TEXTURE_VIEW_UNORDERED_ACCESS));
    Diligent::TextureDesc const& desc = ray_field_->GetDesc();
    dispatch(rays_, (desc.Width + 7U) / 8U, (desc.Height + 7U) / 8U, 1);
}

void HydroSimulation::shade() {
    writeParams(dt_, 0);
    bind(shade_, "state_a", stateView(current_, 0));
    bind(shade_, "state_b", stateView(current_, 1));
    bind(shade_, "rays", ray_field_->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE));
    bind(shade_, "emission_out",
         emission_raw_->GetDefaultView(Diligent::TEXTURE_VIEW_UNORDERED_ACCESS));
    std::uint32_t const groups = resolution_ / 8U;
    dispatch(shade_, groups, groups, groups);
    // One value per cell shows the mesh; filter it for display.
    bind(smooth_, "emission_raw",
         emission_raw_->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE));
    bind(smooth_, "emission_out", emission_->GetDefaultView(Diligent::TEXTURE_VIEW_UNORDERED_ACCESS));
    dispatch(smooth_, groups, groups, groups);
}

bool HydroSimulation::readVolume(Diligent::ITexture* texture, std::vector<float>& out,
                                 std::string& error) {
    Diligent::TextureDesc staging_desc = texture->GetDesc();
    staging_desc.Name = "Helios gas state readback";
    staging_desc.Usage = Diligent::USAGE_STAGING;
    staging_desc.BindFlags = Diligent::BIND_NONE;
    staging_desc.CPUAccessFlags = Diligent::CPU_ACCESS_READ;
    Diligent::RefCntAutoPtr<Diligent::ITexture> staging;
    device_->CreateTexture(staging_desc, nullptr, &staging);
    if (staging == nullptr) {
        error = "Diligent failed to create the gas state readback volume";
        return false;
    }
    Diligent::CopyTextureAttribs copy;
    copy.pSrcTexture = texture;
    copy.SrcTextureTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    copy.pDstTexture = staging;
    copy.DstTextureTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    context_->CopyTexture(copy);
    context_->Flush();
    context_->WaitForIdle();
    Diligent::MappedTextureSubresource mapped;
    context_->MapTextureSubresource(staging, 0, 0, Diligent::MAP_READ,
                                    Diligent::MAP_FLAG_DO_NOT_WAIT, nullptr, mapped);
    if (mapped.pData == nullptr) {
        error = "failed to map the gas state readback volume";
        return false;
    }
    std::size_t const n = resolution_;
    out.resize(n * n * n * 4U);
    for (std::size_t z = 0; z < n; ++z) {
        for (std::size_t y = 0; y < n; ++y) {
            auto const* const row = static_cast<std::uint8_t const*>(mapped.pData) +
                                    z * mapped.DepthStride + y * mapped.Stride;
            std::memcpy(out.data() + (z * n + y) * n * 4U, row, n * 4U * sizeof(float));
        }
    }
    context_->UnmapTextureSubresource(staging, 0, 0);
    return true;
}

bool HydroSimulation::readState(std::vector<float>& a, std::vector<float>& b, std::string& error) {
    return readVolume(state_[current_][0], a, error) && readVolume(state_[current_][1], b, error);
}

} // namespace helios::examples
