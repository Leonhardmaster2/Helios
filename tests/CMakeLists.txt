# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 the Helios authors and contributors
#
# Unit/property tests (doctest) + shader reflection/ABI tests.

function(helios_add_test name)
  cmake_parse_arguments(ARG "" "" "LIBS" ${ARGN})
  add_executable(${name} ${ARG_UNPARSED_ARGUMENTS})
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
  target_link_libraries(${name} PRIVATE helios_doctest ${ARG_LIBS})
  helios_configure_target(${name})
  add_test(NAME ${name} COMMAND ${name} --no-skip --exit)
endfunction()

helios_add_test(helios_core_tests test_core.cpp LIBS helios_core)
helios_add_test(helios_math_tests test_math.cpp LIBS helios_math)
helios_add_test(helios_assets_tests test_assets.cpp LIBS helios_assets)
helios_add_test(helios_universe_tests test_universe.cpp LIBS helios_universe)
helios_add_test(helios_planet_tests test_planet.cpp LIBS helios_planet)
helios_add_test(helios_planet_core_tests test_planet_core.cpp LIBS helios_planet)
helios_add_test(helios_planet_terrain_tests test_planet_terrain.cpp LIBS helios_planet)
helios_add_test(helios_planet_residency_tests test_planet_residency.cpp LIBS helios_planet)
helios_add_test(helios_planet_system_tests test_planet_system.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../examples/planet_lod_lab/planet_system.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../examples/planet_lod_lab/planet_surface.cpp
  LIBS helios_math)
target_include_directories(helios_planet_system_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../examples/planet_lod_lab)
helios_add_test(helios_stellar_systems_tests test_stellar_systems.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../examples/deep_sky_lab/stellar_systems.cpp
  LIBS helios_math)
target_include_directories(helios_stellar_systems_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../examples/deep_sky_lab)
helios_add_test(helios_blackhole_tests test_blackhole.cpp LIBS helios_blackhole)
helios_add_test(helios_scene_assembler_tests test_scene_assembler.cpp LIBS helios_render)
helios_add_test(helios_vk_tests test_vk_core.cpp LIBS helios_vk)
helios_add_test(helios_shader_tests test_shader_abi.cpp LIBS helios_assets helios_math spirv-reflect-static helios_shader_data)
add_dependencies(helios_shader_tests helios_shaders)

if(HELIOS_ENABLE_GPU_TESTS)
  # These checks create a physical Vulkan device but no window. They are not
  # registered in an ordinary developer build, preventing an accidental GPU
  # run from plain `ctest`. Opt in at configure time, then use `ctest -L gpu`.
  add_test(NAME helios_headless_smoke
    COMMAND helios --headless-smoke --report-dir ${CMAKE_CURRENT_BINARY_DIR}/headless-smoke-evidence --iterations 1)
  set_tests_properties(helios_headless_smoke PROPERTIES LABELS "gpu" TIMEOUT 120)

  if(HELIOS_ENABLE_DILIGENT)
    add_test(NAME helios_diligent_probe COMMAND helios_diligent_probe)
    set_tests_properties(helios_diligent_probe PROPERTIES
      LABELS "gpu;diligent"
      TIMEOUT 120)
  endif()
endif()
