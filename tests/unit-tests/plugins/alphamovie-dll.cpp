//
// AlphaMovie 解码回归测试：直接包含插件源文件，用 make_fixtures.py 合成的
// 16x16 DC-only 固件做像素级断言，不依赖 TJS/Window/Layer 运行时。
//
// 固件约定 (见 tests/test_files/alphamovie/make_fixtures.py)：
//   量化表全 16 → IDCT 后像素 = 2 * DC 累积值 + 128
//   JPEG-alpha 帧0: Y 块 [0,2,-2,4] / A 块 [0,8,4,-4]，Cb=Cr=0
//   JPEG-alpha 帧1: Y 全 0 / A 全 2 (验证跨帧共享预测子重置)
//   zlib-alpha 帧0: Y [0,-2,2,0]，alpha = 50+8x+2y
//   zlib-alpha 帧1: Y 全 0，alpha = 200-8x-2y
//

#include <cstring>
#include <fstream>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "alphamovie/AlphaMovie.cpp"
#include "test_config.h"

namespace {

struct ParsedFrame {
    int left, top, w, h;
    uint32_t alphaZLen = 0;
    std::vector<uint8_t> alphaZ;
    std::vector<uint8_t> color;
};

struct ParsedAmv {
    uint32_t flags = 0;
    uint32_t numFrames = 0;
    uint8_t quant[3][64] = {};
    std::vector<ParsedFrame> frames;
};

uint16_t rdU16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
int16_t rdS16(const uint8_t *p) { return (int16_t)rdU16(p); }
uint32_t rdU32(const uint8_t *p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
}

// 测试端独立解析 .amv（与插件实现无关的黄金解析器）
ParsedAmv parseAmv(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    REQUIRE(data.size() >= 0x28);

    ParsedAmv amv;
    amv.flags = rdU32(&data[0x24]);
    amv.numFrames = rdU32(&data[0x14]);
    uint32_t headerSize = rdU32(&data[0x0c]);
    REQUIRE(headerSize == 0x28 + (amv.flags & 1 ? 0xc0 : 0x80));
    for (int t = 0; t < 3; t++) {
        const uint8_t *src = &data[0x28 + t * 64];
        for (int i = 0; i < 64; i++) amv.quant[t][i] = src[i];
    }

    size_t ofs = headerSize;
    for (uint32_t i = 0; i < amv.numFrames; i++) {
        REQUIRE(ofs + 12 <= data.size());
        REQUIRE(rdU32(&data[ofs]) == 0x4d415246); // 'FRAM'
        uint32_t size = rdU32(&data[ofs + 4]);
        ParsedFrame f;
        f.left = rdS16(&data[ofs + 12]);
        f.top = rdS16(&data[ofs + 14]);
        f.w = rdU16(&data[ofs + 16]);
        f.h = rdU16(&data[ofs + 18]);
        if (amv.flags & 1) {
            REQUIRE(size >= 12);
            f.color.assign(&data[ofs + 20], &data[ofs + 20] + size - 12);
        } else {
            REQUIRE(size >= 16);
            f.alphaZLen = rdU32(&data[ofs + 20]);
            REQUIRE(f.alphaZLen <= size - 16);
            f.alphaZ.assign(&data[ofs + 24], &data[ofs + 24] + f.alphaZLen);
            size_t colorLen = size - 16 - f.alphaZLen;
            f.color.assign(&data[ofs + 24 + f.alphaZLen],
                           &data[ofs + 24 + f.alphaZLen] + colorLen);
        }
        amv.frames.push_back(std::move(f));
        ofs += (size_t)size + 8;
    }
    return amv;
}

amvdec::HuffTable makeTables(amvdec::HuffTable &dcLuma, amvdec::HuffTable &dcChroma,
                             amvdec::HuffTable &acLuma, amvdec::HuffTable &acChroma) {
    dcLuma.build(std_dc_luminance_bits, std_dc_luminance_val);
    dcChroma.build(std_dc_chrominance_bits, std_dc_chrominance_val);
    acLuma.build(std_ac_luminance_bits, std_ac_luminance_val);
    acChroma.build(std_ac_chrominance_bits, std_ac_chrominance_val);
    return dcLuma;
}

// 解析整文件并把第 frame 帧解码为 BGRA
std::vector<uint8_t> decodeFixtureFrame(const ParsedAmv &amv, size_t frame) {
    amvdec::HuffTable dcLuma, dcChroma, acLuma, acChroma;
    makeTables(dcLuma, dcChroma, acLuma, acChroma);
    const ParsedFrame &f = amv.frames.at(frame);
    std::vector<uint8_t> bgra;
    decodeFramePayloadToBGRA(
        amv.quant, dcLuma, dcChroma, acLuma, acChroma,
        (amv.flags & 2) != 0,
        f.alphaZ.data(), f.alphaZLen,
        f.color.data(), (uint32_t)f.color.size(),
        f.w, f.h, bgra);
    return bgra;
}

// 取 (x,y) 处 BGRA 像素
void px(const std::vector<uint8_t> &bgra, int w, int x, int y,
        uint8_t &b, uint8_t &g, uint8_t &r, uint8_t &a) {
    const uint8_t *p = &bgra[(size_t)(y * w + x) * 4];
    b = p[0]; g = p[1]; r = p[2]; a = p[3];
}

} // namespace

