# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 the Helios authors and contributors
#
# Dear ImGui static library (MIT) with SDL3 + Vulkan backends.
# Included from HeliosDeps.cmake after FetchContent_MakeAvailable(ImGui).
if(HELIOS_ENABLE_IMGUI AND NOT TARGET imgui)
  add_library(imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_demo.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp")
  target_include_directories(imgui PUBLIC "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
  target_link_libraries(imgui PRIVATE SDL3::SDL3 Vulkan::Headers)
  # volk integration: no loader prototypes; the app provides function
  # pointers via ImGui_ImplVulkan_LoadFunctions (see renderer.cpp).
  target_compile_definitions(imgui PRIVATE VK_NO_PROTOTYPES IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
  helios_configure_target(imgui)
endif()
