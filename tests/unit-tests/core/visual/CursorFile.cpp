// Windows 光标文件（.cur / .ani）解码用例：游标的通道顺序、热点、AND 掩码和
// 动画步进都会直接影响屏幕上看到的指针，构造最小文件逐项断言。
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "CursorFile.h"

namespace {

using Bytes = std::vector<unsigned char>;

void AppendU16(Bytes &out, uint16_t value) {
    out.push_back((unsigned char)(value & 0xff));
    out.push_back((unsigned char)(value >> 8));
}

void AppendU32(Bytes &out, uint32_t value) {
    for(int i = 0; i < 4; i++)
        out.push_back((unsigned char)((value >> (8 * i)) & 0xff));
}

void AppendSignature(Bytes &out, const char *signature) {
    for(int i = 0; i < 4; i++)
        out.push_back((unsigned char)signature[i]);
}

//! RIFF chunk: id, little endian body size, body, alignment byte.
void AppendChunk(Bytes &out, const char *id, const Bytes &body) {
    AppendSignature(out, id);
    AppendU32(out, (uint32_t)body.size());
    out.insert(out.end(), body.begin(), body.end());
    if(body.size() & 1)
        out.push_back(0);
}

//! BITMAPINFOHEADER + palette + 颜色行 + AND 掩码，行序自下而上（DIB 规定）。
Bytes BuildDib(int width, int height, int bpp, const Bytes &palette,
               const Bytes &colorRows, const Bytes &maskRows) {
    Bytes out;
    AppendU32(out, 40);
    AppendU32(out, (uint32_t)width);
    AppendU32(out, (uint32_t)(height * 2)); // 颜色位图 + 掩码
    AppendU16(out, 1);                      // biPlanes
    AppendU16(out, (uint16_t)bpp);
    AppendU32(out, 0); // BI_RGB
    AppendU32(out, (uint32_t)(colorRows.size() + maskRows.size()));
    AppendU32(out, 0);
    AppendU32(out, 0);
    AppendU32(out, bpp <= 8 ? (uint32_t)(palette.size() / 4) : 0);
    AppendU32(out, 0);
    out.insert(out.end(), palette.begin(), palette.end());
    out.insert(out.end(), colorRows.begin(), colorRows.end());
    out.insert(out.end(), maskRows.begin(), maskRows.end());
    return out;
}

//! 32 bpp 光标位图。pixel(x, y) 返回文件里的 BGRA，mask(x, y) 为 true 表示掩码
//! 置 1（透明）。
template <typename PixelFn, typename MaskFn>
Bytes Build32bppDib(int width, int height, PixelFn pixel, MaskFn mask) {
    Bytes colorRows;
    for(int y = height - 1; y >= 0; y--) {
        for(int x = 0; x < width; x++)
            AppendU32(colorRows, pixel(x, y));
    }

    const int maskPitch = ((width + 31) / 32) * 4;
    Bytes maskRows;
    for(int y = height - 1; y >= 0; y--) {
        Bytes row((size_t)maskPitch, 0);
        for(int x = 0; x < width; x++) {
            if(mask(x, y))
                row[(size_t)(x >> 3)] |= (unsigned char)(0x80 >> (x & 7));
        }
        maskRows.insert(maskRows.end(), row.begin(), row.end());
    }

    return BuildDib(width, height, 32, {}, colorRows, maskRows);
}

//! 单个图像的 .cur：目录 + 一个条目。
Bytes BuildCur(const Bytes &dib, int width, int height, int hotSpotX,
               int hotSpotY) {
    Bytes out;
    AppendU16(out, 0); // reserved
    AppendU16(out, 2); // type: cursor
    AppendU16(out, 1); // one image
    out.push_back((unsigned char)(width >= 256 ? 0 : width));
    out.push_back((unsigned char)(height >= 256 ? 0 : height));
    out.push_back(0); // colors
    out.push_back(0); // reserved
    AppendU16(out, (uint16_t)hotSpotX);
    AppendU16(out, (uint16_t)hotSpotY);
    AppendU32(out, (uint32_t)dib.size());
    AppendU32(out, 22); // 6 byte directory + 16 byte entry
    out.insert(out.end(), dib.begin(), dib.end());
    return out;
}

//! ANI 容器：anih 描述帧数、步数和默认速率，rate / seq 驱动动画。
Bytes BuildAni(const std::vector<Bytes> &frames,
               const std::vector<uint32_t> &sequence,
               const std::vector<uint32_t> &rates, uint32_t defaultRate) {
    Bytes aniHeader;
    AppendU32(aniHeader, 36); // cbSizeof
    AppendU32(aniHeader, (uint32_t)frames.size());
    AppendU32(aniHeader, (uint32_t)sequence.size());
    AppendU32(aniHeader, 0); // width
    AppendU32(aniHeader, 0); // height
    AppendU32(aniHeader, 32);
    AppendU32(aniHeader, 1); // planes
    AppendU32(aniHeader, defaultRate);
    AppendU32(aniHeader, 1); // AF_ICON

    Bytes framesBody;
    AppendSignature(framesBody, "fram");
    for(const Bytes &frame : frames)
        AppendChunk(framesBody, "icon", frame);

    Bytes rateBody;
    for(uint32_t rate : rates)
        AppendU32(rateBody, rate);

    Bytes seqBody;
    for(uint32_t index : sequence)
        AppendU32(seqBody, index);

    Bytes body;
    AppendSignature(body, "ACON");
    AppendChunk(body, "anih", aniHeader);
    if(!rates.empty())
        AppendChunk(body, "rate", rateBody);
    if(!sequence.empty())
        AppendChunk(body, "seq ", seqBody);
    AppendChunk(body, "LIST", framesBody);

    Bytes out;
    AppendSignature(out, "RIFF");
    AppendU32(out, (uint32_t)body.size());
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

const tjs_uint8 *PixelAt(const tTVPCursorResource &resource, size_t step, int x,
                         int y) {
    const tTVPCursorImage &image = resource.Steps[step].Image;
    return image.Pixels.data() + ((size_t)y * image.Width + x) * 4;
}

} // namespace

TEST_CASE("cur 32bpp 转成自上而下的 RGBA 并保留热点", "[cursor]") {
    const Bytes dib = Build32bppDib(
        4, 4,
        [](int x, int y) -> uint32_t {
            // 文件里按 B、G、R、A 存放：B = x * 10，G = y * 10，R = 0x80，A = 0xff
            return 0xff000000u | (0x80u << 16) | ((uint32_t)(y * 10) << 8) |
                (uint32_t)(x * 10);
        },
        [](int, int) { return false; });
    const Bytes cursor = BuildCur(dib, 4, 4, 1, 2);

    tTVPCursorResource resource;
    REQUIRE(TVPDecodeCursorFile(cursor.data(), cursor.size(), resource));

    REQUIRE(resource.Steps.size() == 1);
    CHECK(resource.HotSpotX == 1);
    CHECK(resource.HotSpotY == 2);
    const tTVPCursorImage &image = resource.Steps.front().Image;
    REQUIRE(image.Width == 4);
    REQUIRE(image.Height == 4);
    REQUIRE(image.Pixels.size() == 4 * 4 * 4);

    // 输出是 RGBA8888，自顶向下：B/G/R 各自来自哪个通道必须对得上。
    const tjs_uint8 *pixel = PixelAt(resource, 0, 3, 2);
    CHECK((int)pixel[0] == 0x80); // R
    CHECK((int)pixel[1] == 20);   // G 来自 y
    CHECK((int)pixel[2] == 30);   // B 来自 x
    CHECK((int)pixel[3] == 0xff); // A
}

TEST_CASE("32bpp 全零 alpha 时改用 AND 掩码", "[cursor]") {
    const Bytes dib = Build32bppDib(
        4, 4, [](int, int) -> uint32_t { return 0x00c0c0c0u; },
        [](int x, int y) { return x == 2 && y == 2; });
    const Bytes cursor = BuildCur(dib, 4, 4, 0, 0);

    tTVPCursorResource resource;
    REQUIRE(TVPDecodeCursorFile(cursor.data(), cursor.size(), resource));
    CHECK((int)PixelAt(resource, 0, 2, 2)[3] == 0x00);
    CHECK((int)PixelAt(resource, 0, 0, 0)[3] == 0xff);
}

TEST_CASE("8bpp 调色板光标把 BGR 调色板转成 RGB", "[cursor]") {
    // 两个调色板项：纯蓝、纯红
    Bytes palette;
    palette.insert(palette.end(), { 0xff, 0x00, 0x00, 0x00 });
    palette.insert(palette.end(), { 0x00, 0x00, 0xff, 0x00 });

    // 2x2 颜色索引：上行 [1, 0]，下行 [0, 1]（自下而上存储）
    Bytes colorRows = { 0, 1, 0, 0, /*pad*/ 1, 0, 0, 0 };
    Bytes maskRows(8, 0);
    const Bytes dib = BuildDib(2, 2, 8, palette, colorRows, maskRows);

    tTVPCursorResource resource;
    const Bytes cursor = BuildCur(dib, 2, 2, 1, 1);
    REQUIRE(TVPDecodeCursorFile(cursor.data(), cursor.size(), resource));

    REQUIRE(resource.Steps.size() == 1);
    const tjs_uint8 *topLeft = PixelAt(resource, 0, 0, 0);
    CHECK((int)topLeft[0] == 0xff); // 索引 1 -> 调色板里存的是 BGR
    CHECK((int)topLeft[1] == 0x00);
    CHECK((int)topLeft[2] == 0x00);
    CHECK((int)topLeft[3] == 0xff);

    const tjs_uint8 *topRight = PixelAt(resource, 0, 1, 0);
    CHECK((int)topRight[0] == 0x00);
    CHECK((int)topRight[2] == 0xff);
}

TEST_CASE("ani 按 seq 和 rate 展开动画步", "[cursor]") {
    const auto solid = [](uint32_t bgra) {
        return Build32bppDib(2, 2, [bgra](int, int) -> uint32_t { return bgra; },
                             [](int, int) { return false; });
    };
    // solid(bgra) 的字节序是 B、G、R、A
    const Bytes red = BuildCur(solid(0xffff0000u), 2, 2, 0, 1);   // R = 0xff
    const Bytes green = BuildCur(solid(0xff00ff00u), 2, 2, 1, 0); // G = 0xff

    const Bytes ani =
        BuildAni({ red, green }, { 1, 0, 1 }, { 6, 12, 6 }, 6);

    tTVPCursorResource resource;
    REQUIRE(TVPDecodeCursorFile(ani.data(), ani.size(), resource));

    REQUIRE(resource.Steps.size() == 3);
    CHECK(resource.Steps[0].DurationMS == 100);
    CHECK(resource.Steps[1].DurationMS == 200);
    CHECK(resource.Steps[2].DurationMS == 100);

    // 第 0 步取第 1 帧（绿），第 1 步取第 0 帧（红）
    CHECK((int)PixelAt(resource, 0, 0, 0)[1] == 0xff);
    CHECK((int)PixelAt(resource, 0, 0, 0)[0] == 0x00);
    CHECK((int)PixelAt(resource, 1, 0, 0)[0] == 0xff);
    CHECK((int)PixelAt(resource, 1, 0, 0)[1] == 0x00);

    // 热点取自帧里的 .cur 条目
    CHECK(resource.HotSpotX == 0);
    CHECK(resource.HotSpotY == 1);
}

TEST_CASE("ani 缺 rate 时用 anih 的默认速率", "[cursor]") {
    const auto solid = [](uint32_t bgra) {
        return Build32bppDib(1, 1, [bgra](int, int) -> uint32_t { return bgra; },
                             [](int, int) { return false; });
    };
    const Bytes frame = BuildCur(solid(0xff000000u), 1, 1, 0, 0);
    const Bytes ani = BuildAni({ frame, frame }, { 0, 1 }, {}, 12);

    tTVPCursorResource resource;
    REQUIRE(TVPDecodeCursorFile(ani.data(), ani.size(), resource));
    REQUIRE(resource.Steps.size() == 2);
    CHECK(resource.Steps[0].DurationMS == 200);
    CHECK(resource.Steps[1].DurationMS == 200);
}

TEST_CASE("损坏或非光标数据一律拒绝", "[cursor]") {
    tTVPCursorResource resource;
    CHECK(TVPDecodeCursorFile(nullptr, 0, resource) == false);

    const unsigned char png[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    CHECK(TVPDecodeCursorFile(png, sizeof(png), resource) == false);

    const Bytes dib = Build32bppDib(
        2, 2, [](int, int) -> uint32_t { return 0xffffffffu; },
        [](int, int) { return false; });
    // 条目指向文件外
    Bytes broken = BuildCur(dib, 2, 2, 0, 0);
    broken[18] = 0xff;
    broken[19] = 0xff;
    CHECK(TVPDecodeCursorFile(broken.data(), broken.size(), resource) == false);

    // 截断的 ANI：只剩 RIFF 头
    const Bytes ani = BuildAni({}, {}, {}, 6);
    CHECK(TVPDecodeCursorFile(ani.data(), 12, resource) == false);
}
