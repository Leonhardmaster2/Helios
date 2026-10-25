# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 the Helios authors and contributors

# The interactive route shares the SDL/Win32 host of the other Diligent labs,
# so this target follows the same platform gate.
if(NOT WIN32)
  message(STATUS "Skipping helios_deep_sky_lab: Win32 SDL/Diligent bridge only in v0.0.1")
  return()
endif()
if(NOT HELIOS_ENABLE_IMGUI OR NOT TARGET Diligent-Imgui)
  message(STATUS "Skipping helios_deep_sky_lab: HELIOS_ENABLE_IMGUI is required")
  return()
endif()

# CPU-only probe of the galaxy simulation (no GPU, no window).
add_executable(helios_galaxy_probe
  galaxy_probe.cpp
  galaxy_simulation.cpp
  stellar_population.cpp
  stellar_systems.cpp
  fft.cpp
  worker_pool.cpp)
target_include_directories(helios_galaxy_probe PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_SOURCE_DIR}") # shaders/abi.hpp
target_link_libraries(helios_galaxy_probe PRIVATE helios_blackhole helios_diligent_render)
helios_configure_target(helios_galaxy_probe)
set_target_properties(helios_galaxy_probe PROPERTIES FOLDER "Helios/Examples")

add_executable(helios_deep_sky_lab
  main.cpp
  deep_sky_renderer.cpp
  galaxy_explorer.cpp
  galaxy_scene.cpp
  gas_scene.cpp
  gas_validation.cpp
  hydro_simulation.cpp
  neutron_star_model.cpp
  object_variants.cpp
  planet_stage.cpp
  ../black_hole_lab/black_hole_renderer.cpp
  ../planet_lod_lab/planet_patch_renderer.cpp
  ../planet_lod_lab/planet_surface.cpp
  ../planet_lod_lab/planet_system.cpp
  galaxy_simulation.cpp
  stellar_population.cpp
  stellar_systems.cpp
  fft.cpp
  worker_pool.cpp
  ../common/diligent_headless.cpp
  ../common/diligent_sdl_app.cpp
  ../common/fullscreen_pass.cpp
  ../common/hdr_post_chain.cpp)

target_include_directories(helios_deep_sky_lab PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../common"
  "${CMAKE_CURRENT_SOURCE_DIR}/../black_hole_lab"
  "${CMAKE_CURRENT_SOURCE_DIR}/../planet_lod_lab"
  "${CMAKE_SOURCE_DIR}") # shaders/abi.hpp

# Deliberately do not link helios_vk or helios_render: Diligent owns the one
# Vulkan device in this process. helios_blackhole supplies the black-body and
# emission-line colourimetry; helios_diligent_render the PNG writer.
target_link_libraries(helios_deep_sky_lab PRIVATE
  helios_blackhole
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

add_dependencies(helios_deep_sky_lab helios_shaders)
helios_configure_target(helios_deep_sky_lab)

set_target_properties(helios_deep_sky_lab PROPERTIES
  FOLDER "Helios/Examples"
  VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

if(HELIOS_BUILD_TESTS AND HELIOS_ENABLE_GPU_TESTS)
  # Opt-in physical-GPU checks, all windowless: the gas solver against exact
  # solutions, and one small capture per object (short simulations).
  add_test(
    NAME helios_deep_sky_lab_validate_gas
    COMMAND helios_deep_sky_lab --validate-gas --volume 128
      --report "${CMAKE_CURRENT_BINARY_DIR}/gas-validation.json")
  set_tests_properties(helios_deep_sky_lab_validate_gas PROPERTIES
    LABELS "gpu;diligent;deepsky;headless"
    TIMEOUT 120)
  set(helios_deep_sky_nebula_args --volume 96 --sim-time 1.1)
  set(helios_deep_sky_neutron-star_args)
  set(helios_deep_sky_galaxy_args --sim-time 20 --galaxy-stars 200000 --galaxy-gas 100000
      --galaxy-bulge 30000 --galaxy-mesh 256)
  set(helios_deep_sky_remnant_args --volume 96 --sim-time 150)
  set(helios_deep_sky_black-hole_args --preset performance)
  set(helios_deep_sky_planet_args --descent 18)
  set(helios_deep_sky_pulsar_args --variant millisecond-pulsar)
  foreach(object nebula neutron-star galaxy remnant black-hole planet pulsar)
    add_test(
      NAME helios_deep_sky_lab_${object}
      COMMAND helios_deep_sky_lab
        --object ${object} ${helios_deep_sky_${object}_args}
        --capture "${CMAKE_CURRENT_BINARY_DIR}/capture-${object}.png"
        --width 480 --height 270 --accumulate 4
        --report "${CMAKE_CURRENT_BINARY_DIR}/capture-${object}.json")
    set_tests_properties(helios_deep_sky_lab_${object} PROPERTIES
      LABELS "gpu;diligent;deepsky;headless"
      TIMEOUT 120)
  endforeach()
endif()