TEST_CASE("AlphaMovie BitReader unstuffs 0xFF00 and honors markers") {
    // 0x5A 0xFF 0x00 0xA5 → 0xFF00 应还原为一个 0xFF 字面量
    const uint8_t data[] = { 0x5A, 0xFF, 0x00, 0xA5 };
    amvdec::BitReader br(data, sizeof(data));
    CHECK(br.bits(8) == 0x5A);
    CHECK(br.bits(8) == 0xFF);
    CHECK(br.bits(8) == 0xA5);
}

TEST_CASE("AlphaMovie HuffTable decodes standard DC categories") {
    amvdec::HuffTable dc, dcC, ac, acC;
    makeTables(dc, dcC, ac, acC);
    // DC luma cat0='00' cat2='011' cat4='101'
    // '00' + '011' + '101' = 00011101
    const uint8_t data[] = { 0x1D };
    amvdec::BitReader br(data, sizeof(data));
    CHECK(br.decode(dc) == 0);
    CHECK(br.decode(dc) == 2);
    CHECK(br.decode(dc) == 4);
    // AC luma EOB='1010'
    const uint8_t eob[] = { 0xA0 };
    amvdec::BitReader beob(eob, sizeof(eob));
    CHECK(beob.decode(ac) == 0);
}

TEST_CASE("AlphaMovie JPEG-alpha fixture decodes pixel-exact") {
    auto amv = parseAmv(TEST_FILES_PATH "/alphamovie/sample_jpeg.amv");
    REQUIRE(amv.numFrames == 2);

    auto f0 = decodeFixtureFrame(amv, 0);
    REQUIRE(f0.size() == (size_t)16 * 16 * 4);
    uint8_t b, g, r, a;
    // 帧0 块序: TL Y=128 A=128 / TR Y=132 A=144 / BL Y=124 A=136 / BR Y=136 A=120
    struct Expect { int x, y, yv, av; } pts[] = {
        { 3, 3, 128, 128 }, { 11, 3, 132, 144 },
        { 3, 11, 124, 136 }, { 11, 11, 136, 120 },
    };
    for (const auto &e : pts) {
        px(f0, 16, e.x, e.y, b, g, r, a);
        INFO("frame0 x=" << e.x << " y=" << e.y);
        CHECK(b == e.yv); CHECK(g == e.yv); CHECK(r == e.yv);
        CHECK(a == e.av);
    }
    // 整帧 alpha 值分布: TL 128 / TR 144 / BL 136 / BR 120
    px(f0, 16, 0, 0, b, g, r, a); CHECK(a == 128);
    px(f0, 16, 15, 0, b, g, r, a); CHECK(a == 144);
    px(f0, 16, 0, 15, b, g, r, a); CHECK(a == 136);
    px(f0, 16, 15, 15, b, g, r, a); CHECK(a == 120);

    // 帧1: Y 全 128, A 全 132 → 验证共享预测子在帧边界重置
    auto f1 = decodeFixtureFrame(amv, 1);
    REQUIRE(f1.size() == (size_t)16 * 16 * 4);
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            px(f1, 16, x, y, b, g, r, a);
            INFO("frame1 x=" << x << " y=" << y);
            CHECK(b == 128); CHECK(g == 128); CHECK(r == 128);
            CHECK(a == 132);
        }
    }
}

