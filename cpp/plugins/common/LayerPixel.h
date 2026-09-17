#ifndef KRKR2_PLUGIN_LAYER_PIXEL_H
#define KRKR2_PLUGIN_LAYER_PIXEL_H

#include <cstddef>

#include "tjsTypes.h"

// Layer mainImageBuffer channel layout in this fork.
//
// The fork stores pixels in RGBA byte order (byte 0 = R) and uploads them to
// GL as GL_RGBA. Upstream Windows plugins assume BGRA (byte 0 = B), so any
// code that treats R and B differently must go through this header:
//   - byte offsets: R/G/B/A below; as a little-endian uint32 the value is
//     A<<24 | B<<16 | G<<8 | R;
//   - TJS color arguments and results (0xAARRGGBB): fromArgb/toArgb;
//   - BGRA byte buffers from upstream decoders: copyBgraRow.
// Code that processes R, G and B identically (averages, blurs, per-channel
// lerp with the same factor) is layout-independent and needs none of this.
namespace layerPixel {

inline constexpr int R = 0;
inline constexpr int G = 1;
inline constexpr int B = 2;
inline constexpr int A = 3;

// 0xAARRGGBB (TJS color) -> buffer uint32. Swapping R/B is an involution, so
// toArgb is the same operation; both names exist to state intent at call sites.
constexpr tjs_uint32 fromArgb(tjs_uint32 argb) noexcept {
    return (argb & 0xFF00FF00u) | ((argb >> 16) & 0xFFu) |
           ((argb & 0xFFu) << 16);
}

constexpr tjs_uint32 toArgb(tjs_uint32 pixel) noexcept {
    return fromArgb(pixel);
}

// Copies `count` pixels from a BGRA byte buffer into layer layout. dst and src
// must not overlap.
inline void copyBgraRow(tjs_uint8 *dst, const tjs_uint8 *src,
                        std::size_t count) noexcept {
    for(std::size_t i = 0; i < count; ++i, dst += 4, src += 4) {
        dst[R] = src[2];
        dst[G] = src[1];
        dst[B] = src[0];
        dst[A] = src[3];
    }
}

} // namespace layerPixel

#endif
