#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "common/LayerPixel.h"
#include "extNagano/common.h"
#include "extNagano/3duniversal.h"
#include "extNagano/flutter.h"
#include "extNagano/rgbfade.h"

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

// 以下の画素カーネルは fork の Layer 画素 (RGBA 内存序, common/LayerPixel.h)
// を受け取り、上游 Windows (BGRA) と同じ色の意味で結果を返すこと。
// 入力は TJS 色 0xAARRGGBB を layerPixel::fromArgb で内存序にして与える。

TEST_CASE("extNagano rgbfade applies each opacity to its own channel") {
    using layerPixel::fromArgb;
    // opaR だけ 255: R だけ src2 へ寄り、G/B/A は src1 のまま
    const tjs_uint32 out = extNagano::RGBFadePixel(
        fromArgb(0xFF102030u), fromArgb(0xFFA0B0C0u), 255, 0, 0, 0);
    CHECK(out == fromArgb(0xFF9F2030u));
}

TEST_CASE("extNagano 3duniversal RGB rule packs B/G/R as dir/speed/start") {
    using layerPixel::fromArgb;
    // Windows では生画素 = ARGB 値: byte0=B=方向, byte1=G=スピード, byte2=R=開始
    CHECK(extNagano::DecodeRulePixel(fromArgb(0xFF405060u), false) ==
          0xFF405060u);
}

TEST_CASE("extNagano 3duniversal HSB rule reads hue from real red") {
    using layerPixel::fromArgb;
    // 純赤: hue=0, sat=255, 255-V=0
    CHECK(extNagano::DecodeRulePixel(fromArgb(0xFFFF0000u), true) ==
          0x0000FF00u);
}

TEST_CASE("extNagano flutter back color is written in layer layout") {
    using layerPixel::fromArgb;
    CHECK(extNagano::FlutterBackColor(0x80102030u) == fromArgb(0xFF102030u));
}
