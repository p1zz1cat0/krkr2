//
// layerExAreaAverage 算法回归测试：直接包含插件源文件，验证面积平均缩小的
// 核心逻辑。不依赖 Layer/TJS 运行时，用固定 BGRA 缓冲做确定性断言。
// 期望值按上游定点算法手算（DOTBASE=12，除法向零截断）。
//

#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "layerExAreaAverage/layerExAreaAverage.cpp"

namespace {

// 以 fork 的 mainImageBuffer 位序打包像素：uint32 = A<<24|B<<16|G<<8|R
// （RGBA 内存序，低字节 R；与 shrinkCopy 的读写约定一致，与上游 Windows
// krkr2 的 A<<24|R<<16|G<<8|B 相反）。参数按 (a, r, g, b) 颜色语义命名，
// 与上游算法的术语对应。
constexpr tjs_uint32 Pixel(tjs_uint32 a, tjs_uint32 r, tjs_uint32 g,
                           tjs_uint32 b) {
    return (a << 24) | (b << 16) | (g << 8) | r;
}

LayerExAreaAverage::CopyGeometry MakeGeometry(tjs_int dw, tjs_int dh, tjs_int sw,
                                              tjs_int sh) {
    return LayerExAreaAverage::CopyGeometry{0, 0, dw, dh, 0, 0, sw, sh};
}

} // namespace

TEST_CASE("layerExAreaAverage fixed-point primitives") {
    CHECK(LayerExAreaAverage::IntToFixDot(1) == 4096);
    CHECK(LayerExAreaAverage::RealToFixDot(0.5) == 2048);
    CHECK(LayerExAreaAverage::MulFixDot(4096, 4096) == 4096);
    CHECK(LayerExAreaAverage::MulFixDot(2048, 4096) == 2048);
    CHECK(LayerExAreaAverage::DivFixDot(8192, 2) == 4096);
    CHECK(LayerExAreaAverage::FixDotToInt(4096) == 1);
    CHECK(LayerExAreaAverage::FixDotToInt(4095) == 0);
}

TEST_CASE("layerExAreaAverage solid color shrinks to itself") {
    // 8x8 纯色 → 4x4：每个目标像素的平均值必须回到原色
    std::vector<tjs_uint32> src(8 * 8, Pixel(0xFF, 0x7F, 0x3F, 0x1F));
    std::vector<tjs_uint32> dst(4 * 4, 0);
    auto g = MakeGeometry(4, 4, 8, 8);
    LayerExAreaAverage::CopyAreaAverage(
        reinterpret_cast<tjs_uint8 *>(dst.data()), 4 * 4,
        reinterpret_cast<const tjs_uint8 *>(src.data()), 8 * 4, g, 8, 8);
    for(tjs_int i = 0; i < 16; i++)
        CHECK(dst[i] == Pixel(0xFF, 0x7F, 0x3F, 0x1F));
}

TEST_CASE("layerExAreaAverage pitch may exceed row bytes") {
    // 源 pitch 带填充字节时不影响结果（8x8 → 4x4，pitch = 10 像素行）
    std::vector<tjs_uint32> src(8 * 10, Pixel(0xFF, 0x7F, 0x3F, 0x1F));
    std::vector<tjs_uint32> dst(4 * 6, 0);
    auto g = MakeGeometry(4, 4, 8, 8);
    LayerExAreaAverage::CopyAreaAverage(
        reinterpret_cast<tjs_uint8 *>(dst.data()), 6 * 4,
        reinterpret_cast<const tjs_uint8 *>(src.data()), 10 * 4, g, 8, 8);
    // 只断言实际像素行；行间填充不属于写入范围
    for(tjs_int y = 0; y < 4; y++)
        for(tjs_int x = 0; x < 4; x++)
            CHECK(dst[y * 6 + x] == Pixel(0xFF, 0x7F, 0x3F, 0x1F));
}

TEST_CASE("layerExAreaAverage averages two colors with weights") {
    // 2x1 [蓝, 绿] → 1x1：alpha 加权面积各半，红通道为 0，
    // 绿/蓝 = 255*4080/8160 = 127（截断）
    const tjs_uint32 src[2] = {Pixel(0xFF, 0x00, 0x00, 0xFF),
                               Pixel(0xFF, 0x00, 0xFF, 0x00)};
    tjs_uint32 dst[1] = {0};
    auto g = MakeGeometry(1, 1, 2, 1);
    LayerExAreaAverage::CopyAreaAverage(
        reinterpret_cast<tjs_uint8 *>(dst), 4,
        reinterpret_cast<const tjs_uint8 *>(src), 2 * 4, g, 2, 1);
    CHECK(dst[0] == Pixel(0xFF, 0x00, 0x7F, 0x7F));
}

