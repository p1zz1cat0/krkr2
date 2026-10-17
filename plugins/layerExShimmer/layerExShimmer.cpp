// layerExShimmer.dll
//
// Port of layerExShimmer 0310 (keepcreating, WTFPL):
//   http://keepcreating.g2.xrea.com/krkrplugins/ShimmerPlugin/layerExShimmer.zip
//
// Adds two Layer methods for a heat-haze effect:
//   Layer.shimmer(src, map, msk, scalex, scaley, clipx, clipy, clipw, cliph)
//     Copies src into this layer. Each pixel moves by the gradient of the
//     map image (blue channel), times scalex/scaley, times msk/255 (blue
//     channel) when msk is not void.
//   Layer.shimmerBuildMap(map1, map1x, map1y, map2, map2x, map2y)
//     Tiles map1 (shifted by map1x/map1y) over this layer. When map2 is not
//     void, writes the per-byte rounded-up average of map1 and map2.
//
// The shipped DLL is the SSE2 build. This port copies the SSE2 behavior, not
// the scalar #ifndef USE_SSE2 branches of the source:
//   - Interior pixels: float gradient * scale (* mask / 255.0f), rounded to
//     nearest even (cvtps2dq). Source coordinates are clamped, not wrapped.
//   - Outer 1 px ring: 16.16 fixed point with int32 wrap-around (MSVC), then
//     clamped. The source computes these pixels in C, not SSE2.
//   - The source does not write 4 ring pixels: (clipx+clipw-2, top/bottom)
//     and (left/right, clipy+cliph-2). This port does not write them either.
//   - shimmerBuildMap with two maps uses pavgb on all 4 bytes. When both
//     offsets (mod map width) are multiples of 4, the source writes only
//     (width & ~3) columns. This port does the same.
//
// Port changes:
//   - Single thread, no SSE2 (KThreadPool and __asm removed).
//   - Buffers come from PluginSafety LayerReadView/LayerWriteView. The source
//     reads mainImageBuffer as a raw pointer and has no bounds checks.
//   - Fork pixel order is RGBA, so "blue" is byte layerPixel::B, not byte 0.
//   - The edge gradient uses neighbors clamped into the clip rectangle. For
//     clipw/cliph >= 2 this is identical to the 8 separate edge loops of the
//     source. For clipw or cliph == 1 the source reads outside the map.
//   - shimmerBuildMap with one map tiles map1 for any map width. The source
//     SSE2 code needs a map width that is a multiple of 4.
//   - When src is this layer, src is copied first. The source result then
//     depends on its write order.
//   - |scale| must be <= 32767 so that scale * 0x10000 fits in int.
//     Offsets larger than 32767 px do not copy the pminsw clamp quirk.
//
// Source bugs that this port does not copy:
//   - KThreadPool does not initialize the worker state. The first shimmer
//     call on a layer can skip the rows of the worker threads.
//   - The SSE2 mask loop has no zero-count check. A mask call with an
//     interior width (clipw - 2) < 4 loops about 2^32 times and crashes.
//
// Verified against the original DLL on krkrz 20150817: all pixels of
// tests/test_files/layerExShimmer are bit-exact (see oracle-startup.tjs).

#include "ncbind.hpp"
#include "common/LayerPixel.h"
#include "common/PluginSafety.h"

#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <spdlog/spdlog.h>
#include <vector>

#define NCB_MODULE_NAME TJS_W("layerExShimmer.dll")

namespace {
void LogPluginError(const char *msg) {
    if(auto logger = spdlog::get("plugin"))
        logger->error("[layerExShimmer] {}", msg);
}

// NCB_ATTACH_FUNCTION skips registration when Layer does not exist yet.
// Resolve Layer before registration so Plugins.link works before the first
// Window/Layer is created.
void EnsureLayerClassRegistered() {
    tTJSVariant layerClass;
    TVPExecuteExpression(TJS_W("Layer"), &layerClass);
    if(layerClass.Type() != tvtObject)
        TVPThrowExceptionMessage(
            TJS_W("layerExShimmer.dll requires the Layer class."));
}
} // namespace

NCB_PRE_REGIST_CALLBACK(EnsureLayerClassRegistered);

// PluginImpl.cpp references this anchor so the static linker keeps this file.
extern "C" void TVPLayerExShimmerPluginAnchor() {}

namespace shimmer {

struct Plane {
    const tjs_uint8 *pixels;
    tjs_int width, height, pitch;

