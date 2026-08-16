# SPDX-License-Identifier: AGPL-3.0-only
add_library(helios_math STATIC
  vec.cpp
  mat4.cpp)
target_include_directories(helios_math PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(helios_math PUBLIC helios_core)
helios_configure_target(helios_math)
