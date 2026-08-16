// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helios/universe/module.hpp"

TEST_CASE("universe module skeleton identity") {
    CHECK(helios::universe::kModuleName == "helios_universe");
    CHECK_FALSE(helios::universe::kModuleScope.empty());
}
