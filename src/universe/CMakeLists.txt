# SPDX-License-Identifier: AGPL-3.0-only
# Module skeleton (M0). Frame/time/astronomy systems arrive in M1/M7.
add_library(helios_universe STATIC module.cpp)
target_include_directories(helios_universe PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(helios_universe PUBLIC helios_core helios_math helios_assets)
helios_configure_target(helios_universe)
