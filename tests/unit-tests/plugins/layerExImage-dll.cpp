#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "packinone/layerExImage.h"

TEST_CASE("PackinOne layerExImage gaussianBlur rejects abnormal floats") {
    using packinone::layerExImage::ValidateGaussianBlurRadius;

    const std::vector<double> invalid = {
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        std::nextafter(64.0, std::numeric_limits<double>::infinity()),
        std::nextafter(-64.0, -std::numeric_limits<double>::infinity()),
        static_cast<double>(std::numeric_limits<float>::max()),
        std::numeric_limits<double>::max(),
    };
    for(const double value : invalid) {
        float normalized = 123.0f;
        CHECK_FALSE(ValidateGaussianBlurRadius(value, normalized));
        CHECK(normalized == 123.0f);
    }

    const std::vector<double> valid = {0.0, -0.0, 0.25, -0.25, 64.0};
    for(const double value : valid) {
        float normalized = -1.0f;
        REQUIRE(ValidateGaussianBlurRadius(value, normalized));
        CHECK(std::isfinite(normalized));
        CHECK(normalized >= 0.0f);
        CHECK(normalized <= 64.0f);
        CHECK(std::fabs(normalized - std::fabs(value)) < 0.0001f);
    }
}

TEST_CASE("PackinOne generateWhiteNoise preserves alpha and padding") {
    constexpr int width = 7;
    constexpr int height = 5;
    constexpr std::ptrdiff_t pitch = width * 4 + 3;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(pitch) * height,
                                      0xA5);
    std::vector<unsigned char> alpha(static_cast<std::size_t>(width) * height);

    for(int y = 0; y < height; ++y) {
        auto *row = pixels.data() + y * pitch;
        for(int x = 0; x < width; ++x) {
            auto *pixel = row + x * 4;
            pixel[0] = static_cast<unsigned char>(x * 11 + y);
            pixel[1] = static_cast<unsigned char>(x * 7 + y * 3);
            pixel[2] = static_cast<unsigned char>(x * 5 + y * 9);
            pixel[3] = static_cast<unsigned char>(0x20 + x + y * width);
            alpha[static_cast<std::size_t>(y) * width + x] = pixel[3];
        }
    }

    packinone::layerExImage::GenerateWhiteNoise(pixels.data(), width, height,
                                                   pitch);

    for(int y = 0; y < height; ++y) {
        const auto *row = pixels.data() + y * pitch;
        for(int x = 0; x < width; ++x) {
            const auto *pixel = row + x * 4;
            CHECK(pixel[0] == pixel[1]);
            CHECK(pixel[1] == pixel[2]);
            CHECK(pixel[3] == alpha[static_cast<std::size_t>(y) * width + x]);
        }
        CHECK(std::all_of(row + width * 4, row + pitch,
                          [](unsigned char value) { return value == 0xA5; }));
    }

    // The random output intentionally has no fixed-pixel or fixed-sequence
    // expectation. A second invocation only proves repeatability of safety.
    packinone::layerExImage::GenerateWhiteNoise(pixels.data(), width, height,
                                                   pitch);
    for(int y = 0; y < height; ++y) {
        const auto *row = pixels.data() + y * pitch;
        for(int x = 0; x < width; ++x) {
            const auto *pixel = row + x * 4;
            CHECK(pixel[0] == pixel[1]);
            CHECK(pixel[1] == pixel[2]);
            CHECK(pixel[3] == alpha[static_cast<std::size_t>(y) * width + x]);
        }
    }
}
