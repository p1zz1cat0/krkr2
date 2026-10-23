#include <catch2/catch_test_macros.hpp>
#include "VorbisWaveDecoder.h"

#include <vector>

// Multitrack Opus (mapping family 255) is mixed to stereo by track pairs.
TEST_CASE("multitrack mix sums even channels left and odd channels right", "[sound][opus]") {
    // 2 frames x 4 channels: tracks (0,1) and (2,3).
    const std::vector<std::int16_t> src = {100, -200, 30, 40, 1, 2, 3, 4};
    std::vector<std::int16_t> dst(4, 7);
    TVPMixTrackPairsToStereo(src.data(), 2, 4, dst.data());
    REQUIRE(dst == std::vector<std::int16_t>{130, -160, 4, 6});
}

TEST_CASE("multitrack mix sends a last single channel to both sides", "[sound][opus]") {
    const std::vector<std::int16_t> src = {10, 20, 5};
    std::vector<std::int16_t> dst(2);
    TVPMixTrackPairsToStereo(src.data(), 1, 3, dst.data());
    REQUIRE(dst == std::vector<std::int16_t>{15, 25});
}

TEST_CASE("multitrack mix clips sums to 16 bits", "[sound][opus]") {
    const std::vector<std::int16_t> src = {30000, -30000, 30000, -30000};
    std::vector<std::int16_t> dst(2);
    TVPMixTrackPairsToStereo(src.data(), 1, 4, dst.data());
    REQUIRE(dst == std::vector<std::int16_t>{32767, -32768});
}