TEST_CASE("AlphaMovie zlib-alpha fixture decodes pixel-exact") {
    auto amv = parseAmv(TEST_FILES_PATH "/alphamovie/sample_zlib.amv");
    REQUIRE(amv.numFrames == 2);

    auto f0 = decodeFixtureFrame(amv, 0);
    REQUIRE(f0.size() == (size_t)16 * 16 * 4);
    uint8_t b, g, r, a;
    struct Expect { int x, y, yv; } pts[] = {
        { 3, 3, 128 }, { 11, 3, 124 }, { 3, 11, 132 }, { 11, 11, 128 },
    };
    for (const auto &e : pts) {
        px(f0, 16, e.x, e.y, b, g, r, a);
        INFO("zlib frame0 x=" << e.x << " y=" << e.y);
        CHECK(b == e.yv); CHECK(g == e.yv); CHECK(r == e.yv);
    }
    // alpha = 50 + 8x + 2y
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            px(f0, 16, x, y, b, g, r, a);
            INFO("zlib frame0 alpha x=" << x << " y=" << y);
            CHECK(a == 50 + 8 * x + 2 * y);
        }

    auto f1 = decodeFixtureFrame(amv, 1);
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            px(f1, 16, x, y, b, g, r, a);
            INFO("zlib frame1 x=" << x << " y=" << y);
            CHECK(b == 128); CHECK(g == 128); CHECK(r == 128);
            CHECK(a == 200 - 8 * x - 2 * y);
        }
}

TEST_CASE("AlphaMovie rejects invalid frame dimensions") {
    amvdec::HuffTable dcL, dcC, acL, acC;
    makeTables(dcL, dcC, acL, acC);
    uint8_t quant[3][64] = {};
    for (int t = 0; t < 3; t++)
        for (int i = 0; i < 64; i++) quant[t][i] = 16;
    std::vector<uint8_t> bgra;

    // 非 16 倍数
    CHECK_THROWS_AS(decodeFramePayloadToBGRA(quant, dcL, dcC, acL, acC, false,
                                             nullptr, 0, nullptr, 0, 20, 16, bgra),
                    eTJSError);
    // 零尺寸
    CHECK_THROWS_AS(decodeFramePayloadToBGRA(quant, dcL, dcC, acL, acC, false,
                                             nullptr, 0, nullptr, 0, 0, 16, bgra),
                    eTJSError);
    // 超过 4096 上限（原版会按 uint16 直接分配数 GB 平面）
    CHECK_THROWS_AS(decodeFramePayloadToBGRA(quant, dcL, dcC, acL, acC, false,
                                             nullptr, 0, nullptr, 0, 8192, 16, bgra),
                    eTJSError);
}

TEST_CASE("AlphaMovie rejects truncated entropy") {
	// 熵流只有 Cb 的半个块，必须报告损坏而不是用 0 补位生成伪像素。
    amvdec::HuffTable dcL, dcC, acL, acC;
    makeTables(dcL, dcC, acL, acC);
    uint8_t quant[3][64] = {};
    for (int t = 0; t < 3; t++)
        for (int i = 0; i < 64; i++) quant[t][i] = 16;
    const uint8_t tiny[] = { 0x00 };
    std::vector<uint8_t> bgra;
	CHECK_THROWS_AS(decodeFramePayloadToBGRA(quant, dcL, dcC, acL, acC, false,
	                                         nullptr, 0, tiny, 1, 16, 16, bgra),
	                eTJSError);
}

TEST_CASE("AlphaMovie corrupted zlib alpha falls back to opaque") {
    auto amv = parseAmv(TEST_FILES_PATH "/alphamovie/sample_zlib.amv");
    REQUIRE(amv.numFrames >= 1);

    amvdec::HuffTable dcL, dcC, acL, acC;
    makeTables(dcL, dcC, acL, acC);
    // 破坏 alpha zlib 数据
    amv.frames[0].alphaZ.assign(amv.frames[0].alphaZ.size(), 0x00);
    const ParsedFrame &f = amv.frames[0];
    std::vector<uint8_t> bgra;
    CHECK_NOTHROW(decodeFramePayloadToBGRA(
        amv.quant, dcL, dcC, acL, acC, true,
        f.alphaZ.data(), f.alphaZLen,
        f.color.data(), (uint32_t)f.color.size(),
        f.w, f.h, bgra));
    uint8_t b, g, r, a;
    px(bgra, 16, 5, 5, b, g, r, a);
    CHECK(a == 255); // 回退为不透明 (与原版行为一致)
}
