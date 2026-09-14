//
// layerExSave 算法回归测试：layerExSave 是独立静态库（add_subdirectory +
// ZLIB），链接行会展开其全部成员对象，因此本测试不 include 源文件（会与
// 库成员 duplicate symbol），改为链接库本体、直接调用导出的编码器与
// utils API。在无 TJS/Layer 运行时下验证核心逻辑。
// 像素缓冲按 fork 的 mainImageBuffer 位序构造：uint32 = A<<24|B<<16|G<<8|R
// （RGBA 内存序，低字节 R）。
// 期望值按上游 wtnbgo/layerExSave 算法手算；TLG5/PNG 的真实解码回读由
// smoke fixture（loadImages 回读）覆盖，此处断言文件结构与字节输出。
//

#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "layerExSave/LayerExSaveEncoders.h"
#include "layerExSave/LayerExSaveUtils.h"
#include "layerExSave/thirdparty/lodepng/lodepng.h"

#include <zlib.h>

namespace {

// 以 fork 的 mainImageBuffer 位序打包像素（与 shrinkCopy/layerExAreaAverage
// 测试的约定一致）
constexpr tjs_uint32 Pixel(tjs_uint32 a, tjs_uint32 r, tjs_uint32 g,
                           tjs_uint32 b) {
    return (a << 24) | (b << 16) | (g << 8) | r;
}

LayerExSave::ImageSource MakeSource(const std::vector<tjs_uint32> &pixels,
                                    tjs_int w, tjs_int h, tjs_int pitchBytes) {
    return LayerExSave::ImageSource{
        reinterpret_cast<const tjs_uint8 *>(pixels.data()), w, h, pitchBytes};
}

tjs_uint32 readPixelLE(const tjs_uint8 *p) {
    return static_cast<tjs_uint32>(p[0]) | (static_cast<tjs_uint32>(p[1]) << 8) |
           (static_cast<tjs_uint32>(p[2]) << 16) |
           (static_cast<tjs_uint32>(p[3]) << 24);
}

// 手动解析 PNG：提取 IHDR 与全部 IDAT payload 并 inflate 返回明文。
struct PngParts {
    tjs_uint32 width = 0, height = 0;
    tjs_uint32 ihdrFlags = 0;
    std::vector<tjs_uint8> idatPlaintext;
};

PngParts ParsePng(const std::vector<tjs_uint8> &png) {
    REQUIRE(png.size() >= 57);
    REQUIRE(std::memcmp(png.data(), "\x89PNG\x0D\x0A\x1A\x0A", 8) == 0);
    PngParts parts;
    std::size_t at = 8;
    std::vector<tjs_uint8> idat;
    while(at + 8 <= png.size()) {
        const tjs_uint32 len = (png[at] << 24) | (png[at + 1] << 16) |
                               (png[at + 2] << 8) | png[at + 3];
        const char *type = reinterpret_cast<const char *>(&png[at + 4]);
        const tjs_uint8 *payload = &png[at + 8];
        // CRC 校验
        const unsigned long crc = ::crc32(
            0, &png[at + 4], static_cast<uInt>(len + 4));
        const tjs_uint32 expect = (png[at + 8 + len] << 24) |
                                  (png[at + 9 + len] << 16) |
                                  (png[at + 10 + len] << 8) |
                                  png[at + 11 + len];
        REQUIRE(static_cast<tjs_uint32>(crc) == expect);

        if(std::memcmp(type, "IHDR", 4) == 0) {
            parts.width = (payload[0] << 24) | (payload[1] << 16) |
                          (payload[2] << 8) | payload[3];
            parts.height = (payload[4] << 24) | (payload[5] << 16) |
                           (payload[6] << 8) | payload[7];
            parts.ihdrFlags =
                (payload[8] << 24) | (payload[9] << 16) | (payload[10] << 8) |
                payload[11];
            REQUIRE(payload[12] == 0); // 非 interlace
        } else if(std::memcmp(type, "IDAT", 4) == 0) {
            idat.insert(idat.end(), payload, payload + len);
        } else if(std::memcmp(type, "IEND", 4) == 0) {
            at += 12 + len;
            break;
        }
        at += 12 + len;
    }
    REQUIRE(at == png.size()); // IEND 之后无多余字节
    // inflate（zlib 流）
    parts.idatPlaintext.resize(parts.height *
                               (static_cast<std::size_t>(parts.width) * 4 + 1));
    uLongf destLen = parts.idatPlaintext.size();
    REQUIRE(::uncompress(parts.idatPlaintext.data(), &destLen, idat.data(),
                         static_cast<uLong>(idat.size())) == Z_OK);
    return parts;
}

} // namespace

