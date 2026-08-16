// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Renderer: swapchain-backed windowed path with the M0 smoke scene
// (triangle + depth probes) and optional ImGui overlay. Two frames in
// flight; serialized single-queue baseline.
#include "helios/render/renderer.hpp"

#include "helios/core/log.hpp"
#include "helios/core/profiler.hpp"
#include "helios/render/frame.hpp"
#include "helios/vk/device.hpp"
#include "helios/vk/instance.hpp"
#include "helios/vk/command.hpp"
#include "helios/vk/smoke/smoke_suite.hpp"
#include "helios/vk/swapchain.hpp"
#include "helios/vk/sync.hpp"

#include <array>
#include <cstring>
#include <filesystem>
#include <vector>

#include <SDL3/SDL.h>
#include <volk.h>

#ifdef HELIOS_ENABLE_IMGUI
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>
#endif

namespace fs = std::filesystem;

namespace helios::render {

namespace {
struct DepthImage {
    vk::Image image;
    VkImageView view = VK_NULL_HANDLE;
};
} // namespace

struct Renderer::Impl {
    vk::Instance* instance = nullptr;
    vk::Device* device = nullptr;
    vk::Swapchain* swapchain = nullptr;
    std::unique_ptr<vk::Instance> owned_instance;
    std::unique_ptr<vk::Device> owned_device;
    std::unique_ptr<vk::Swapchain> owned_swapchain;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    std::unique_ptr<vk::smoke::WindowedSmokeScene> scene;
    std::vector<FrameContext> frames;
    std::uint32_t frame_index = 0; // current in-flight slot
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool validation_enabled = false;
    bool imgui_enabled = false;
    SDL_Window* window = nullptr;
    std::uint32_t max_frames = 0;
    std::uint64_t frame_count = 0;
    DepthImage depth;
    // GPU timing
    vk::QueryPool timestamp_pool;
    std::array<std::uint64_t, 4> timestamp_values{};
    std::uint32_t timestamps_ready = 0;
    // ImGui Vulkan init
    VkDescriptorPool imgui_pool = VK_NULL_HANDLE;
    VkFormat swapchain_format = VK_FORMAT_UNDEFINED;
    bool imgui_initialized = false;
    // Swapchain image views (rebuilt on swapchain recreation)
    std::vector<VkImageView> swapchain_image_views;

    ~Impl();
};

Renderer::Impl::~Impl() = default;

core::Result<std::unique_ptr<Renderer>> Renderer::create(RendererCreateInfo const& info) {
    auto renderer = std::unique_ptr<Renderer>(new Renderer());
    auto impl = std::make_unique<Impl>();
    impl->window = info.window;
    impl->width = info.width;
    impl->height = info.height;
    impl->validation_enabled = info.enable_validation;
    impl->imgui_enabled = info.enable_imgui && HELIOS_ENABLE_IMGUI;
    impl->max_frames = info.max_frames;

    vk::InstanceOptions instance_options;
    instance_options.enable_validation = info.enable_validation;
    instance_options.require_validation = false;
    auto instance = vk::Instance::create(instance_options);
    if (instance.failed()) {
        return instance.error();
    }
    impl->owned_instance = std::move(instance.value());
    impl->instance = impl->owned_instance.get();

    auto surface = impl->instance->createSurface(info.window);
    if (surface.failed()) {
        return surface.error();
    }
    impl->surface = surface.value();

    vk::DeviceOptions device_options;
    device_options.require_swapchain = true;
    auto device = vk::Device::create(*impl->instance, impl->surface, device_options);
    if (device.failed()) {
        return device.error();
    }
    impl->owned_device = std::move(device.value());
    impl->device = impl->owned_device.get();

    auto swapchain = vk::Swapchain::create(*impl->device, impl->surface, info.width, info.height);
    if (swapchain.failed()) {
        return swapchain.error();
    }
    impl->owned_swapchain = std::move(swapchain.value());
    impl->swapchain = impl->owned_swapchain.get();
    impl->width = impl->swapchain->info().extent.width;
    impl->height = impl->swapchain->info().extent.height;
    impl->swapchain_format = impl->swapchain->info().format;
    for (auto image : impl->swapchain->images()) {
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = impl->swapchain_format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        if (vkCreateImageView(impl->device->handle(), &vci, nullptr, &view) != VK_SUCCESS) {
            return core::makeError(core::ErrorCode::Vulkan, "swapchain image view creation failed");
        }
        impl->swapchain_image_views.push_back(view);
    }

    // Frames in flight.
    const std::uint32_t frame_count = std::max(2u, impl->swapchain->info().image_count > 2 ? 2u : 2u);
    for (std::uint32_t i = 0; i < frame_count; ++i) {
        auto frame = createFrameContext(*impl->device, std::to_string(i));
        if (frame.failed()) {
            return frame.error();
        }
        impl->frames.push_back(std::move(frame.value()));
    }

    // Depth attachment for windowed depth probes.
    VkFormat depth_format = impl->device->report().depth_choice == vk::DepthFormatChoice::D32Sfloat
                                ? VK_FORMAT_D32_SFLOAT
                                : impl->device->report().depth_choice == vk::DepthFormatChoice::D32SfloatS8
                                      ? VK_FORMAT_D32_SFLOAT_S8_UINT
                                      : VK_FORMAT_D24_UNORM_S8_UINT;
    auto depth_image = vk::Image::create(*impl->device, VkExtent2D{impl->width, impl->height}, depth_format,
                                         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, false, "window-depth");
    if (depth_image.failed()) {
        return depth_image.error();
    }
    impl->depth.image = std::move(depth_image.value());
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = impl->depth.image.handle();
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = depth_format;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(impl->device->handle(), &view_info, nullptr, &impl->depth.view) != VK_SUCCESS) {
        return core::makeError(core::ErrorCode::Vulkan, "depth image view creation failed");
    }
    impl->device->setDebugName(VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<std::uint64_t>(impl->depth.view),
                               "window-depth-view");

