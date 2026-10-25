# SPDX-License-Identifier: AGPL-3.0-only
# helios_render: scene assembly boundary, render resources/passes, telemetry.
add_library(helios_render STATIC
  renderer.cpp
  frame.cpp
  reports.cpp
  debug_ui.cpp
  headless.cpp
  scene_assembler.cpp
  device_probe.cpp
  gpu_precision.cpp
  gpu_precision_plan.cpp
  png_writer.cpp)
target_include_directories(helios_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(helios_render PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(helios_render PUBLIC helios_core helios_math helios_assets helios_universe helios_planet)
target_link_libraries(helios_render PRIVATE helios_vk SDL3::SDL3)
if(HELIOS_ENABLE_IMGUI)
  target_link_libraries(helios_render PRIVATE imgui)
  target_compile_definitions(helios_render PRIVATE HELIOS_ENABLE_IMGUI=1)
endif()
helios_configure_target(helios_render)

if(HELIOS_ENABLE_DILIGENT)
  # Kept separate from helios_render during parity migration: the legacy
  # renderer owns a Volk implementation and must never be linked into the
  # same executable as Diligent's Vulkan implementation.
  add_library(helios_diligent_render STATIC
    diligent_runtime.cpp
    png_writer.cpp)
  target_include_directories(helios_diligent_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(helios_diligent_render
    PUBLIC helios_core
    PRIVATE Diligent-GraphicsEngineVk-static Diligent-Common)
  helios_configure_target(helios_diligent_render)
endif()