TEST_CASE("layerExSave TLG5 header and block layout") {
    // 8x5 → blockcount = 2
    std::vector<tjs_uint32> pixels(8 * 5);
    for(std::size_t i = 0; i < pixels.size(); i++)
        pixels[i] = Pixel(0xFF, 0x11, 0x22, 0x33);
    const auto src = MakeSource(pixels, 8, 5, 32);
    auto enc = LayerExSave::CreateTlg5Encoder(src, ttstr());
    REQUIRE(enc);
    while(enc->step()) {
    }
    REQUIRE(enc->finished());
    const auto &data = enc->data();

    // TLG5.0\x00raw\x1a\x00 + colors=4 + w/h (LE) + blockheight=4
    REQUIRE(data.size() >= 28);
    REQUIRE(std::memcmp(data.data(), "TLG5.0\x00raw\x1a\x00", 11) == 0);
    CHECK(data[11] == 4);
    const auto rd32 = [&](std::size_t at) {
        return static_cast<tjs_uint32>(data[at]) |
               (static_cast<tjs_uint32>(data[at + 1]) << 8) |
               (static_cast<tjs_uint32>(data[at + 2]) << 16) |
               (static_cast<tjs_uint32>(data[at + 3]) << 24);
    };
    CHECK(rd32(12) == 8);
    CHECK(rd32(16) == 5);
    CHECK(rd32(20) == 4);
    // 块尺寸表在 header（24 字节）之后，2 块
    const tjs_uint32 b0 = rd32(24), b1 = rd32(28);
    CHECK(b0 > 0);
    CHECK(b1 > 0);
    // 两块之后文件结束：24(header) + 8(块表) + 块数据
    CHECK(data.size() == 24 + 8 + b0 + b1);
}

TEST_CASE("layerExSave TLG5 chunked steps match one-shot run") {
    std::vector<tjs_uint32> pixels(10 * 12);
    for(std::size_t i = 0; i < pixels.size(); i++)
        pixels[i] = Pixel(0x80 + (i % 64) * 2, (i * 7) & 0xff,
                          (i * 13) & 0xff, (i * 29) & 0xff);
    const auto src = MakeSource(pixels, 10, 12, 40);

    auto all = LayerExSave::CreateTlg5Encoder(src, ttstr());
    while(all->step()) {
    }
    auto piecewise = LayerExSave::CreateTlg5Encoder(src, ttstr());
    int guard = 0;
    while(piecewise->step() && ++guard < 1000) {
    }
    CHECK(guard <= 3); // 12 行 → 3 块，每块一个 step
    REQUIRE(all->data().size() == piecewise->data().size());
    CHECK(std::memcmp(all->data().data(), piecewise->data().data(),
                      all->data().size()) == 0);
}

