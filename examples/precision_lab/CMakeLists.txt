# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 the Helios authors and contributors

# This lab intentionally owns a Diligent/Vulkan device directly. It must not
# link helios_render or helios_vk because both contain the legacy Volk path.
# The compute module comes from Helios's canonical offline Slang manifest and
# is embedded in the executable through helios_shader_data.
add_executable(helios_diligent_precision_lab main.cpp)
target_compile_definitions(helios_diligent_precision_lab PRIVATE
  "HELIOS_VERSION_STRING=\"${PROJECT_VERSION}\"")
target_link_libraries(helios_diligent_precision_lab PRIVATE
  Diligent-GraphicsEngineVk-static
  Diligent-Common
  helios_shader_data
  helios_json)
helios_configure_target(helios_diligent_precision_lab)

if(HELIOS_BUILD_TESTS AND HELIOS_ENABLE_GPU_TESTS)
  # Opt-in physical-GPU check: one eight-thread dispatch and one 128-byte
  # readback, no window, with a hard timeout.
  add_test(
    NAME helios_diligent_precision_lab
    COMMAND helios_diligent_precision_lab --ctest)
  set_tests_properties(helios_diligent_precision_lab PROPERTIES
    LABELS "gpu;diligent;precision;headless"
    TIMEOUT 30)
endif()