    // GPU frame timing pool.
    auto timestamps = vk::QueryPool::create(*impl->device, VK_QUERY_TYPE_TIMESTAMP, 4, "frame-timestamps");
    if (timestamps.failed()) {
        return timestamps.error();
    }
    impl->timestamp_pool = std::move(timestamps.value());

    // Smoke scene (windowed).
    auto scene = vk::smoke::WindowedSmokeScene::create(*impl->device, impl->swapchain_format, depth_format);
    if (scene.failed()) {
        return scene.error();
    }
    impl->scene = std::move(scene.value());

    // ImGui initialization.
    if (impl->imgui_enabled) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();

        ImGui_ImplSDL3_InitForVulkan(info.window);

        ImGui_ImplVulkan_InitInfo init_info{};
        init_info.ApiVersion = VK_API_VERSION_1_3;
        init_info.Instance = impl->instance->handle();
        init_info.PhysicalDevice = impl->device->physicalDevice();
        init_info.Device = impl->device->handle();
        init_info.QueueFamily = impl->device->graphicsQueueFamily();
        init_info.Queue = impl->device->graphicsQueue();
        init_info.DescriptorPoolSize = 64; // ImGui creates its own pool
        init_info.MinImageCount = 2;
        init_info.ImageCount = static_cast<std::uint32_t>(impl->frames.size());
        init_info.UseDynamicRendering = true;
        init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        init_info.PipelineInfoMain.PipelineRenderingCreateInfo.sType =
            VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
        init_info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        init_info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &impl->swapchain_format;
        if (!ImGui_ImplVulkan_Init(&init_info)) {
            return core::makeError(core::ErrorCode::Initialization, "ImGui_ImplVulkan_Init failed");
        }
        impl->imgui_initialized = true;
        // Font atlas texture is created during ImGui_ImplVulkan_Init (new API).
    }

    renderer->impl_ = std::move(impl);
    renderer->device_name_ = renderer->impl_->device->report().device_name;
    renderer->width_ = renderer->impl_->width;
    renderer->height_ = renderer->impl_->height;
    return renderer;
}