    const tjs_uint8 *at(tjs_int x, tjs_int y) const {
        return pixels + static_cast<std::size_t>(y) * pitch +
            static_cast<std::size_t>(x) * 4;
    }
    int blue(tjs_int x, tjs_int y) const { return at(x, y)[layerPixel::B]; }
};

struct Clip {
    tjs_int x, y, w, h;
};

inline tjs_int Clamp(tjs_int64 v, tjs_int max) {
    return v < 0 ? 0 : v >= max ? max - 1 : static_cast<tjs_int>(v);
}

inline tjs_int Wrap(tjs_int v, tjs_int max) { return ((v % max) + max) % max; }

// Clip rules of the source. Returns false when nothing is drawn.
inline bool ClipShimmer(Clip &c, tjs_int dstW, tjs_int dstH, tjs_int srcW,
                        tjs_int srcH, tjs_int mapW, tjs_int mapH, bool hasMask,
                        tjs_int mskW, tjs_int mskH) {
    if(c.w == 0)
        c.w = srcW;
    if(c.h == 0)
        c.h = srcH;
    if(c.x < 0)
        c.w += c.x, c.x = 0;
    if(c.y < 0)
        c.h += c.y, c.y = 0;
    if(c.w <= 0 || c.h <= 0 || c.x >= dstW || c.y >= dstH)
        return false;
    if(c.x + c.w > dstW)
        c.w = dstW - c.x;
    if(c.y + c.h > dstH)
        c.h = dstH - c.y;
    if(c.w > mapW || c.h > mapH)
        return false;
    if(hasMask && (c.w > mskW || c.h > mskH))
        return false;
    return true;
}

// Ring pixels: 16.16 fixed point. MSVC int math wraps, so multiply as uint32.
inline tjs_int RingOffset(int grad, int fixedScale, const int *mask) {
    const auto product = static_cast<std::uint32_t>(grad) *
        static_cast<std::uint32_t>(fixedScale);
    if(!mask)
        return static_cast<std::int32_t>(product) >> 16;
    const auto masked = static_cast<std::int32_t>(
        product * static_cast<std::uint32_t>(*mask));
    return (masked / 255) >> 16;
}

// Interior pixels: SSE2 mulps then cvtps2dq (round to nearest even).
// Out-of-range values give the cvtps2dq "integer indefinite" INT_MIN.
inline tjs_int InteriorOffset(int grad, float scale, const int *mask) {
    float v = static_cast<float>(grad) * scale;
    if(mask)
        v *= static_cast<float>(*mask) / 255.0f;
    if(!(v >= -2147483648.0f && v < 2147483648.0f))
        return INT_MIN;
    return static_cast<tjs_int>(std::nearbyint(v));
}

// The clip must come from ClipShimmer and fit dst, map and msk.
inline void Shimmer(tjs_uint8 *dst, tjs_int dstPitch, const Plane &src,
                    const Plane &map, const Plane *msk, float scalex,
                    float scaley, const Clip &c) {
    const int fixedX = static_cast<int>(scalex * 0x10000);
    const int fixedY = static_cast<int>(scaley * 0x10000);
    const tjs_int right = c.w - 1, bottom = c.h - 1;
    for(tjs_int my = 0; my < c.h; ++my) {
        auto *row = reinterpret_cast<tjs_uint32 *>(
            dst + static_cast<std::size_t>(c.y + my) * dstPitch);
        const bool rowEdge = my == 0 || my == bottom;
        for(tjs_int mx = 0; mx < c.w; ++mx) {
            const bool colEdge = mx == 0 || mx == right;
            // Source quirk: these ring pixels are not written.
            if((rowEdge && mx == right - 1 && mx > 0) ||
               (colEdge && my == bottom - 1 && my > 0))
                continue;
            const int gx = map.blue(mx < right ? mx + 1 : mx, my) -
                map.blue(mx > 0 ? mx - 1 : mx, my);
            const int gy = map.blue(mx, my < bottom ? my + 1 : my) -
                map.blue(mx, my > 0 ? my - 1 : my);
            int m = 0;
            const int *mp = nullptr;
            if(msk) {
                m = msk->blue(mx, my);
                mp = &m;
            }
            tjs_int ox, oy;
            if(rowEdge || colEdge) {
                ox = RingOffset(gx, fixedX, mp);
                oy = RingOffset(gy, fixedY, mp);
            } else {
                ox = InteriorOffset(gx, scalex, mp);
                oy = InteriorOffset(gy, scaley, mp);
            }
            const tjs_int sx = Clamp(tjs_int64(c.x) + mx + ox, src.width);
            const tjs_int sy = Clamp(tjs_int64(c.y) + my + oy, src.height);
            std::memcpy(&row[c.x + mx], src.at(sx, sy), 4);
        }
    }
}

inline void BuildMap(tjs_uint8 *dst, tjs_int dstW, tjs_int dstH,
                     tjs_int dstPitch, const Plane &map1, tjs_int map1x,
                     tjs_int map1y, const Plane *map2, tjs_int map2x,
                     tjs_int map2y) {
    const tjs_int x1 = Wrap(map1x, map1.width), y1 = Wrap(map1y, map1.height);
    const tjs_int x2 = map2 ? Wrap(map2x, map2->width) : 0;
    const tjs_int y2 = map2 ? Wrap(map2y, map2->height) : 0;
    tjs_int width = dstW;
    if(map2 && x1 % 4 == 0 && x2 % 4 == 0)
        width &= ~3;
    for(tjs_int y = 0; y < dstH; ++y) {
        tjs_uint8 *out = dst + static_cast<std::size_t>(y) * dstPitch;
        const tjs_int r1 = Wrap(y - y1, map1.height);
        const tjs_int r2 = map2 ? Wrap(y - y2, map2->height) : 0;
        for(tjs_int x = 0; x < width; ++x, out += 4) {
            const tjs_uint8 *a = map1.at(Wrap(x - x1, map1.width), r1);
            if(!map2) {
                std::memcpy(out, a, 4);
                continue;
            }
            const tjs_uint8 *b = map2->at(Wrap(x - x2, map2->width), r2);
            for(int i = 0; i < 4; ++i)
                out[i] = static_cast<tjs_uint8>((a[i] + b[i] + 1) >> 1);
        }
    }
}

} // namespace shimmer

