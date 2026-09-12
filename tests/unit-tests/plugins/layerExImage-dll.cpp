#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/LayerExImageOps.h"
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

TEST_CASE("layerExImageOps light applies the reference brightness table") {
    constexpr int width = 4;
    constexpr int height = 2;
    constexpr int pitch = width * 4;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(pitch) * height,
                                      0);
    // Fill B=0, G=64, R=192, A=255 rows.
    for(int y = 0; y < height; ++y) {
        for(int x = 0; x < width; ++x) {
            auto *pixel = pixels.data() + y * pitch + x * 4;
            pixel[0] = 0;
            pixel[1] = 64;
            pixel[2] = 192;
            pixel[3] = 255;
        }
    }

    const layerExImageOps::ImageMutableView view{
        pixels.data(), width, height, pitch};
    // brightness=0, contrast=0 is the identity table: the reference offsets
    // by brightness + 128 around the 128 midpoint, cancelling at zero.
    layerExImageOps::ApplyLight(view, 0, 0);

    for(int y = 0; y < height; ++y) {
        for(int x = 0; x < width; ++x) {
            const auto *pixel = pixels.data() + y * pitch + x * 4;
            CHECK(pixel[0] == 0);
            CHECK(pixel[1] == 64);
            CHECK(pixel[2] == 192);
            CHECK(pixel[3] == 255);
        }
    }

    // brightness=128 shifts every channel up by 128 with saturation at 255;
    // alpha is never part of the table.
    layerExImageOps::ApplyLight(view, 128, 0);
    for(int y = 0; y < height; ++y) {
        for(int x = 0; x < width; ++x) {
            const auto *pixel = pixels.data() + y * pitch + x * 4;
            CHECK(pixel[0] == 128);
            CHECK(pixel[1] == 192);
            CHECK(pixel[2] == 255);
            CHECK(pixel[3] == 255);
        }
    }
}

TEST_CASE("layerExImageOps gaussianBlur failure leaves pixels unchanged") {
    // The op-level failure path is the byte-count check; radius validation
    // lives in the registration surfaces (ValidateGaussianBlurRadius /
    // ValidateBlurRadius), so an invalid view is what reaches this layer.
    constexpr int width = 0;
    constexpr int height = 2;
    constexpr int pitch = 16;
    std::vector<unsigned char> pixels(32, 0);
    for(std::size_t i = 0; i < pixels.size(); ++i)
        pixels[i] = static_cast<unsigned char>(i * 37 + 5);

    const std::vector<unsigned char> before = pixels;
    const layerExImageOps::ImageMutableView view{
        pixels.data(), width, height, pitch};
    CHECK_FALSE(layerExImageOps::ApplyGaussianBlur(view, 1.0f));
    CHECK(pixels == before);
}

TEST_CASE("layerExImageOps colorize full blend replaces hue and saturation") {
    constexpr int width = 2;
    constexpr int height = 1;
    constexpr int pitch = width * 4;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(pitch) * height,
                                      0);
    pixels[0] = 10;  // B
    pixels[1] = 20;  // G
    pixels[2] = 30;  // R
    pixels[3] = 200; // A
    pixels[4] = 40;
    pixels[5] = 50;
    pixels[6] = 60;
    pixels[7] = 255;

    const layerExImageOps::ImageMutableView view{
        pixels.data(), width, height, pitch};
    layerExImageOps::ApplyColorize(view, 0, 0, 1.0);

    for(int x = 0; x < width; ++x) {
        const auto *pixel = pixels.data() + x * 4;
        // hue=0, saturation=0 yields a gray whose lightness matches the
        // source's HSL lightness; alpha must stay untouched.
        CHECK(pixel[0] == pixel[1]);
        CHECK(pixel[1] == pixel[2]);
        CHECK(pixel[3] == (x == 0 ? 200 : 255));
    }
}
