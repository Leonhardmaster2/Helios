# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 the Helios authors and contributors

# The v0.0.1 interactive host currently uses SDL's Win32 HWND bridge. Keep the
# CPU planet/terrain tests portable while the Linux native-window bridge is a
# later milestone.
if(NOT WIN32)
  message(STATUS "Skipping helios_planet_lod_lab: Win32 SDL/Diligent bridge only in v0.0.1")
  return()
endif()

if(NOT HELIOS_ENABLE_IMGUI OR NOT TARGET Diligent-Imgui)
  message(STATUS "Skipping helios_planet_lod_lab: HELIOS_ENABLE_IMGUI is required")
  return()
endif()

add_executable(helios_planet_lod_lab
  main.cpp
  planet_patch_renderer.cpp
  planet_surface.cpp
  planet_system.cpp
  ../common/diligent_headless.cpp
  ../common/diligent_sdl_app.cpp)

target_include_directories(helios_planet_lod_lab PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../common"
  "${CMAKE_SOURCE_DIR}") # shaders/abi.hpp

# Deliberately do not link helios_vk, helios_render, or their Volk loader.
# Diligent owns the one Vulkan device/context/swap-chain path in this process.
# helios_diligent_render supplies the PNG writer for the offscreen capture route.
target_link_libraries(helios_planet_lod_lab PRIVATE
  helios_planet
  helios_diligent_render
  helios_shader_data
  Diligent-GraphicsEngineVk-static
  Diligent-GraphicsTools
  Diligent-Common
  Diligent-Imgui
  SDL3::SDL3-static
  imgui)

add_dependencies(helios_planet_lod_lab helios_shaders)
helios_configure_target(helios_planet_lod_lab)

set_target_properties(helios_planet_lod_lab PROPERTIES
  FOLDER "Helios/Examples"
  VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
