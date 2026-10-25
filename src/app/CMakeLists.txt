# SPDX-License-Identifier: AGPL-3.0-only
# helios_app has NO helios_vk link edge (docs/ARCHITECTURE.md).
add_executable(helios main.cpp app.cpp cli.cpp headless.cpp precision_lab.cpp precision_lab_gpu.cpp)
target_include_directories(helios PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(helios PRIVATE helios_render helios_planet helios_universe helios_assets helios_math helios_core)
target_link_libraries(helios PRIVATE SDL3::SDL3)
if(HELIOS_ENABLE_IMGUI)
  target_link_libraries(helios PRIVATE imgui)
endif()
helios_configure_target(helios)
add_dependencies(helios helios_shaders)
if(WIN32)
  target_link_options(helios PRIVATE /SUBSYSTEM:CONSOLE)
endif()