TEST_CASE("layerExAreaAverage transparent pixels do not tint color") {
    // 2x1 [不透明蓝, 全透明(0x123456)] → 1x1：
    // 平均 alpha = (255*4096 + 0)/8192 = 127（截断），
    // 颜色只取不透明像素 → 纯蓝；透明像素的颜色被忽略
    const tjs_uint32 src[2] = {Pixel(0xFF, 0x00, 0x00, 0xFF),
                               Pixel(0x00, 0x12, 0x34, 0x56)};
    tjs_uint32 dst[1] = {0};
    auto g = MakeGeometry(1, 1, 2, 1);
    LayerExAreaAverage::CopyAreaAverage(
        reinterpret_cast<tjs_uint8 *>(dst), 4,
        reinterpret_cast<const tjs_uint8 *>(src), 2 * 4, g, 2, 1);
    CHECK(dst[0] == Pixel(0x7F, 0x00, 0x00, 0xFF));
}

TEST_CASE("layerExAreaAverage fully transparent source keeps its color") {
    // 2x2 全透明 → 1x1：totalarea_rgb 为 0 时回退到透明像素颜色（上游语义）
    std::vector<tjs_uint32> src(4, Pixel(0x00, 0xAB, 0xCD, 0xEF));
    tjs_uint32 dst[1] = {0xFFFFFFFFu};
    auto g = MakeGeometry(1, 1, 2, 2);
    LayerExAreaAverage::CopyAreaAverage(
        reinterpret_cast<tjs_uint8 *>(dst), 4,
        reinterpret_cast<const tjs_uint8 *>(src.data()), 2 * 4, g, 2, 2);
    CHECK(dst[0] == Pixel(0x00, 0xAB, 0xCD, 0xEF));
}

TEST_CASE("layerExAreaAverage non-integral ratio weighting") {
    // 3x3 灰阶 → 2x2（rw = 1.5 像素）：目标像素对源像素的覆盖权重为
    // 4096:2048 组合；期望值由定点算法逐项计算
    const tjs_uint32 r[3][3] = {
        {0x10, 0x20, 0x30},
        {0x40, 0x50, 0x60},
        {0x70, 0x80, 0x90},
    };
    tjs_uint32 src[9];
    for(tjs_int y = 0; y < 3; y++)
        for(tjs_int x = 0; x < 3; x++)
            src[y * 3 + x] = Pixel(0xFF, r[y][x], r[y][x], r[y][x]);
    tjs_uint32 dst[4] = {0, 0, 0, 0};
    auto g = MakeGeometry(2, 2, 3, 3);
    LayerExAreaAverage::CopyAreaAverage(
        reinterpret_cast<tjs_uint8 *>(dst), 2 * 4,
        reinterpret_cast<const tjs_uint8 *>(src), 3 * 4, g, 3, 3);
    CHECK(dst[0] == Pixel(0xFF, 0x25, 0x25, 0x25));
    CHECK(dst[1] == Pixel(0xFF, 0x3A, 0x3A, 0x3A));
    CHECK(dst[2] == Pixel(0xFF, 0x65, 0x65, 0x65));
    CHECK(dst[3] == Pixel(0xFF, 0x7A, 0x7A, 0x7A));
}

TEST_CASE("layerExAreaAverage int64 accumulators survive extreme shrink") {
    // 512x512 全不透明白 → 1x1：alpha 累加达 255 * 511^2 * 4096 ≈ 2.7e11，
    // 上游 int32 累加器在此溢出 UB；int64 必须输出不透明白
    std::vector<tjs_uint32> src(512 * 512, 0xFFFFFFFFu);
    tjs_uint32 dst[1] = {0};
    auto g = MakeGeometry(1, 1, 512, 512);
    LayerExAreaAverage::CopyAreaAverage(
        reinterpret_cast<tjs_uint8 *>(dst), 4,
        reinterpret_cast<const tjs_uint8 *>(src.data()), 512 * 4, g, 512, 512);
    CHECK(dst[0] == 0xFFFFFFFFu);
}

