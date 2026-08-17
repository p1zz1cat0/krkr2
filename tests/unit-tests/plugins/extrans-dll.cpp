#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "Extrans/ExtransMath.h"

TEST_CASE("extrans wave locks official integer amplitude semantics") {
    REQUIRE(extrans::ValidateWaveParameters(65, 50, 0.2));
    CHECK_FALSE(extrans::ValidateWaveParameters(65, 50,
        std::numeric_limits<double>::infinity()));
    CHECK_FALSE(extrans::ValidateWaveParameters(
        65, std::numeric_limits<std::int32_t>::min(), 0.2));

    const auto frame = extrans::ComputeWaveFrame(11, 1000, 200, 50, 0.2, 0);
    CHECK(frame.height == 1);
    CHECK(frame.blendRatio == 2);
    const auto officialShift = static_cast<std::int32_t>(
        std::sin((1 - 100) * frame.omega) * frame.height);
    CHECK(officialShift == 0);

    CHECK(extrans::ComputeWaveFrame(250, 1000, 200, 50, 0.2, 1).omega ==
          Catch::Approx(0.15));
    CHECK(extrans::ComputeWaveFrame(250, 1000, 200, 50, 0.2, 2).omega ==
          Catch::Approx(0.05));
}

TEST_CASE("extrans mosaic uses official global grid and truncation") {
    REQUIRE(extrans::ValidateMosaicParameters(137, 91, 30));
    CHECK_FALSE(extrans::ValidateMosaicParameters(137, 91, 1));
    CHECK_FALSE(extrans::ValidateMosaicParameters(137, 91,
                                                   1LL << 40));

    const auto frame = extrans::ComputeMosaicFrame(100, 1000, 137, 91, 30);
    CHECK(frame.blockSize == 7);
    CHECK(frame.offsetX == -5);
    CHECK(frame.offsetY == 0);
    CHECK(frame.blendRatio == 25);

    std::array<std::int32_t, 137 * 91> whole{};
    std::array<std::int32_t, 137 * 91> chunked{};
    for(std::int32_t y = 0; y < 91; ++y) {
        for(std::int32_t x = 0; x < 137; ++x) {
            whole[y * 137 + x] =
                extrans::ComputeMosaicSampleCoordinate(
                    x, 137, frame.blockSize, frame.offsetX) +
                137 * extrans::ComputeMosaicSampleCoordinate(
                          y, 91, frame.blockSize, frame.offsetY);
        }
    }
    for(const auto &rect : std::array<std::array<int, 4>, 5>{
            std::array<int, 4>{0, 0, 37, 29},
            std::array<int, 4>{37, 0, 100, 29},
            std::array<int, 4>{0, 29, 3, 62},
            std::array<int, 4>{3, 29, 134, 17},
            std::array<int, 4>{3, 46, 134, 45},
        }) {
        for(int y = rect[1]; y < rect[1] + rect[3]; ++y) {
            for(int x = rect[0]; x < rect[0] + rect[2]; ++x) {
                chunked[y * 137 + x] =
                    extrans::ComputeMosaicSampleCoordinate(
                        x, 137, frame.blockSize, frame.offsetX) +
                    137 * extrans::ComputeMosaicSampleCoordinate(
                              y, 91, frame.blockSize, frame.offsetY);
            }
        }
    }
    CHECK(chunked == whole);
    CHECK(extrans::ComputeMosaicSampleCoordinate(
              0, 137, frame.blockSize, frame.offsetX) == 0);
    CHECK(extrans::ComputeMosaicSampleCoordinate(
              136, 137, frame.blockSize, frame.offsetX) == 136);

    // Official Blend() uses a signed arithmetic shift by eight, including
    // negative channel deltas. Lock representative opaque and alpha pixels.
    CHECK(extrans::BlendARGB256(0x80102030u, 0x40f00080u, 25) ==
          0x79251c37u);
}

TEST_CASE("extrans rotate coordinates reject non-finite and truncate to zero") {
    CHECK(extrans::IsFiniteRotateParameter(-2.0));
    CHECK_FALSE(extrans::IsFiniteRotateParameter(
        std::numeric_limits<double>::quiet_NaN()));
    CHECK_FALSE(extrans::IsFiniteRotateParameter(
        std::numeric_limits<double>::infinity()));
    CHECK(extrans::TruncateRotateCoordinate(3.9) == 3);
    CHECK(extrans::TruncateRotateCoordinate(-3.9) == -3);
    CHECK_THROWS(extrans::TruncateRotateCoordinate(
        std::numeric_limits<double>::infinity()));
}