TEST_CASE("layerExSave TLG5 tags chunk wrapper") {
    std::vector<tjs_uint32> pixels(4 * 4, Pixel(0xFF, 1, 2, 3));
    const auto src = MakeSource(pixels, 4, 4, 16);
    // 上游 TagsCaller 序列化格式："len:name=len:value,"
    const ttstr tags(TJS_W("4:name=5:value,"));
    auto enc = LayerExSave::CreateTlg5Encoder(src, tags);
    while(enc->step()) {
    }
    const auto &data = enc->data();
    // TLG0.0\x00sds\x1a\x00 + rawlen(4) + raw TLG + "tags" + len + payload
    REQUIRE(std::memcmp(data.data(), "TLG0.0\x00sds\x1a\x00", 11) == 0);
    const auto rd32 = [&](std::size_t at) {
        return static_cast<tjs_uint32>(data[at]) |
               (static_cast<tjs_uint32>(data[at + 1]) << 8) |
               (static_cast<tjs_uint32>(data[at + 2]) << 16) |
               (static_cast<tjs_uint32>(data[at + 3]) << 24);
    };
    const tjs_uint32 rawLen = rd32(11);
    REQUIRE(rawLen < data.size());
    std::size_t tagsAt = 11 + 4 + rawLen;
    REQUIRE(std::memcmp(&data[tagsAt], "tags", 4) == 0);
    const tjs_uint32 tagsLen = rd32(tagsAt + 4);
    // narrow 字节长度按 ASCII 内容核对（name/value 均为 ASCII）
    CHECK(tagsLen == data.size() - (tagsAt + 8));
    CHECK(std::memcmp(&data[tagsAt + 8], "4:name=5:value,", tagsLen) == 0);
}

TEST_CASE("layerExSave PNG chunked encoder emits valid RGBA stream") {
    // 2x2：混合 alpha 检查 RGBA 直写（无通道交换）
    const tjs_uint32 pixels[2 * 2] = {
        Pixel(0xFF, 0x10, 0x20, 0x30), Pixel(0x80, 0x40, 0x50, 0x60),
        Pixel(0x00, 0x70, 0x80, 0x90), Pixel(0xFF, 0xA0, 0xB0, 0xC0)};
    std::vector<tjs_uint32> pixelVec(pixels, pixels + 4);
    const auto src = MakeSource(pixelVec, 2, 2, 8);
    auto enc = LayerExSave::CreatePngChunkEncoder(src, LayerExSave::PngTagParams());
    while(enc->step()) {
    }
    const PngParts parts = ParsePng(enc->data());
    CHECK(parts.width == 2);
    CHECK(parts.height == 2);
    CHECK(parts.ihdrFlags == 0x08060000); // bitdepth 8 | colortype RGBA
    REQUIRE(parts.idatPlaintext.size() == 2 * (2 * 4 + 1));
    // filter 字节全 0，行明文与源内存序一致（fork RGBA → PNG RGBA 直写）
    for(int y = 0; y < 2; y++) {
        CHECK(parts.idatPlaintext[y * (2 * 4 + 1)] == 0);
        for(int x = 0; x < 2; x++)
            CHECK(readPixelLE(&parts.idatPlaintext[y * (2 * 4 + 1) + 1 + x * 4]) ==
                  pixels[y * 2 + x]);
    }
}

TEST_CASE("layerExSave PNG chunked steps match one-shot run and honor reso chunk") {
    std::vector<tjs_uint32> pixels(5 * 33);
    for(std::size_t i = 0; i < pixels.size(); i++)
        pixels[i] = Pixel(0xFF, (i * 3) & 0xff, (i * 11) & 0xff, 0x40);
    const auto src = MakeSource(pixels, 5, 33, 20);

    LayerExSave::PngTagParams tags;
    tags.hasReso = true;
    tags.resoX = tags.resoY = 3780; // 96dpi
    tags.resoMeter = true;

    auto all = LayerExSave::CreatePngChunkEncoder(src, tags);
    while(all->step()) {
    }
    auto piecewise = LayerExSave::CreatePngChunkEncoder(src, tags);
    while(piecewise->step()) {
    }
    REQUIRE(all->data().size() == piecewise->data().size());
    CHECK(std::memcmp(all->data().data(), piecewise->data().data(),
                      all->data().size()) == 0);

    // pHYs chunk 存在且内容正确（大端 3780,3780,unit=1）
    const auto &png = all->data();
    bool found = false;
    for(std::size_t at = 8; at + 8 <= png.size();) {
        const tjs_uint32 len = (png[at] << 24) | (png[at + 1] << 16) |
                               (png[at + 2] << 8) | png[at + 3];
        if(std::memcmp(&png[at + 4], "pHYs", 4) == 0) {
            found = true;
            const tjs_uint32 x = (png[at + 8] << 24) | (png[at + 9] << 16) |
                                 (png[at + 10] << 8) | png[at + 11];
            CHECK(x == 3780);
            CHECK(png[at + 16] == 1); // unit = meter
            break;
        }
        at += 12 + len;
    }
    CHECK(found);
}

