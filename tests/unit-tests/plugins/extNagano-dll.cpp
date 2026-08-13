#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "extNagano/common.h"

TEST_CASE("extNagano rejects unbounded image budgets before allocation") {
    REQUIRE(extNagano::CheckImageSize(1, 1));
    REQUIRE(extNagano::CheckImageSize(32, 32));
    REQUIRE(extNagano::CheckImageSize(1920, 1080));
    REQUIRE(extNagano::CheckImageSize(4096, 4096));

    CHECK_FALSE(extNagano::CheckImageSize(0, 32));
    CHECK_FALSE(extNagano::CheckImageSize(32, 0));
    CHECK_FALSE(extNagano::CheckImageSize(-1, 32));
    CHECK_FALSE(extNagano::CheckImageSize(32, -8));
    CHECK_FALSE(extNagano::CheckImageSize(4097, 16));
    CHECK_FALSE(extNagano::CheckImageSize(16, 4097));
    CHECK_FALSE(extNagano::CheckImageSize(
        static_cast<tjs_uint>(std::numeric_limits<tjs_int>::max()), 2u));
}

TEST_CASE("extNagano morphing array budget stays at the official triangle cap") {
    CHECK(extNagano::kMaxMorphElements == 256 * 6);
}