Renderer::~Renderer() {
    if (impl_ != nullptr && impl_->device != nullptr) {
        impl_->device->waitIdle();
        for (auto& frame : impl_->frames) {
            destroyFrameContext(*impl_->device, frame);
        }
        for (VkImageView v : impl_->swapchain_image_views) {
            vkDestroyImageView(impl_->device->handle(), v, nullptr);
        }
        impl_->swapchain_image_views.clear();
        if (impl_->depth.view != VK_NULL_HANDLE) {
            vkDestroyImageView(impl_->device->handle(), impl_->depth.view, nullptr);
            impl_->depth.view = VK_NULL_HANDLE;
        }
#ifdef HELIOS_ENABLE_IMGUI
        if (impl_->imgui_initialized) {
            ImGui_ImplVulkan_Shutdown();
            ImGui_ImplSDL3_Shutdown();
            ImGui::DestroyContext();
        }
#endif
        if (impl_->surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(impl_->instance->handle(), impl_->surface, nullptr);
            impl_->surface = VK_NULL_HANDLE;
        }
        impl_->scene.reset();
        impl_->owned_swapchain.reset();
        impl_->owned_device.reset();
        impl_->owned_instance.reset();
    }
}

bool Renderer::renderFrame() {
    HELIOS_ZONE_SCOPED;
    auto& impl = *impl_;
    if (impl.frame_count >= impl.max_frames && impl.max_frames > 0) {
        return false;
    }

    const core::Clock::time_point frame_start = core::Clock::now();
    FrameContext& frame = impl.frames[impl.frame_index % impl.frames.size()];

    // Wait for the previous use of this frame slot.
    if (vkWaitForFences(impl.device->handle(), 1, &frame.in_flight, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        core::logError("fence wait failed");
        return false;
    }

    // Acquire next swapchain image.
    std::uint32_t image_index = 0;
    auto acquire = impl.swapchain->acquireNextImage(frame.image_available, frame.in_flight);
    if (acquire.failed()) {
        if (acquire.error().code() == core::ErrorCode::Vulkan) {
            // OUT_OF_DATE: recreate and retry once.
            core::logInfo("swapchain out of date; recreating");
            SDL_Window* win = impl.window;
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(win, &w, &h);
            if (impl.swapchain->recreate(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)).failed()) {
                core::logError("swapchain recreation failed");
                return false;
            }
            for (VkImageView v : impl.swapchain_image_views) {
                vkDestroyImageView(impl.device->handle(), v, nullptr);
            }
            impl.swapchain_image_views.clear();
            for (auto image : impl.swapchain->images()) {
                VkImageViewCreateInfo vci{};
                vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                vci.image = image;
                vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
                vci.format = impl.swapchain_format;
                vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                VkImageView view = VK_NULL_HANDLE;
                vkCreateImageView(impl.device->handle(), &vci, nullptr, &view);
                impl.swapchain_image_views.push_back(view);
            }
            impl.width = impl.swapchain->info().extent.width;
            impl.height = impl.swapchain->info().extent.height;
            return true; // try again next frame
        }
        return false;
    }
    image_index = acquire.value();

    vkResetFences(impl.device->handle(), 1, &frame.in_flight);

    // Begin recording.
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(frame.cmd, &begin);

    const VkExtent2D extent{impl.width, impl.height};
    const VkImage swap_image = impl.swapchain->image(image_index);

    // Swapchain image: undefined -> color attachment.
    VkImageMemoryBarrier2 to_color{};
    to_color.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    to_color.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    to_color.srcAccessMask = 0;
    to_color.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    to_color.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    to_color.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_color.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_color.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_color.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_color.image = swap_image;
    to_color.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &to_color;
    vkCmdPipelineBarrier2(frame.cmd, &dep);

    // Depth image: undefined -> depth attachment.
    VkImageMemoryBarrier2 to_depth{};
    to_depth.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    to_depth.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    to_depth.srcAccessMask = 0;
    to_depth.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
    to_depth.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    to_depth.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_depth.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    to_depth.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_depth.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_depth.image = impl.depth.image.handle();
    to_depth.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep2{};
    dep2.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep2.imageMemoryBarrierCount = 1;
    dep2.pImageMemoryBarriers = &to_depth;
    vkCmdPipelineBarrier2(frame.cmd, &dep2);

    // GPU timestamps: begin.
    if (impl.timestamp_pool.handle() != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(frame.cmd, impl.timestamp_pool.handle(), 0, 2);
        vkCmdWriteTimestamp2(frame.cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, impl.timestamp_pool.handle(), 0);
    }

    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = impl.swapchain_image_views[image_index];
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0.08f, 0.08f, 0.12f, 1.0f}};

    VkRenderingAttachmentInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView = impl.depth.view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.clearValue.depthStencil = {0.0f, 0};

    VkRenderingInfo render{};
    render.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    render.renderArea = {{0, 0}, extent};
    render.layerCount = 1;
    render.colorAttachmentCount = 1;
    render.pColorAttachments = &color;
    render.pDepthAttachment = &depth;

    vkCmdBeginRendering(frame.cmd, &render);

    // Smoke scene (triangle + depth probes).
    impl.scene->record(frame.cmd, extent, static_cast<std::uint32_t>(impl.frame_count));