TEST_CASE("layerExSave lodepng path round-trips RGBA pixels") {
    const tjs_uint32 pixels[2 * 3] = {
        Pixel(0xFF, 0x01, 0x02, 0x03), Pixel(0xFF, 0x04, 0x05, 0x06),
        Pixel(0x7F, 0x07, 0x08, 0x09), Pixel(0xFF, 0x0A, 0x0B, 0x0C),
        Pixel(0x00, 0x0D, 0x0E, 0x0F), Pixel(0xFF, 0x10, 0x11, 0x12)};
    std::vector<tjs_uint32> pixelVec(pixels, pixels + 6);
    const auto src = MakeSource(pixelVec, 2, 3, 8);
    std::vector<tjs_uint8> png;
    REQUIRE(LayerExSave::EncodeWithLodePNG(src, nullptr, png));
    // 有非 255 alpha → RGBA
    lexsave::lodepng::State state;
    unsigned w = 0, h = 0;
    std::vector<tjs_uint8> decoded;
    REQUIRE(lexsave::lodepng::decode(decoded, w, h, png) == 0);
    REQUIRE(w == 2);
    REQUIRE(h == 3);
    REQUIRE(decoded.size() == 2 * 3 * 4); // lodepng::decode 默认展开 RGBA
    for(int i = 0; i < 6; i++)
        CHECK(readPixelLE(&decoded[i * 4]) == pixels[i]);
}

TEST_CASE("layerExSave lodepng province encodes 256-palette PNG") {
    // 1 字节/像素的 province 缓冲
    const tjs_uint8 province[2 * 2] = {0, 1, 2, 3};
    std::vector<tjs_uint8> png;
    REQUIRE(LayerExSave::EncodeProvinceWithLodePNG(province, 2, 2, 2, png));
    REQUIRE(std::memcmp(png.data(), "\x89PNG\x0D\x0A\x1A\x0A", 8) == 0);
    // IHDR 的 colortype 必须是 3（palette）
    const tjs_uint8 colortype = png[8 + 8 + 9];
    CHECK(colortype == 3);
}

TEST_CASE("layerExSave crop rect finds opaque bounds") {
    // 4x4，只有 (1,1)-(2,2) 不透明
    std::vector<tjs_uint32> pixels(16, 0);
    for(int y = 1; y <= 2; y++)
        for(int x = 1; x <= 2; x++)
            pixels[y * 4 + x] = Pixel(0xFF, 1, 2, 3);
    const auto src = MakeSource(pixels, 4, 4, 16);

    const auto crop = LayerExSave::CalcCropRect(src.pixels, 4, 4, 16, false);
    REQUIRE(crop.hasCrop);
    CHECK(crop.x == 1);
    CHECK(crop.y == 1);
    CHECK(crop.w == 2);
    CHECK(crop.h == 2);

    // 全透明 → void（hasCrop=false）
    std::vector<tjs_uint32> blank(16, 0);
    const auto srcBlank = MakeSource(blank, 4, 4, 16);
    CHECK_FALSE(LayerExSave::CalcCropRect(srcBlank.pixels, 4, 4, 16, false)
                    .hasCrop);

    // zeroMode：非零 RGB 但 alpha=0 的像素算"有内容"
    std::vector<tjs_uint32> zerosOnly(4, Pixel(0x00, 5, 6, 7));
    const auto srcZero = MakeSource(zerosOnly, 2, 2, 8);
    CHECK_FALSE(LayerExSave::CalcCropRect(srcZero.pixels, 2, 2, 8, false)
                    .hasCrop);
    CHECK(LayerExSave::CalcCropRect(srcZero.pixels, 2, 2, 8, true).hasCrop);
}

