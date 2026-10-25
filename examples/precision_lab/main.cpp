// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// One-dispatch, no-window proof of Helios's precision boundary:
// binary64 world storage -> CPU origin subtraction -> bounded float32 GPU
// view transform -> staging readback -> binary64 reference comparison.

#include "Buffer.h"
#include "EngineFactoryVk.h"
#include "PipelineState.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "Shader.h"
#include "shaders_embedded.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifndef HELIOS_GIT_HASH
#define HELIOS_GIT_HASH "unknown"
#endif
#ifndef HELIOS_DILIGENT_COMMIT
#define HELIOS_DILIGENT_COMMIT "unknown"
#endif
#ifndef HELIOS_DILIGENT_CORE_COMMIT
#define HELIOS_DILIGENT_CORE_COMMIT "unknown"
#endif
#ifndef HELIOS_BUILD_TYPE
#define HELIOS_BUILD_TYPE "unknown"
#endif
#ifndef HELIOS_VERSION_STRING
#define HELIOS_VERSION_STRING "unknown"
#endif

namespace {

using Vec3d = std::array<double, 3>;
using Vec3f = std::array<float, 3>;

constexpr std::uint32_t kSampleCount = 8;
constexpr std::uint32_t kPairCount = kSampleCount / 2;
constexpr double kMaximumUploadedMagnitudeM = 16'384.0;
constexpr double kFloatErrorSafetyFactor = 24.0;

// Exactly orthonormal in real arithmetic. Non-axis-aligned coefficients make
// the GPU execute a meaningful view-space dot product rather than a copy.
constexpr std::array<Vec3d, 3> kViewRows{{
    Vec3d{0.36, 0.48, -0.80},
    Vec3d{-0.80, 0.60, 0.0},
    Vec3d{0.48, 0.64, 0.60},
}};

struct Scenario {
    char const* name;
    Vec3d camera_world_m;
    Vec3d base_offset_m;
    double requested_motion_m;
};

// Every scenario is represented by a base/moved pair. The motion grows with
// binary64 ULP at the corresponding world scale; the AU pair still resolves a
// millimetre while the one-light-year pair honestly uses a 16 m step.
constexpr std::array<Scenario, kPairCount> kScenarios{{
    {"near_planet", {1.0e7, -2.5e6, 7.5e5}, {15.125, -23.5, 7.25}, 0.0001},
    {"one_au", {1.495978707e11, -4.1e10, 1.9e10}, {1024.125, -2048.5, 512.25}, 0.001},
    {"solar_system_edge", {1.0e13, -2.5e12, 1.25e12}, {4096.25, -3072.5, 1536.75}, 0.03125},
    {"one_light_year",
     {9.4607304725808e15, -2.3651826181452e15, 1.1825913090726e15},
     {8192.0, -4096.0, 2048.0},
     16.0},
}};

struct GpuPoint {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};
static_assert(sizeof(GpuPoint) == 16);

struct alignas(16) GpuFrame {
    std::array<float, 4> row_0{};
    std::array<float, 4> row_1{};
    std::array<float, 4> row_2{};
    std::array<std::uint32_t, 4> metadata{};
};
static_assert(sizeof(GpuFrame) == 64);

struct Sample {
    Vec3d camera_world_m{};
    Vec3d object_world_m{};
    Vec3d relative_double_m{};
    GpuPoint uploaded{};
    Vec3d reference_view_m{};
    Vec3d naive_float_view_m{};
    std::array<double, 3> allowed_error_m{};
};

std::atomic<std::uint32_t> gValidationWarnings{0};
std::atomic<std::uint32_t> gValidationErrors{0};
std::mutex gValidationMessagesMutex;
std::vector<std::string> gValidationMessages;

void DILIGENT_CALL_TYPE messageCallback(Diligent::DEBUG_MESSAGE_SEVERITY severity,
                                        Diligent::Char const* message,
                                        Diligent::Char const*, Diligent::Char const*, Diligent::Int32) {
    if (severity == Diligent::DEBUG_MESSAGE_SEVERITY_WARNING) {
        ++gValidationWarnings;
    } else if (severity == Diligent::DEBUG_MESSAGE_SEVERITY_ERROR ||
               severity == Diligent::DEBUG_MESSAGE_SEVERITY_FATAL_ERROR) {
        ++gValidationErrors;
    }
    if (severity >= Diligent::DEBUG_MESSAGE_SEVERITY_WARNING) {
        std::scoped_lock lock{gValidationMessagesMutex};
        constexpr std::size_t kMessageLimit = 32;
        if (gValidationMessages.size() < kMessageLimit) {
            gValidationMessages.emplace_back(message != nullptr ? message : "(no message)");
        }
    }
}

[[nodiscard]] Vec3d subtract(Vec3d const& lhs, Vec3d const& rhs) {
    return {lhs[0] - rhs[0], lhs[1] - rhs[1], lhs[2] - rhs[2]};
}

[[nodiscard]] Vec3d transformDouble(Vec3d const& value) {
    Vec3d result{};
    for (std::size_t row = 0; row < result.size(); ++row) {
        for (std::size_t column = 0; column < value.size(); ++column) {
            result[row] += kViewRows[row][column] * value[column];
        }
    }
    return result;
}

[[nodiscard]] Vec3d transformNaiveFloat(Vec3d const& object_world_m, Vec3d const& camera_world_m) {
    Vec3f relative{};
    for (std::size_t i = 0; i < relative.size(); ++i) {
        // This is the intentionally wrong comparison path: casting absolute
        // world coordinates before subtraction destroys local information.
        relative[i] = static_cast<float>(object_world_m[i]) - static_cast<float>(camera_world_m[i]);
    }
    Vec3d result{};
    for (std::size_t row = 0; row < result.size(); ++row) {
        float component = 0.0f;
        for (std::size_t column = 0; column < relative.size(); ++column) {
            component += static_cast<float>(kViewRows[row][column]) * relative[column];
        }
        result[row] = component;
    }
    return result;
}

[[nodiscard]] double maxAbs(Vec3d const& value) {
    return std::max({std::abs(value[0]), std::abs(value[1]), std::abs(value[2])});
}

[[nodiscard]] double maxAbsDifference(Vec3d const& lhs, Vec3d const& rhs) {
    return std::max({std::abs(lhs[0] - rhs[0]), std::abs(lhs[1] - rhs[1]), std::abs(lhs[2] - rhs[2])});
}

[[nodiscard]] double vectorLength(Vec3d const& value) {
    return std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
}

struct EmbeddedShaderView {
    unsigned char const* data = nullptr;
    std::size_t size = 0;
};

[[nodiscard]] EmbeddedShaderView findEmbeddedShader(std::string_view name) {
    for (std::size_t index = 0; index < helios::shaders::embedded::kShaderCount; ++index) {
        auto const& shader = helios::shaders::embedded::kShaders[index];
        if (shader.name != nullptr && name == shader.name) {
            return {shader.data, shader.size};
        }
    }
    return {};
}

[[nodiscard]] std::array<Sample, kSampleCount> buildSamples() {
    std::array<Sample, kSampleCount> samples{};
    for (std::size_t scenario_index = 0; scenario_index < kScenarios.size(); ++scenario_index) {
        Scenario const& scenario = kScenarios[scenario_index];
        for (std::size_t motion_index = 0; motion_index < 2; ++motion_index) {
            Sample& sample = samples[scenario_index * 2 + motion_index];
            sample.camera_world_m = scenario.camera_world_m;
            Vec3d requested_offset = scenario.base_offset_m;
            if (motion_index != 0) {
                requested_offset[0] += scenario.requested_motion_m;
            }
            for (std::size_t component = 0; component < 3; ++component) {
                sample.object_world_m[component] =
                    sample.camera_world_m[component] + requested_offset[component];
            }

            // This is the production precision boundary being tested.
            sample.relative_double_m = subtract(sample.object_world_m, sample.camera_world_m);
            sample.uploaded = {
                static_cast<float>(sample.relative_double_m[0]),
                static_cast<float>(sample.relative_double_m[1]),
                static_cast<float>(sample.relative_double_m[2]),
                1.0f,
            };
            sample.reference_view_m = transformDouble(sample.relative_double_m);
            sample.naive_float_view_m = transformNaiveFloat(sample.object_world_m, sample.camera_world_m);

            // Conservative absolute IEEE-754 float32 bound. It includes
            // binary64->float32 input rounding plus three multiply-add terms.
            for (std::size_t row = 0; row < 3; ++row) {
                double l1_magnitude = 0.0;
                for (std::size_t column = 0; column < 3; ++column) {
                    l1_magnitude += std::abs(kViewRows[row][column] * sample.relative_double_m[column]);
                }
                sample.allowed_error_m[row] =
                    1.0e-6 + kFloatErrorSafetyFactor *
                                 static_cast<double>(std::numeric_limits<float>::epsilon()) * l1_magnitude;
            }
        }
    }
    return samples;
}

struct GpuRun {
    bool succeeded = false;
    std::string error;
    std::string adapter_name;
    std::uint32_t vulkan_major = 0;
    std::uint32_t vulkan_minor = 0;
    std::array<GpuPoint, kSampleCount> output{};
    std::uint32_t validation_warnings = 0;
    std::uint32_t validation_errors = 0;
    std::vector<std::string> validation_messages;
};

[[nodiscard]] GpuRun runGpu(std::span<Sample const, kSampleCount> samples, bool enable_validation) {
    GpuRun run;
    gValidationWarnings.store(0);
    gValidationErrors.store(0);
    {
        std::scoped_lock lock{gValidationMessagesMutex};
        gValidationMessages.clear();
    }

    Diligent::IEngineFactoryVk* factory = Diligent::GetEngineFactoryVk();
    if (factory == nullptr) {
        run.error = "Diligent Vulkan engine factory is unavailable";
        return run;
    }
    factory->SetMessageCallback(messageCallback);

    Diligent::RefCntAutoPtr<Diligent::IRenderDevice> device;
    Diligent::RefCntAutoPtr<Diligent::IDeviceContext> context;
    Diligent::EngineVkCreateInfo engine_info;
    engine_info.EnableValidation = enable_validation;
    factory->CreateDeviceAndContextsVk(engine_info, &device, &context);
    if (device == nullptr || context == nullptr) {
        run.error = "Diligent failed to create a Vulkan device/context";
        factory->SetMessageCallback(nullptr);
        return run;
    }
    if (device->GetDeviceInfo().Type != Diligent::RENDER_DEVICE_TYPE_VULKAN) {
        run.error = "Diligent returned a non-Vulkan device";
        factory->SetMessageCallback(nullptr);
        return run;
    }
    if (device->GetDeviceInfo().Features.ComputeShaders == Diligent::DEVICE_FEATURE_STATE_DISABLED) {
        run.error = "the Vulkan adapter does not expose compute shaders";
        factory->SetMessageCallback(nullptr);
        return run;
    }
    run.adapter_name = device->GetAdapterInfo().Description;
    auto const vulkan_version = factory->GetVulkanVersion();
    run.vulkan_major = vulkan_version.Major;
    run.vulkan_minor = vulkan_version.Minor;

    EmbeddedShaderView const shader_bytes = findEmbeddedShader("diligent_precision.slang.compute.spv");
    if (shader_bytes.data == nullptr || shader_bytes.size < 20 ||
        (shader_bytes.size % sizeof(std::uint32_t)) != 0) {
        run.error = "offline precision SPIR-V is missing or malformed";
        factory->SetMessageCallback(nullptr);
        return run;
    }
    std::uint32_t spirv_magic = 0;
    std::memcpy(&spirv_magic, shader_bytes.data, sizeof(spirv_magic));
    if (spirv_magic != 0x07230203U) {
        run.error = "offline precision shader does not begin with the SPIR-V magic";
        factory->SetMessageCallback(nullptr);
        return run;
    }

    Diligent::ShaderCreateInfo shader_info;
    shader_info.Desc.Name = "Helios Diligent camera-relative precision compute";
    shader_info.Desc.ShaderType = Diligent::SHADER_TYPE_COMPUTE;
    shader_info.EntryPoint = "main";
    shader_info.ByteCode = shader_bytes.data;
    shader_info.ByteCodeSize = shader_bytes.size;
    Diligent::RefCntAutoPtr<Diligent::IShader> compute_shader;
    device->CreateShader(shader_info, &compute_shader);
    if (compute_shader == nullptr) {
        run.error = "Diligent rejected the offline precision SPIR-V";
        factory->SetMessageCallback(nullptr);
        return run;
    }

    GpuFrame frame{};
    for (std::size_t component = 0; component < 3; ++component) {
        frame.row_0[component] = static_cast<float>(kViewRows[0][component]);
        frame.row_1[component] = static_cast<float>(kViewRows[1][component]);
        frame.row_2[component] = static_cast<float>(kViewRows[2][component]);
    }
    frame.metadata[0] = kSampleCount;

    Diligent::BufferDesc frame_desc;
    frame_desc.Name = "Helios precision frame constants";
    frame_desc.Size = sizeof(frame);
    frame_desc.Usage = Diligent::USAGE_IMMUTABLE;
    frame_desc.BindFlags = Diligent::BIND_UNIFORM_BUFFER;
    Diligent::BufferData frame_data{&frame, sizeof(frame)};
    Diligent::RefCntAutoPtr<Diligent::IBuffer> frame_buffer;
    device->CreateBuffer(frame_desc, &frame_data, &frame_buffer);

    std::array<GpuPoint, kSampleCount> input_points{};
    for (std::size_t i = 0; i < input_points.size(); ++i) {
        input_points[i] = samples[i].uploaded;
    }
    Diligent::BufferDesc input_desc;
    input_desc.Name = "Helios precision camera-relative input";
    input_desc.Size = sizeof(input_points);
    input_desc.Usage = Diligent::USAGE_IMMUTABLE;
    input_desc.BindFlags = Diligent::BIND_SHADER_RESOURCE;
    input_desc.Mode = Diligent::BUFFER_MODE_STRUCTURED;
    input_desc.ElementByteStride = sizeof(GpuPoint);
    Diligent::BufferData input_data{input_points.data(), sizeof(input_points)};
    Diligent::RefCntAutoPtr<Diligent::IBuffer> input_buffer;
    device->CreateBuffer(input_desc, &input_data, &input_buffer);

    Diligent::BufferDesc output_desc;
    output_desc.Name = "Helios precision view-space output";
    output_desc.Size = sizeof(run.output);
    output_desc.Usage = Diligent::USAGE_DEFAULT;
    output_desc.BindFlags = Diligent::BIND_UNORDERED_ACCESS;
    output_desc.Mode = Diligent::BUFFER_MODE_STRUCTURED;
    output_desc.ElementByteStride = sizeof(GpuPoint);
    Diligent::RefCntAutoPtr<Diligent::IBuffer> output_buffer;
    device->CreateBuffer(output_desc, nullptr, &output_buffer);

    Diligent::BufferDesc staging_desc;
    staging_desc.Name = "Helios precision readback staging";
    staging_desc.Size = sizeof(run.output);
    staging_desc.Usage = Diligent::USAGE_STAGING;
    staging_desc.BindFlags = Diligent::BIND_NONE;
    staging_desc.CPUAccessFlags = Diligent::CPU_ACCESS_READ;
    Diligent::RefCntAutoPtr<Diligent::IBuffer> staging_buffer;
    device->CreateBuffer(staging_desc, nullptr, &staging_buffer);

    if (frame_buffer == nullptr || input_buffer == nullptr || output_buffer == nullptr ||
        staging_buffer == nullptr) {
        run.error = "Diligent failed to create precision lab buffers";
        factory->SetMessageCallback(nullptr);
        return run;
    }

    Diligent::ComputePipelineStateCreateInfo pipeline_info;
    pipeline_info.PSODesc.Name = "Helios Diligent precision pipeline";
    pipeline_info.PSODesc.PipelineType = Diligent::PIPELINE_TYPE_COMPUTE;
    pipeline_info.PSODesc.ResourceLayout.DefaultVariableType =
        Diligent::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
    pipeline_info.pCS = compute_shader;
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> pipeline;
    device->CreateComputePipelineState(pipeline_info, &pipeline);
    if (pipeline == nullptr) {
        run.error = "Diligent failed to create the precision compute pipeline";
        factory->SetMessageCallback(nullptr);
        return run;
    }

    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> resources;
    pipeline->CreateShaderResourceBinding(&resources, true);
    auto* frame_variable =
        resources != nullptr ? resources->GetVariableByName(Diligent::SHADER_TYPE_COMPUTE, "precision_frame")
                             : nullptr;
    auto* input_variable = resources != nullptr ? resources->GetVariableByName(Diligent::SHADER_TYPE_COMPUTE,
                                                                               "camera_relative_positions")
                                                : nullptr;
    auto* output_variable = resources != nullptr ? resources->GetVariableByName(Diligent::SHADER_TYPE_COMPUTE,
                                                                                "view_space_positions")
                                                 : nullptr;
    auto* input_view = input_buffer->GetDefaultView(Diligent::BUFFER_VIEW_SHADER_RESOURCE);
    auto* output_view = output_buffer->GetDefaultView(Diligent::BUFFER_VIEW_UNORDERED_ACCESS);
    if (resources == nullptr || frame_variable == nullptr || input_variable == nullptr ||
        output_variable == nullptr || input_view == nullptr || output_view == nullptr) {
        run.error = "Diligent could not reflect/bind precision shader resources";
        factory->SetMessageCallback(nullptr);
        return run;
    }
    frame_variable->Set(frame_buffer);
    input_variable->Set(input_view);
    output_variable->Set(output_view);

    context->SetPipelineState(pipeline);
    context->CommitShaderResources(resources, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Diligent::DispatchComputeAttribs dispatch;
    dispatch.ThreadGroupCountX = 1;
    dispatch.ThreadGroupCountY = 1;
    dispatch.ThreadGroupCountZ = 1;
    context->DispatchCompute(dispatch);
    context->CopyBuffer(output_buffer, 0, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION, staging_buffer,
                        0, sizeof(run.output), Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->WaitForIdle();

    void* mapped = nullptr;
    context->MapBuffer(staging_buffer, Diligent::MAP_READ, Diligent::MAP_FLAG_DO_NOT_WAIT, mapped);
    if (mapped == nullptr) {
        run.error = "Diligent precision staging readback returned null";
        factory->SetMessageCallback(nullptr);
        return run;
    }
    std::memcpy(run.output.data(), mapped, sizeof(run.output));
    context->UnmapBuffer(staging_buffer, Diligent::MAP_READ);

    run.validation_warnings = gValidationWarnings.load();
    run.validation_errors = gValidationErrors.load();
    {
        std::scoped_lock lock{gValidationMessagesMutex};
        run.validation_messages = gValidationMessages;
    }
    run.succeeded = true;
    factory->SetMessageCallback(nullptr);
    return run;
}

[[nodiscard]] Vec3d pointToDouble(GpuPoint const& point) {
    return {static_cast<double>(point.x), static_cast<double>(point.y), static_cast<double>(point.z)};
}

[[nodiscard]] nlohmann::ordered_json makeReport(std::span<Sample const, kSampleCount> samples,
                                                GpuRun const& gpu) {
    using Json = nlohmann::ordered_json;
    Json report = Json::object();
    report["schema"] = "helios.diligent_precision_lab";
    report["schema_version"] = 1;
    report["backend"] = "vulkan";
    report["window_created"] = false;
    report["provenance"] = {
        {"helios_version", HELIOS_VERSION_STRING},
        {"helios_commit", HELIOS_GIT_HASH},
        {"diligent_engine_commit", HELIOS_DILIGENT_COMMIT},
        {"diligent_core_commit", HELIOS_DILIGENT_CORE_COMMIT},
        {"build_type", HELIOS_BUILD_TYPE},
        {"shader", "diligent_precision.slang.compute.spv (embedded)"},
        {"adapter", gpu.adapter_name},
        {"vulkan_version",
         std::to_string(gpu.vulkan_major) + "." + std::to_string(gpu.vulkan_minor)},
    };
    report["contract"] = {
        {"world_storage", "binary64"},
        {"origin_subtraction", "cpu_binary64_before_gpu_upload"},
        {"gpu_transform", "camera_relative_float32"},
        {"maximum_uploaded_component_m", kMaximumUploadedMagnitudeM},
    };
    report["work"] = {
        {"dispatch_count", 1},
        {"thread_group_count", 1},
        {"threads_per_group", kSampleCount},
        {"readback_bytes", sizeof(gpu.output)},
    };

    bool all_passed = gpu.succeeded;
    double global_max_gpu_error = 0.0;
    double global_max_naive_error = 0.0;
    Json scenarios = Json::array();
    for (std::size_t scenario_index = 0; scenario_index < kScenarios.size(); ++scenario_index) {
        Scenario const& specification = kScenarios[scenario_index];
        Sample const& base = samples[scenario_index * 2];
        Sample const& moved = samples[scenario_index * 2 + 1];
        Vec3d const base_gpu = pointToDouble(gpu.output[scenario_index * 2]);
        Vec3d const moved_gpu = pointToDouble(gpu.output[scenario_index * 2 + 1]);

        Vec3d gpu_motion{};
        Vec3d reference_motion{};
        for (std::size_t component = 0; component < 3; ++component) {
            gpu_motion[component] = moved_gpu[component] - base_gpu[component];
            reference_motion[component] =
                moved.reference_view_m[component] - base.reference_view_m[component];
        }
        double const represented_motion = moved.relative_double_m[0] - base.relative_double_m[0];
        double const gpu_base_error = maxAbsDifference(base_gpu, base.reference_view_m);
        double const gpu_moved_error = maxAbsDifference(moved_gpu, moved.reference_view_m);
        double const gpu_error = std::max(gpu_base_error, gpu_moved_error);
        double const allowed_error =
            std::max(*std::max_element(base.allowed_error_m.begin(), base.allowed_error_m.end()),
                     *std::max_element(moved.allowed_error_m.begin(), moved.allowed_error_m.end()));
        double const micro_motion_error = maxAbsDifference(gpu_motion, reference_motion);
        double const micro_motion_allowed = allowed_error * 2.0;
        double const naive_error =
            std::max(maxAbsDifference(base.naive_float_view_m, base.reference_view_m),
                     maxAbsDifference(moved.naive_float_view_m, moved.reference_view_m));
        double const uploaded_max = std::max(maxAbs(base.relative_double_m), maxAbs(moved.relative_double_m));
        bool const pair_passed = std::isfinite(gpu_error) && uploaded_max <= kMaximumUploadedMagnitudeM &&
                                 represented_motion > 0.0 && vectorLength(gpu_motion) > 0.0 &&
                                 gpu_error <= allowed_error && micro_motion_error <= micro_motion_allowed;

        global_max_gpu_error = std::max(global_max_gpu_error, gpu_error);
        global_max_naive_error = std::max(global_max_naive_error, naive_error);
        all_passed = all_passed && pair_passed;

        double const camera_ulp =
            std::nextafter(specification.camera_world_m[0], std::numeric_limits<double>::infinity()) -
            specification.camera_world_m[0];
        Json entry = Json::object();
        entry["name"] = specification.name;
        entry["world_scale_m"] = maxAbs(specification.camera_world_m);
        entry["world_binary64_ulp_m"] = camera_ulp;
        entry["requested_motion_m"] = specification.requested_motion_m;
        entry["represented_motion_after_binary64_subtraction_m"] = represented_motion;
        entry["gpu_view_motion_m"] = gpu_motion;
        entry["cpu_double_reference_view_motion_m"] = reference_motion;
        entry["micro_motion_max_error_m"] = micro_motion_error;
        entry["micro_motion_allowed_error_m"] = micro_motion_allowed;
        entry["maximum_uploaded_component_m"] = uploaded_max;
        entry["gpu_transform_max_error_m"] = gpu_error;
        entry["gpu_transform_allowed_error_m"] = allowed_error;
        entry["naive_absolute_float_world_transform_max_error_m"] = naive_error;
        entry["passed"] = pair_passed;
        scenarios.push_back(std::move(entry));
    }
    report["scenarios"] = std::move(scenarios);
    report["summary"] = {
        {"scenario_count", kPairCount},
        {"sample_count", kSampleCount},
        {"maximum_gpu_transform_error_m", global_max_gpu_error},
        {"maximum_naive_world_float_error_m", global_max_naive_error},
        {"validation_warning_count", gpu.validation_warnings},
        {"validation_error_count", gpu.validation_errors},
        {"validation_messages", gpu.validation_messages},
    };
    all_passed = all_passed && gpu.validation_errors == 0;
    report["passed"] = all_passed;
    return report;
}

void printUsage(char const* executable) {
    std::cout << "Usage: " << executable << " [--ctest] [--no-validation]\n"
              << "Runs one headless Diligent/Vulkan compute dispatch and emits JSON.\n";
}

} // namespace

int main(int argc, char** argv) {
    bool ctest_mode = false;
    bool enable_validation = true;
    for (int i = 1; i < argc; ++i) {
        std::string_view const argument = argv[i];
        if (argument == "--ctest") {
            ctest_mode = true;
        } else if (argument == "--no-validation") {
            enable_validation = false;
        } else if (argument == "--help" || argument == "-h") {
            printUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "unknown argument: " << argument << '\n';
            printUsage(argv[0]);
            return 64;
        }
    }
    (void)ctest_mode; // The current lab is already the permanently bounded path.

    try {
        auto const samples = buildSamples();
        GpuRun const gpu = runGpu(samples, enable_validation);
        if (!gpu.succeeded) {
            nlohmann::ordered_json failure = {
                {"schema", "helios.diligent_precision_lab"},
                {"schema_version", 1},
                {"backend", "vulkan"},
                {"window_created", false},
                {"provenance",
                 {{"helios_version", HELIOS_VERSION_STRING},
                  {"helios_commit", HELIOS_GIT_HASH},
                  {"diligent_engine_commit", HELIOS_DILIGENT_COMMIT},
                  {"diligent_core_commit", HELIOS_DILIGENT_CORE_COMMIT},
                  {"build_type", HELIOS_BUILD_TYPE}}},
                {"passed", false},
                {"error", gpu.error},
            };
            std::cout << failure.dump(2) << '\n';
            return 2;
        }

        nlohmann::ordered_json const report = makeReport(samples, gpu);
        std::cout << report.dump(2) << '\n';
        return report.at("passed").get<bool>() ? 0 : 3;
    } catch (std::exception const& error) {
        nlohmann::ordered_json failure = {
            {"schema", "helios.diligent_precision_lab"},
            {"schema_version", 1},
            {"backend", "vulkan"},
            {"window_created", false},
            {"provenance",
             {{"helios_version", HELIOS_VERSION_STRING},
              {"helios_commit", HELIOS_GIT_HASH},
              {"diligent_engine_commit", HELIOS_DILIGENT_COMMIT},
              {"diligent_core_commit", HELIOS_DILIGENT_CORE_COMMIT},
              {"build_type", HELIOS_BUILD_TYPE}}},
            {"passed", false},
            {"error", error.what()},
        };
        std::cout << failure.dump(2) << '\n';
        return 4;
    }
}