TEST_CASE("layerExAreaAverage ClipToImages transfers overshoot ratio") {
    // 目标矩形超出目标 image：按比例同步收缩源矩形
    LayerExAreaAverage::CopyGeometry g{2, 2, 4, 4, 0, 0, 8, 8};
    LayerExAreaAverage::ClipToImages(g, 4, 4, 8, 8);
    CHECK(g.dWidth == 2);
    CHECK(g.dHeight == 2);
    CHECK(g.sWidth == 4);
    CHECK(g.sHeight == 4);

    // 源矩形超出源 image：按比例反向收缩目标矩形（10 * 15/20 = 7.5 → 7）
    LayerExAreaAverage::CopyGeometry g2{0, 0, 10, 10, 0, 0, 20, 20};
    LayerExAreaAverage::ClipToImages(g2, 20, 20, 15, 15);
    CHECK(g2.sWidth == 15);
    CHECK(g2.sHeight == 15);
    CHECK(g2.dWidth == 7);
    CHECK(g2.dHeight == 7);
}

TEST_CASE("layerExAreaAverage ClipToImages collapses out-of-image source") {
    // 源矩形完全出界：收缩为退化矩形，调用方据此跳过绘制
    LayerExAreaAverage::CopyGeometry g{0, 0, 4, 4, 100, 100, 50, 50};
    LayerExAreaAverage::ClipToImages(g, 4, 4, 8, 8);
    CHECK(g.sWidth < 0);
    CHECK(g.dWidth < 0);
}

TEST_CASE("layerExAreaAverage ValidateParams accepts valid geometry") {
    tTJSVariant v0((tjs_int64)0), v1((tjs_int64)0), v2((tjs_int64)2),
        v3((tjs_int64)2), v4(0), v5((tjs_int64)0), v6((tjs_int64)0),
        v7((tjs_int64)4), v8((tjs_int64)4);
    tTJSVariant *param[9] = {&v0, &v1, &v2, &v3, &v4,
                             &v5, &v6, &v7, &v8};
    LayerExAreaAverage::CopyGeometry g{};
    REQUIRE(LayerExAreaAverage::ValidateParams(9, param, g) == TJS_S_OK);
    CHECK(g.dWidth == 2);
    CHECK(g.sHeight == 4);
}

TEST_CASE("layerExAreaAverage ValidateParams rejects bad param count") {
    tTJSVariant v0((tjs_int64)0), v1((tjs_int64)0), v2((tjs_int64)2),
        v3((tjs_int64)2), v4(0), v5((tjs_int64)0), v6((tjs_int64)0),
        v7((tjs_int64)4), v8((tjs_int64)4);
    tTJSVariant *param[9] = {&v0, &v1, &v2, &v3, &v4,
                             &v5, &v6, &v7, &v8};
    LayerExAreaAverage::CopyGeometry g{};
    REQUIRE(LayerExAreaAverage::ValidateParams(8, param, g) ==
            TJS_E_BADPARAMCOUNT);
}

TEST_CASE("layerExAreaAverage ValidateParams rejects upstream-UB input") {
    // 负坐标与零尺寸在上游是未定义行为，移植显式拒绝
    tTJSVariant neg((tjs_int64)-1), zero((tjs_int64)0), two((tjs_int64)2),
        four((tjs_int64)4), pad(0);
    tTJSVariant *param[9] = {&neg, &zero, &two, &two,
                             &pad, &zero, &zero, &four, &four};
    LayerExAreaAverage::CopyGeometry g{};
    REQUIRE(LayerExAreaAverage::ValidateParams(9, param, g) ==
            TJS_E_INVALIDPARAM);

    tTJSVariant *zeroSize[9] = {&zero, &zero, &zero, &two,
                                &pad, &zero, &zero, &four, &four};
    REQUIRE(LayerExAreaAverage::ValidateParams(9, zeroSize, g) ==
            TJS_E_INVALIDPARAM);
}

TEST_CASE("layerExAreaAverage ValidateParams throws on enlargement") {
    // 放大按上游语义抛 TJS 异常（仅缩小约束）
    tTJSVariant zero((tjs_int64)0), big((tjs_int64)8), small((tjs_int64)4),
        pad(0);
    tTJSVariant *param[9] = {&zero, &zero, &big, &big,
                             &pad, &zero, &zero, &small, &small};
    LayerExAreaAverage::CopyGeometry g{};
    REQUIRE_THROWS(LayerExAreaAverage::ValidateParams(9, param, g));
}