namespace {

bool IsLayerObject(const tTJSVariant &v) {
    return v.Type() == tvtObject && v.AsObjectNoAddRef();
}

bool ReadPlane(const tTJSVariant &v, shimmer::Plane &out) {
    if(!IsLayerObject(v))
        return false;
    const auto view = pluginSafety::LayerReadView::create(v.AsObjectNoAddRef());
    if(!view)
        return false;
    out = {view.value.pixels(), view.value.width(), view.value.height(),
           view.value.pitchBytes()};
    return true;
}

bool ReadScale(const tTJSVariant &v, float &out) {
    const auto r = pluginSafety::readFiniteReal(v, -32767.0, 32767.0);
    if(!r)
        return false;
    out = static_cast<float>(r.value);
    return true;
}

tjs_error ShimmerMethod(tTJSVariant *, tjs_int numparams, tTJSVariant **param,
                        iTJSDispatch2 *objthis) {
    if(numparams < 9)
        return TJS_E_BADPARAMCOUNT;
    const auto dst = pluginSafety::LayerWriteView::create(objthis);
    shimmer::Plane src{}, map{}, msk{};
    const bool hasMask = IsLayerObject(*param[2]);
    float scalex = 0, scaley = 0;
    if(!dst || !ReadPlane(*param[0], src) || !ReadPlane(*param[1], map) ||
       (hasMask && !ReadPlane(*param[2], msk))) {
        LogPluginError("shimmer: this, src, map and msk need layer images");
        return TJS_E_INVALIDPARAM;
    }
    if(!ReadScale(*param[3], scalex) || !ReadScale(*param[4], scaley)) {
        LogPluginError("shimmer: scalex/scaley must be finite, |s| <= 32767");
        return TJS_E_INVALIDPARAM;
    }
    shimmer::Clip c{static_cast<tjs_int>(*param[5]),
                    static_cast<tjs_int>(*param[6]),
                    static_cast<tjs_int>(*param[7]),
                    static_cast<tjs_int>(*param[8])};
    if(!shimmer::ClipShimmer(c, dst.value.width(), dst.value.height(),
                             src.width, src.height, map.width, map.height,
                             hasMask, msk.width, msk.height))
        return TJS_S_OK;

    std::vector<tjs_uint8> srcCopy;
    if(src.pixels == dst.value.pixels()) {
        srcCopy.assign(src.pixels,
                       src.pixels + static_cast<std::size_t>(src.height - 1) *
                               src.pitch + static_cast<std::size_t>(src.width) * 4);
        src.pixels = srcCopy.data();
    }
    shimmer::Shimmer(dst.value.pixels(), dst.value.pitchBytes(), src, map,
                     hasMask ? &msk : nullptr, scalex, scaley, c);
    return TJS_S_OK;
}

tjs_error ShimmerBuildMapMethod(tTJSVariant *, tjs_int numparams,
                                tTJSVariant **param, iTJSDispatch2 *objthis) {
    if(numparams < 6)
        return TJS_E_BADPARAMCOUNT;
    const auto dst = pluginSafety::LayerWriteView::create(objthis);
    shimmer::Plane map1{}, map2{};
    const bool hasMap2 = IsLayerObject(*param[3]);
    if(!dst || !ReadPlane(*param[0], map1) ||
       (hasMap2 && !ReadPlane(*param[3], map2))) {
        LogPluginError("shimmerBuildMap: this, map1 and map2 need layer images");
        return TJS_E_INVALIDPARAM;
    }
    shimmer::BuildMap(dst.value.pixels(), dst.value.width(),
                      dst.value.height(), dst.value.pitchBytes(), map1,
                      static_cast<tjs_int>(*param[1]),
                      static_cast<tjs_int>(*param[2]),
                      hasMap2 ? &map2 : nullptr,
                      static_cast<tjs_int>(*param[4]),
                      static_cast<tjs_int>(*param[5]));
    return TJS_S_OK;
}

} // namespace

NCB_ATTACH_FUNCTION(shimmer, Layer, ShimmerMethod);
NCB_ATTACH_FUNCTION(shimmerBuildMap, Layer, ShimmerBuildMapMethod);
