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
helios_add_test(helios_scene_assembler_tests test_scene_assembler.cpp LIBS helios_render)
helios_add_test(helios_vk_tests test_vk_core.cpp LIBS helios_vk)
helios_add_test(helios_shader_tests test_shader_abi.cpp LIBS helios_assets helios_math spirv-reflect-static helios_shader_data)
add_dependencies(helios_shader_tests helios_shaders)

# Optional GPU smoke executable: runs the headless smoke suite via the app
# binary (helios --headless-smoke). Only emitted for the "gpu" CTest
# configuration, which no built-in preset defines: the GPU smoke is run
# manually (see evidence/m0/commands.md) and on the self-hosted CI runner.
add_test(NAME helios_headless_smoke
  COMMAND helios --headless-smoke --report-dir ${CMAKE_CURRENT_BINARY_DIR}/headless-smoke-evidence --iterations 1
  CONFIGURATIONS gpu)
set_tests_properties(helios_headless_smoke PROPERTIES LABELS "gpu" TIMEOUT 120)
