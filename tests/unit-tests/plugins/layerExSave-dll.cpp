//
// layerExSave 编码器回归测试：不依赖 Layer/TJS 运行时，直接用固定 RGBA
// 缓冲驱动 CompressTLG5，按 TLG5 格式手算期望字节。Layer 成员的真实
// 注册、调用与 core 读取器往返由 tests/test_files/layerExSave smoke 覆盖。
//

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ncbind.hpp"
#include "layerExSave/savetlg5.hpp"

namespace {

struct Tlg5Probe : CompressTLG5 {
    const std::vector<unsigned char> bytes() const {
        return std::vector<unsigned char>(data.begin(), data.begin() + size);
    }
};

} // namespace

TEST_CASE("layerExSave TLG5 writes B,G,R,A channels from RGBA memory") {
    // 1x1 像素，RGBA 内存：R=0x10 G=0x20 B=0x30 A=0x40
    const unsigned char pixel[4] = {0x10, 0x20, 0x30, 0x40};
    Tlg5Probe tlg;
    REQUIRE_FALSE(tlg.main(1, 1, pixel, 4));
    const auto out = tlg.bytes();

    // header(11) + colors(1) + w/h/blockheight(12) + blocksizes(1*4)
    REQUIRE(out.size() == 28 + 4 * (1 + 4 + 1));
    CHECK(std::string(out.begin(), out.begin() + 6) == "TLG5.0");
    CHECK(out[11] == 4);

    // 首行无上行、无左邻：通道 0..3 = B-G, G, R-G, A；1 字节无法压缩，
    // 每通道以 raw 标记 0x01 + 长度 1 存储
    const unsigned char expected[4] = {0x30 - 0x20, 0x20,
                                       static_cast<unsigned char>(0x10 - 0x20),
                                       0x40};
    for(int c = 0; c < 4; c++) {
        const size_t at = 28 + c * 6;
        INFO("channel " << c);
        CHECK(out[at] == 0x01);
        CHECK(out[at + 1] == 1);
        CHECK(out[at + 5] == expected[c]);
    }
}

TEST_CASE("layerExSave TLG5 honors pitch padding and upper-row deltas") {
    // 1x2，pitch 8（每行 4 字节填充）；第二行与第一行差值编码
    const unsigned char rows[16] = {0x10, 0x20, 0x30, 0x40, 0xEE, 0xEE, 0xEE, 0xEE,
                                    0x11, 0x22, 0x33, 0x44, 0xEE, 0xEE, 0xEE, 0xEE};
    Tlg5Probe tlg;
    REQUIRE_FALSE(tlg.main(1, 2, rows, 8));
    const auto out = tlg.bytes();

    // 两行同属一个 block（BLOCK_HEIGHT=4）：每通道 raw 2 字节
    REQUIRE(out.size() == 28 + 4 * (1 + 4 + 2));
    // 第二行 delta：B=3 G=2 R=1 A=4 → 通道 B-G=1, G=2, R-G=0xFF, A=4
    const unsigned char second[4] = {1, 2, 0xFF, 4};
    for(int c = 0; c < 4; c++) {
        const size_t at = 28 + c * 7;
        INFO("channel " << c);
        CHECK(out[at] == 0x01);
        CHECK(out[at + 6] == second[c]);
    }
}