TEST_CASE("extrans turn table preserves official fixed point semantics") {
    const auto &table = extrans::GetTurnTable();
    const auto &gloss = extrans::GetTurnGloss();

    REQUIRE(gloss[4] == 16);
    REQUIRE(gloss[8] == 192);
    REQUIRE(gloss[13] == 0);
    std::uint64_t officialHash = 1469598103934665603ULL;
    for(int phase = 0; phase < 64; ++phase) {
        for(int line = 0; line < 64; ++line) {
            const auto &entry = table[phase][line];
            const std::int32_t fields[] = {entry.start, entry.length,
                                           entry.sourceX, entry.sourceY,
                                           entry.stepX, entry.stepY};
            for(std::int32_t field : fields) {
                const std::uint32_t raw = static_cast<std::uint32_t>(field);
                for(int byte = 0; byte < 4; ++byte) {
                    officialHash ^= (raw >> (byte * 8)) & 0xffu;
                    officialHash *= 1099511628211ULL;
                }
            }
            if(phase == 0 || phase == 63)
                continue;
            CHECK(entry.start >= 0);
            CHECK(entry.start < 64);
            CHECK(entry.length >= 1);
            CHECK(entry.start + entry.length <= 64);
            CHECK(extrans::UnpackSigned24(
                      extrans::PackSigned24(entry.sourceX)) == entry.sourceX);
            CHECK(extrans::UnpackSigned24(
                      extrans::PackSigned24(entry.sourceY)) == entry.sourceY);
            CHECK(extrans::UnpackSigned24(
                      extrans::PackSigned24(entry.stepX)) == entry.stepX);
            CHECK(extrans::UnpackSigned24(
                      extrans::PackSigned24(entry.stepY)) == entry.stepY);
        }
    }
    // FNV-1a over all six exported fields for all 4096 official table rows.
    CHECK(officialHash == 0xed328a893801ff46ULL);

    // Fixed golden samples generated independently from the official Perl
    // formulas. These lock truncation toward zero and the 65536 scale.
    CHECK(table[1][0] == extrans::TurnParameters{0, 64, 0, 0, 65536, 0});
    CHECK(table[31][31] ==
          extrans::TurnParameters{30, 2, 196608, 4128768, 3538944, -4128768});
    CHECK(table[32][31] ==
          extrans::TurnParameters{30, 2, 0, 3604480, 4128768, -3276800});
    CHECK(table[62][63] ==
          extrans::TurnParameters{0, 64, 0, 4128768, 65536, 0});
}

TEST_CASE("extrans turn phase uses logical tile coordinates") {
    CHECK(extrans::ComputeTurnGlobalPhase(0, 1000, 128, 128) == -4);
    CHECK(extrans::ComputeTurnGlobalPhase(1000, 1000, 128, 128) == 68);
    CHECK(extrans::ComputeTurnTilePhase(31, 2, 1) == 29);
    CHECK(extrans::ComputeTurnTilePhase(31, 1, 2) == 33);
    CHECK(extrans::ComputeTurnTilePhase(-10, 0, 0) == 0);
    CHECK(extrans::ComputeTurnTilePhase(80, 0, 0) == 63);
}