TEST_CASE("layerExSave diff rect locates changed region") {
    std::vector<tjs_uint32> base(4 * 4, Pixel(0xFF, 0, 0, 0));
    std::vector<tjs_uint32> modified = base;
    modified[1 * 4 + 2] = Pixel(0xFF, 9, 9, 9); // (2,1) 处改色
    const auto from = MakeSource(base, 4, 4, 16);
    const auto to = MakeSource(modified, 4, 4, 16);

    LayerExSave::CropResult crop;
    REQUIRE(LayerExSave::CalcDiffRect(from.pixels, 4, 4, 16, to.pixels, 4, 4,
                                      16, crop));
    REQUIRE(crop.hasCrop);
    CHECK(crop.x == 2);
    CHECK(crop.y == 1);
    CHECK(crop.w == 1);
    CHECK(crop.h == 1);

    // 完全相同 → 无差分
    std::vector<tjs_uint32> same = base;
    const auto toSame = MakeSource(same, 4, 4, 16);
    LayerExSave::CropResult none;
    REQUIRE(LayerExSave::CalcDiffRect(from.pixels, 4, 4, 16, toSame.pixels, 4,
                                      4, 16, none));
    CHECK_FALSE(none.hasCrop);

    // 尺寸不一致 → false（TJS 层转异常）
    const auto small = MakeSource(base, 2, 2, 8);
    LayerExSave::CropResult err;
    CHECK_FALSE(LayerExSave::CalcDiffRect(from.pixels, 4, 4, 16, small.pixels,
                                          2, 2, 8, err));
}

TEST_CASE("layerExSave diff pixel counts and fills in fork byte order") {
    std::vector<tjs_uint32> base(2, Pixel(0xFF, 1, 1, 1));
    std::vector<tjs_uint32> modified = base;
    modified[1] = Pixel(0xFF, 2, 2, 2);
    const auto from = MakeSource(base, 2, 1, 8);
    auto toBuf = modified;
    const auto to = MakeSource(toBuf, 2, 1, 8);

    const tjs_int count = LayerExSave::CalcDiffPixel(
        from.pixels, 2, 1, 8,
        reinterpret_cast<tjs_uint8 *>(toBuf.data()), 2, 1, 8, false, 0,
        false, 0);
    CHECK(count == 1);

    // diffcol 0xFF3366CC → fork 内存字节序 R,G,B,A = 33,66,CC,FF
    toBuf = modified;
    LayerExSave::CalcDiffPixel(from.pixels, 2, 1, 8,
                               reinterpret_cast<tjs_uint8 *>(toBuf.data()),
                               2, 1, 8, false, 0, true, 0xFF3366CC);
    const tjs_uint32 written = readPixelLE(reinterpret_cast<const tjs_uint8 *>(
        toBuf.data()) + 4);
    CHECK(written == Pixel(0xFF, 0x33, 0x66, 0xCC));
}