#ifdef HELIOS_ENABLE_IMGUI
    if (impl.imgui_enabled) {
        // ImGui frame (CPU).
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        drawDebugUi();
        ImGui::Render();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), frame.cmd);
    }
#endif

    vkCmdEndRendering(frame.cmd);

    // GPU timestamps: end.
    if (impl.timestamp_pool.handle() != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp2(frame.cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, impl.timestamp_pool.handle(), 1);
    }

    // Color attachment -> present source.
    VkImageMemoryBarrier2 to_present{};
    to_present.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    to_present.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    to_present.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    to_present.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
    to_present.dstAccessMask = 0;
    to_present.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.image = swap_image;
    to_present.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep3{};
    dep3.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep3.imageMemoryBarrierCount = 1;
    dep3.pImageMemoryBarriers = &to_present;
    vkCmdPipelineBarrier2(frame.cmd, &dep3);

    if (vkEndCommandBuffer(frame.cmd) != VK_SUCCESS) {
        core::logError("vkEndCommandBuffer failed");
        return false;
    }

    // Submit.
    VkSemaphoreSubmitInfo wait_sem{};
    wait_sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    wait_sem.semaphore = frame.image_available;
    wait_sem.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signal_sem{};
    signal_sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signal_sem.semaphore = frame.render_finished;
    signal_sem.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cmd_info{};
    cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmd_info.commandBuffer = frame.cmd;
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &wait_sem;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal_sem;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmd_info;
    if (vkQueueSubmit2(impl.device->graphicsQueue(), 1, &submit, frame.in_flight) != VK_SUCCESS) {
        core::logError("vkQueueSubmit2 failed");
        return false;
    }
    frame.submitted = true;

    // Present.
    auto present = impl.swapchain->present(image_index, frame.render_finished);
    if (present.failed()) {
        core::logInfo("present failed; recreating swapchain");
        SDL_Window* win = impl.window;
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(win, &w, &h);
        impl.swapchain->recreate(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
        impl.width = impl.swapchain->info().extent.width;
        impl.height = impl.swapchain->info().extent.height;
    }

    // Timing.
    const double cpu_ms = std::chrono::duration<double, std::milli>(core::Clock::now() - frame_start).count();
    stats_.cpu_frame_ms.add(cpu_ms);

    // Read back GPU timestamps (lag by frames-in-flight for availability).
    if (impl.timestamp_pool.handle() != VK_NULL_HANDLE) {
        if (impl.timestamps_ready >= impl.frames.size()) {
            std::array<std::uint64_t, 4> values{};
            if (vkGetQueryPoolResults(impl.device->handle(), impl.timestamp_pool.handle(), 0, 2,
                                      sizeof(std::uint64_t) * 2, values.data(), sizeof(std::uint64_t),
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                const float period = impl.device->timestampPeriodNanos();
                const double gpu_ms = static_cast<double>(values[1] - values[0]) * period / 1e6;
                stats_.gpu_frame_ms.add(gpu_ms);
            }
        } else {
            ++impl.timestamps_ready;
        }
    }

    ++impl.frame_count;
    ++stats_.presented_frames;
    HELIOS_FRAME_MARK;
    return true;
}

std::string Renderer::deviceReportJson() const {
    return impl_->device->reportJson().dump(2);
}

std::uint32_t Renderer::validationErrorCount() const {
    return impl_->instance->validationStats().error_count;
}

std::uint32_t Renderer::validationWarningCount() const {
    return impl_->instance->validationStats().warning_count;
}

} // namespace helios::render
