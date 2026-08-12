//
// shrinkCopy 算法回归测试：直接包含插件源文件，验证整数平均缩小的核心逻辑。
// 不依赖 Layer/TJS 运行时，用固定 BGRA 缓冲做确定性断言。
//

#include <cstring>

#include <catch2/catch_test_macros.hpp>

#include "shrinkCopy.cpp"

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

TEST_CASE("shrinkCopy RtoL truncates toward zero") {
    CHECK(ShrinkCopy::RtoL(1.9) == 1);
    CHECK(ShrinkCopy::RtoL(-1.9) == -1);
    CHECK(ShrinkCopy::RtoL(0.0) == 0);
}