TEST_CASE("layerExSave ooze color extends edges into transparency") {
    // 2x1：左不透明 (10,20,30)，右全透明；fill=0 → 透明像素被左邻居
    // 延伸为 (10,20,30)，alpha 不变
    std::vector<tjs_uint32> pixels(2);
    pixels[0] = Pixel(0xFF, 10, 20, 30);
    pixels[1] = 0;
    tjs_uint8 *buf = reinterpret_cast<tjs_uint8 *>(pixels.data());
    LayerExSave::OozeColor(buf, 2, 1, 8, 1, 1, 0);
    CHECK(readPixelLE(buf) == Pixel(0xFF, 10, 20, 30));
    CHECK(readPixelLE(buf + 4) == Pixel(0, 10, 20, 30));

    // 低 alpha 但非零（threshold=1 之下）先被 fillColor 覆盖再延伸
    std::vector<tjs_uint32> filled(2);
    filled[0] = Pixel(0xFF, 10, 20, 30);
    filled[1] = Pixel(0x00, 9, 9, 9);
    tjs_uint8 *buf2 = reinterpret_cast<tjs_uint8 *>(filled.data());
    LayerExSave::OozeColor(buf2, 2, 1, 8, 1, 1, 0x00AABBCC);
    CHECK(readPixelLE(buf2 + 4) == Pixel(0, 10, 20, 30));
}

TEST_CASE("layerExSave copy blue to alpha") {
    std::vector<tjs_uint32> src(2, 0);
    std::vector<tjs_uint32> dst(2, 0);
    src[0] = Pixel(0xFF, 0, 0, 0x42);
    src[1] = Pixel(0x11, 0, 0, 0x7F);
    const auto s = MakeSource(src, 2, 1, 8);
    tjs_uint8 *dbuf = reinterpret_cast<tjs_uint8 *>(dst.data());
    LayerExSave::CopyBlueToAlpha(dbuf, 2, 1, 8, s.pixels, 2, 1, 8);
    // fork 的 B 通道 = p[2]；dst 字节 3/7 为两个像素的 alpha
    CHECK(dbuf[3] == 0x42);
    CHECK(dbuf[7] == 0x7F);
}

TEST_CASE("layerExSave is blank clips region to source bounds") {
    std::vector<tjs_uint32> pixels(4 * 4, 0);
    pixels[2 * 4 + 2] = Pixel(0xFF, 1, 1, 1);
    const auto src = MakeSource(pixels, 4, 4, 16);
    CHECK(LayerExSave::IsBlank(src.pixels, 4, 4, 16, 0, 0, 4, 4) == false);
    CHECK(LayerExSave::IsBlank(src.pixels, 4, 4, 16, 0, 0, 2, 2));
    // 越界区域裁剪后不含内容像素
    CHECK(LayerExSave::IsBlank(src.pixels, 4, 4, 16, 3, 3, 8, 8));
    // 负起点裁剪后扩展为全图，仍含内容像素
    CHECK_FALSE(LayerExSave::IsBlank(src.pixels, 4, 4, 16, -4, -4, 8, 8));
}

TEST_CASE("layerExSave clear alpha fills below threshold with transparent fill") {
    std::vector<tjs_uint32> pixels;
    pixels.push_back(Pixel(0x10, 1, 2, 3)); // alpha 16 ≤ threshold 16
    pixels.push_back(Pixel(0x11, 4, 5, 6)); // alpha 17 > threshold
    tjs_uint8 *buf = reinterpret_cast<tjs_uint8 *>(pixels.data());
    LayerExSave::ClearAlpha(buf, 2, 1, 8, 16, 0x00AABBCC);
    // 被清除：alpha=0 且颜色 = fill（fork 字节序 R=AA,G=BB,B=CC）
    CHECK(readPixelLE(buf) == Pixel(0x00, 0xAA, 0xBB, 0xCC));
    // 未被清除：保持原样
    CHECK(readPixelLE(buf + 4) == Pixel(0x11, 4, 5, 6));
}

TEST_CASE("layerExSave average color averages in fork byte order") {
    std::vector<tjs_uint32> pixels;
    pixels.push_back(Pixel(0x10, 2, 4, 6));
    pixels.push_back(Pixel(0x20, 4, 8, 12));
    const auto src = MakeSource(pixels, 2, 1, 8);
    const tjs_uint32 color = LayerExSave::CalcAverageColor(
        src.pixels, 2, 1, 8, 0, 0, 2, 1);
    // a=(0x10+0x20)/2=0x18, r=3, g=6, b=9 → 0xAARRGGBB
    CHECK(color == 0x18030609u);
    // 全部越界的空区域抛异常
    CHECK_THROWS(LayerExSave::CalcAverageColor(src.pixels, 2, 1, 8, 5, 5, 1,
                                               1));
}

