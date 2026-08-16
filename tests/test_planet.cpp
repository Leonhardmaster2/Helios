// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helios/planet/module.hpp"

TEST_CASE("planet module skeleton identity") {
    CHECK(helios::planet::kModuleName == "helios_planet");
    CHECK_FALSE(helios::planet::kModuleScope.empty());
}
