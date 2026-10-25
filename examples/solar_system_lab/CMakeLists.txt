# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 the Helios authors and contributors

# The v0.0.1 interactive host uses SDL's Win32 HWND bridge, like the planet
# LOD and deep-sky labs.
if(NOT WIN32)
  message(STATUS "Skipping helios_solar_system: Win32 SDL/Diligent bridge only in v0.0.1")
  return()
endif()

if(NOT HELIOS_ENABLE_IMGUI OR NOT TARGET Diligent-Imgui)
  message(STATUS "Skipping helios_solar_system: HELIOS_ENABLE_IMGUI is required")
  return()
endif()

add_executable(helios_solar_system
  main.cpp
  solar_system.cpp
  solar_system_stage.cpp
  ../planet_lod_lab/planet_patch_renderer.cpp
  ../planet_lod_lab/planet_surface.cpp
  ../planet_lod_lab/planet_system.cpp
  ../common/diligent_headless.cpp
  ../common/diligent_sdl_app.cpp
  ../common/fullscreen_pass.cpp
  ../common/hdr_post_chain.cpp)

target_include_directories(helios_solar_system PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../common"
  "${CMAKE_CURRENT_SOURCE_DIR}/../planet_lod_lab"
  "${CMAKE_SOURCE_DIR}") # shaders/abi.hpp

# Deliberately do not link helios_vk, helios_render, or their Volk loader.
# Diligent owns the one Vulkan device/context/swap-chain path in this process.
# helios_diligent_render supplies the PNG writer for the offscreen capture route.
target_link_libraries(helios_solar_system PRIVATE
  helios_planet
  helios_diligent_render
  helios_shader_data
  helios_json
  Diligent-GraphicsEngineVk-static
  Diligent-GraphicsTools
  Diligent-Common
  Diligent-Imgui
  SDL3::SDL3-static
  imgui)

add_dependencies(helios_solar_system helios_shaders)
helios_configure_target(helios_solar_system)

set_target_properties(helios_solar_system PROPERTIES
  FOLDER "Helios/Examples"
  VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