TEST_CASE("layerExSave fingerprint is pitch independent and stable") {
    // 2x2 内容，两个 pitch 不同的缓冲
    std::vector<tjs_uint32> tight(4);
    std::vector<tjs_uint32> padded(2 * 4);
    const tjs_uint32 content[4] = {Pixel(0xFF, 1, 2, 3), Pixel(0x80, 4, 5, 6),
                                   Pixel(0x00, 7, 8, 9), Pixel(0xFF, 10, 11, 12)};
    for(int i = 0; i < 4; i++) {
        tight[i] = content[i];
        padded[(i / 2) * 4 + (i % 2)] = content[i];
    }
    const auto a = MakeSource(tight, 2, 2, 8);
    const auto b = MakeSource(padded, 2, 2, 16);
    const tjs_uint64 ha = LayerExSave::FingerprintValue(a.pixels, 2, 2, 8, true);
    const tjs_uint64 hb = LayerExSave::FingerprintValue(b.pixels, 2, 2, 16, true);
    CHECK(ha == hb);
    // ignore_transparent 只影响完全透明像素的参与
    const tjs_uint64 strict = LayerExSave::FingerprintValue(a.pixels, 2, 2, 8, false);
    CHECK(strict != ha);
    // 内容变化 → 指纹变化
    std::vector<tjs_uint32> changed = tight;
    changed[0] = Pixel(0xFF, 99, 2, 3);
    const auto c = MakeSource(changed, 2, 2, 8);
    CHECK(LayerExSave::FingerprintValue(c.pixels, 2, 2, 8, true) != ha);
}

TEST_CASE("layerExSave shrink vector packs b,g,r,a in little-endian order") {
    // 2x1：两个不透明像素，颜色按 fork 内存序存放
    std::vector<tjs_uint32> pixels;
    pixels.push_back(Pixel(0xFF, 0x10, 0x20, 0x30));
    pixels.push_back(Pixel(0xFF, 0x40, 0x50, 0x60));
    const auto src = MakeSource(pixels, 2, 1, 8);
    const auto vec = LayerExSave::ShrinkVector(src.pixels, 2, 1, 8, 1, 1);
    REQUIRE(vec.size() == 1);
    // 均值 (b,g,r,a) = (0x48, 0x38, 0x28, 0xFF)
    // 打包低字节起 b,g,r,a（与上游 Windows 输出一致）
    CHECK(vec[0] == 0xFF283848u);
}

TEST_CASE("layerExSave octet vector sum single and paired") {
    LayerExSave::VectorSumWork work;
    const tjs_uint8 a[3] = {1, 2, 3};
    const tjs_uint8 b[3] = {1, 2, 3};
    REQUIRE(LayerExSave::OctetVectorSum(a, 3, b, 3, work));
    CHECK(work.csum == 1 + 4 + 9);
    CHECK(work.dsum == 0);
    CHECK(work.psum1 == 1 + 4 + 9);
    CHECK(work.psum2 == 1 + 4 + 9);
    CHECK(work.nsum1 == 6);
    CHECK(work.nsum2 == 6);
    CHECK(work.diff == 0);

    LayerExSave::VectorSumWork single;
    REQUIRE(LayerExSave::OctetVectorSum(a, 3, nullptr, 0, single));
    CHECK(single.dsum == 1 + 4 + 9);
    CHECK(single.psum1 == 1 + 4 + 9);
    CHECK(single.psum2 == 0);
    CHECK(single.nsum1 == 6);
    CHECK(single.diff == 3); // 三个非零元素

    LayerExSave::VectorSumWork mismatch;
    CHECK_FALSE(LayerExSave::OctetVectorSum(a, 3, b, 2, mismatch));
}
