# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 the Helios authors and contributors

add_executable(helios_terrain_lab main.cpp)
target_link_libraries(helios_terrain_lab PRIVATE helios_planet)
helios_configure_target(helios_terrain_lab)

if(HELIOS_BUILD_TESTS)
  add_test(
    NAME helios_terrain_lab_quick
    COMMAND helios_terrain_lab
      --face +Z
      --level 3
      --x 3
      --y 4
      --seed 0x48454c494f535631
      --amplitude 1500
      --root-octaves 3
      --octaves 7
      --scale 1
      --output-dir "${CMAKE_CURRENT_BINARY_DIR}/quick-test-output")
  set_tests_properties(helios_terrain_lab_quick PROPERTIES
    LABELS "cpu;planet;terrain;example"
    PASS_REGULAR_EXPRESSION "validation=PASS"
    TIMEOUT 30)
endif()
