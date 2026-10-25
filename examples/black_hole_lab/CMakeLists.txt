# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 the Helios authors and contributors

add_executable(helios_black_hole_lab main.cpp)
target_link_libraries(helios_black_hole_lab PRIVATE helios_blackhole)
helios_configure_target(helios_black_hole_lab)

if(HELIOS_BUILD_TESTS)
  add_test(
    NAME helios_black_hole_lab_quick
    COMMAND helios_black_hole_lab
      --width 48
      --height 27
      --samples 1
      --threads 1
      --no-rgba
      --output-dir "${CMAKE_CURRENT_BINARY_DIR}/quick-test-output")
  set_tests_properties(helios_black_hole_lab_quick PROPERTIES
    LABELS "cpu;blackhole;example"
    TIMEOUT 120)
endif()

# ---------------------------------------------------------------------------
# Real-time Diligent/Vulkan renderer. The CPU reference above stays portable
# and GPU-free; this target shares the SDL/Win32 host of the planet lab for
# its interactive route, so it follows the same platform gate.
# ---------------------------------------------------------------------------
if(NOT HELIOS_ENABLE_DILIGENT)
  return()
endif()
if(NOT WIN32)
  message(STATUS "Skipping helios_black_hole_realtime: Win32 SDL/Diligent bridge only in v0.0.1")
  return()
endif()
if(NOT HELIOS_ENABLE_IMGUI OR NOT TARGET Diligent-Imgui)
  message(STATUS "Skipping helios_black_hole_realtime: HELIOS_ENABLE_IMGUI is required")
  return()
endif()

add_executable(helios_black_hole_realtime
  realtime_main.cpp
  black_hole_renderer.cpp
  ../common/diligent_headless.cpp
  ../common/diligent_sdl_app.cpp
  ../common/fullscreen_pass.cpp
  ../common/hdr_post_chain.cpp)

target_include_directories(helios_black_hole_realtime PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../common"
  "${CMAKE_SOURCE_DIR}") # shaders/abi.hpp

# Deliberately do not link helios_vk or helios_render: Diligent owns the one
# Vulkan device in this process. helios_diligent_render supplies the PNG writer.
target_link_libraries(helios_black_hole_realtime PRIVATE
  helios_blackhole
  helios_diligent_render
  helios_shader_data
  helios_json
  Diligent-GraphicsEngineVk-static
  Diligent-GraphicsTools
  Diligent-Common
  Diligent-Imgui
  SDL3::SDL3-static
  imgui)

add_dependencies(helios_black_hole_realtime helios_shaders)
helios_configure_target(helios_black_hole_realtime)

set_target_properties(helios_black_hole_realtime PROPERTIES
  FOLDER "Helios/Examples"
  VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

if(HELIOS_BUILD_TESTS AND HELIOS_ENABLE_GPU_TESTS)
  # Opt-in physical-GPU checks. Both are windowless: no SDL window or swap
  # chain is created, so they never touch the desktop.
  add_test(
    NAME helios_black_hole_realtime_validate
    COMMAND helios_black_hole_realtime --validate --width 320 --height 180
      --report "${CMAKE_CURRENT_BINARY_DIR}/realtime-validation.json")
  add_test(
    NAME helios_black_hole_realtime_capture
    COMMAND helios_black_hole_realtime
      --capture "${CMAKE_CURRENT_BINARY_DIR}/realtime-capture.png"
      --width 640 --height 360
      --report "${CMAKE_CURRENT_BINARY_DIR}/realtime-capture.json")
  set_tests_properties(
    helios_black_hole_realtime_validate helios_black_hole_realtime_capture PROPERTIES
    LABELS "gpu;diligent;blackhole;headless"
    TIMEOUT 120)
endif()
