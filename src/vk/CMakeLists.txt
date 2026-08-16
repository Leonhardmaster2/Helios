# SPDX-License-Identifier: AGPL-3.0-only
# helios_vk: instance/device selection, queues, swapchain/headless path,
# VMA resources, descriptors, sync, pipeline cache. Depends on core only.
add_library(helios_vk STATIC
  instance.cpp
  device.cpp
  capabilities.cpp
  vma_allocator.cpp
  buffer.cpp
  image.cpp
  swapchain.cpp
  shader_module.cpp
  pipeline.cpp
  descriptor_set.cpp
  sync.cpp
  command.cpp
  smoke/smoke_context.cpp
  smoke/smoke_suite.cpp
  smoke/windowed_scene.cpp)
target_include_directories(helios_vk PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(helios_vk PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(helios_vk PUBLIC helios_core Vulkan::Headers helios_volk helios_shader_data)
target_link_libraries(helios_vk PUBLIC helios_vma)
target_link_libraries(helios_vk PRIVATE SDL3::SDL3)
target_compile_definitions(helios_vk PUBLIC VK_NO_PROTOTYPES)
if(WIN32)
  target_compile_definitions(helios_vk PUBLIC VK_USE_PLATFORM_WIN32_KHR)
endif()
helios_configure_target(helios_vk)