TEST_CASE("extrans turn mapping is independent of divisible update chunks") {
    constexpr int width = 137;
    constexpr int height = 91;
    constexpr int globalPhase = 31;
    std::vector<extrans::TurnPixelMapping> whole;
    std::vector<extrans::TurnPixelMapping> chunked(width * height);
    whole.reserve(width * height);
    for(int y = 0; y < height; ++y)
        for(int x = 0; x < width; ++x)
            whole.push_back(extrans::MapTurnPixel(globalPhase, x, y));

    // Deliberately split through tiles on all four sides and include narrow
    // strips. Logical coordinates, not chunk-local coordinates, drive phase.
    for(const auto &rect : std::array<std::array<int, 4>, 5>{
            std::array<int, 4>{0, 0, 37, 29},
            std::array<int, 4>{37, 0, 100, 29},
            std::array<int, 4>{0, 29, 3, 62},
            std::array<int, 4>{3, 29, 134, 17},
            std::array<int, 4>{3, 46, 134, 45},
        }) {
        for(int y = rect[1]; y < rect[1] + rect[3]; ++y)
            for(int x = rect[0]; x < rect[0] + rect[2]; ++x)
                chunked[y * width + x] =
                    extrans::MapTurnPixel(globalPhase, x, y);
    }
    for(std::size_t index = 0; index < whole.size(); ++index) {
        CHECK(chunked[index].background == whole[index].background);
        CHECK(chunked[index].source2 == whole[index].source2);
        CHECK(chunked[index].sourceX == whole[index].sourceX);
        CHECK(chunked[index].sourceY == whole[index].sourceY);
        CHECK(chunked[index].gloss == whole[index].gloss);
    }

    CHECK_FALSE(extrans::MapTurnPixel(0, 71, 19).source2);
    CHECK(extrans::MapTurnPixel(63, 71, 19).source2);
    CHECK(extrans::MapTurnPixel(31, 0, 64).source2);
    CHECK_FALSE(extrans::MapTurnPixel(31, 64, 0).source2);
}

TEST_CASE("extrans alpha-safe blending and turn gloss preserve alpha") {
    const std::uint32_t translucent = 0x40102030u;
    CHECK((extrans::ApplyTurnGloss(translucent, 192) & 0xff000000u) ==
          0x40000000u);
    CHECK(extrans::ApplyTurnGloss(translucent, 0) == translucent);
    CHECK(extrans::BlendARGB256(0x00112233u, 0xffaabbccu, 0) ==
          0x00112233u);
    CHECK(extrans::BlendARGB256(0x00112233u, 0xffaabbccu, 256) ==
          0xffaabbccu);
    const auto half = extrans::BlendARGB256(0x00102030u, 0x80406080u, 128);
    CHECK((half >> 24) == 0x40u);
}

TEST_CASE("extrans ripple validates official options and hardening") {
    extrans::RippleParameters parameters{64, 64, 32, 32, 16, 1.0f, 6.0f, 8};
    REQUIRE(extrans::ValidateRippleParameters(parameters));
    for(int width : {16, 32, 64, 128}) {
        parameters.rippleWidth = width;
        CHECK(extrans::ValidateRippleParameters(parameters));
    }
    parameters.rippleWidth = 15;
    CHECK_FALSE(extrans::ValidateRippleParameters(parameters));
    parameters.rippleWidth = 16;
    parameters.roundness = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(extrans::ValidateRippleParameters(parameters));
    parameters.roundness = 1.0f;
    parameters.speed = std::numeric_limits<float>::infinity();
    CHECK_FALSE(extrans::ValidateRippleParameters(parameters));
    parameters.speed = 6.0f;
    parameters.maxDrift = 64;
    CHECK_FALSE(extrans::ValidateRippleParameters(parameters));
}

TEST_CASE("extrans ripple preserves official negative speed phase") {
    extrans::RippleParameters parameters{64, 64, 32, 32, 16, 1.0f, -6.0f, 8};
    const auto negative = extrans::ComputeRippleFrame(500, 1000, parameters);
    CHECK(negative.phase == 15);
    parameters.speed = 6.0f;
    const auto positive = extrans::ComputeRippleFrame(500, 1000, parameters);
    CHECK(positive.phase != negative.phase);

    parameters.maxDrift = 0;
    const auto zeroDrift = extrans::ComputeRippleFrame(500, 1000, parameters);
    CHECK(zeroDrift.drift == 0);
    const auto tables = extrans::GenerateRippleTables(parameters);
    CHECK(tables.drift.empty());
}

TEST_CASE("extrans ripple table encodes official direction and distance") {
    const extrans::RippleParameters parameters{
        8, 8, 4, 4, 16, 1.0f, 6.0f, 2};
    const auto tables = extrans::GenerateRippleTables(parameters);
    REQUIRE(tables.mapWidth == 4);
    REQUIRE(tables.mapHeight == 4);
    REQUIRE(tables.displacement.size() == 16);
    REQUIRE(tables.drift.size() ==
            2 * extrans::kRippleDriftPrecision * 16 *
                extrans::kRippleDirectionPrecision);
    CHECK(tables.displacement.front() == 16);
    CHECK(tables.displacement.back() == 144);
}
