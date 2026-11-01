//
// shrinkCopy 算法回归测试：直接包含插件源文件，验证整数平均缩小的核心逻辑。
// 不依赖 Layer/TJS 运行时，用固定 BGRA 缓冲做确定性断言。
//

#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "shrinkCopy/shrinkCopy.cpp"

TEST_CASE("shrinkCopy ShrinkLine averages pixels") {
    // 4 个 BGRA 像素：黑 / 白 / (10,20,30) / (90,80,70)，alpha 分别为 0/255/255/255
    const unsigned char src[16] = {
        0,   0,   0,   0,
        255, 255, 255, 255,
        10,  20,  30,  255,
        90,  80,  70,  255,
    };
    unsigned char dst[8] = {0, 0, 0, 0, 0, 0, 0, 0};

    LimitedShrink::WrtRefT w = dst;
    LimitedShrink::BufRefT r = src;
    LimitedShrink::ShrinkLine(w, r, 2, 2, 8, 4);

    CHECK(dst[0] == 127); // (0+255)/2
    CHECK(dst[1] == 127);
    CHECK(dst[2] == 127);
    CHECK(dst[3] == 255);
    CHECK(dst[4] == 50);  // (10+90)/2
    CHECK(dst[5] == 50);  // (20+80)/2
    CHECK(dst[6] == 50);  // (30+70)/2
    CHECK(dst[7] == 255);
}

TEST_CASE("shrinkCopy ShrinkLine pass-through forces opaque alpha") {
    const unsigned char src[8] = {7, 8, 9, 1, 11, 12, 13, 2};
    unsigned char dst[8] = {0, 0, 0, 0, 0, 0, 0, 0};

    LimitedShrink::WrtRefT w = dst;
    LimitedShrink::BufRefT r = src;
    LimitedShrink::ShrinkLine(w, r, 2, 1, 4, 4);

    CHECK(dst[0] == 7);
    CHECK(dst[1] == 8);
    CHECK(dst[2] == 9);
    CHECK(dst[3] == 255);
    CHECK(dst[4] == 11);
    CHECK(dst[5] == 12);
    CHECK(dst[6] == 13);
    CHECK(dst[7] == 255);
}

namespace {
struct BufferShrink : ShrinkCopy {
    BufferShrink(const unsigned char *source, long swidth, long sheight,
                 unsigned char *target, long dwidth, long dheight)
        : ShrinkCopy(nullptr, 0, 0, dwidth, dheight,
                     nullptr, 0, 0, swidth, sheight) {
        ps = source;
        siw = swidth;
        sih = sheight;
        spch = swidth * 4;
        pd = target;
        diw = dwidth;
        dih = dheight;
        dpch = dwidth * 4;
    }
};
}

TEST_CASE("shrinkCopy area average preserves pixel channels") {
    std::vector<unsigned char> src(4 * 4 * 4);
    for (int i = 0; i < 16; ++i) {
        src[i * 4] = i * 4;
        src[i * 4 + 1] = 80 + i * 4;
        src[i * 4 + 2] = 160 + i * 4;
        src[i * 4 + 3] = 128 + i * 4;
    }
    unsigned char dst[2 * 2 * 4] = {};
    BufferShrink task(src.data(), 4, 4, dst, 2, 2);
    REQUIRE(task.clip());
    task.copy();
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            for (int c = 0; c < 4; ++c) {
                const int p = ((y * 2) * 4 + x * 2) * 4 + c;
                const int expected = (src[p] + src[p + 4] +
                                      src[p + 16] + src[p + 20]) / 4;
                CHECK(dst[(y * 2 + x) * 4 + c] == expected);
            }
        }
    }
}

TEST_CASE("shrinkCopy save thumbnail preserves opaque alpha") {
    constexpr int sw = 496, sh = 279, dw = 224, dh = 126;
    std::vector<unsigned char> src(sw * sh * 4), dst(dw * dh * 4);
    for (int y = 0; y < sh; ++y) {
        for (int x = 0; x < sw; ++x) {
            const int p = (y * sw + x) * 4;
            src[p] = (x * 7 + y * 3) % 256;
            src[p + 1] = (x * 3 + y * 5) % 256;
            src[p + 2] = (x + y * 11) % 256;
            src[p + 3] = 255;
        }
    }
    BufferShrink task(src.data(), sw, sh, dst.data(), dw, dh);
    REQUIRE(task.clip());
    task.copy();
    for (int i = 0; i < dw * dh; ++i) {
        REQUIRE(dst[i * 4 + 3] == 255);
    }
}

TEST_CASE("shrinkCopy RtoL truncates toward zero") {
    CHECK(ShrinkCopy::RtoL(1.9) == 1);
    CHECK(ShrinkCopy::RtoL(-1.9) == -1);
    CHECK(ShrinkCopy::RtoL(0.0) == 0);
}
