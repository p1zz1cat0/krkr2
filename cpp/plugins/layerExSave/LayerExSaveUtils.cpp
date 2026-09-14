// layerExSave.dll Layer 杂项函数的算法核心实现
// 头文件记录移植来源与通道序约定，此处不重复。

#include "LayerExSaveUtils.h"

#include "MsgIntf.h"

#include <cstring>

namespace LayerExSave {
namespace {

// 上游 IS_SAME_COLOR：alpha 相等且（全透明或 RGB 各通道相等）。
// 比较只在同序两缓冲的相同字节位置进行，与通道语义无关。
inline bool isSameColor(const tjs_uint8 *p1, const tjs_uint8 *p2) {
    return p1[3] == p2[3] &&
           (p1[3] == 0 ||
            (p1[0] == p2[0] && p1[1] == p2[1] && p1[2] == p2[2]));
}

// 与上游 CheckTransp / CheckZerop 同签名：(p, next, count)。
inline bool rowHasOpaque(const tjs_uint8 *p, tjs_int next, tjs_int count) {
    for(; count > 0; count--, p += next)
        if(p[3] != 0)
            return true;
    return false;
}

inline bool rowHasNonZero(const tjs_uint8 *p, tjs_int next, tjs_int count) {
    for(; count > 0; count--, p += next)
        if(p[0] != 0 || p[1] != 0 || p[2] != 0 || p[3] != 0)
            return true;
    return false;
}

inline bool rowDiffers(const tjs_uint8 *p1, tjs_int next1, const tjs_uint8 *p2,
                       tjs_int next2, tjs_int count) {
    for(; count > 0; count--, p1 += next1, p2 += next2)
        if(!isSameColor(p1, p2))
            return true;
    return false;
}

// TJS 0xAARRGGBB → fork 内存序 dword（r,g,b,a）。
inline tjs_uint32 tjsColorToMemoryWord(tjs_uint32 color) {
    const tjs_uint32 a = (color >> 24) & 0xff;
    const tjs_uint32 r = (color >> 16) & 0xff;
    const tjs_uint32 g = (color >> 8) & 0xff;
    const tjs_uint32 b = color & 0xff;
    return (a << 24) | (b << 16) | (g << 8) | r;
}

} // namespace

CropResult CalcCropRect(const tjs_uint8 *pixels, tjs_int width, tjs_int height,
                        tjs_int pitchBytes, bool zeroMode) {
    CropResult out;
    const tjs_int nc = 4, nl = pitchBytes;
    const auto hasContent =
        zeroMode ? rowHasNonZero : rowHasOpaque;

    tjs_int x1 = 0, y1 = 0, x2 = width - 1, y2 = height - 1;

    const tjs_uint8 *p = pixels;
    for(; x1 < width; x1++, p += nc)
        if(hasContent(p, nl, height))
            break;
    if(x1 >= width)
        return out; // 全部透明
    p = pixels + x2 * nc;
    for(; x2 >= 0; x2--, p -= nc)
        if(hasContent(p, nl, height))
            break;
    const tjs_int rw = x2 - x1 + 1;
    p = pixels + x1 * nc;
    for(; y1 < height; y1++, p += nl)
        if(hasContent(p, nc, rw))
            break;
    p = pixels + x1 * nc + y2 * nl;
    for(; y2 >= 0; y2--, p -= nl)
        if(hasContent(p, nc, rw))
            break;

    out.hasCrop = true;
    out.x = x1;
    out.y = y1;
    out.w = rw;
    out.h = y2 - y1 + 1;
    return out;
}

bool CalcDiffRect(const tjs_uint8 *from, tjs_int fw, tjs_int fh,
                  tjs_int fpitchBytes, const tjs_uint8 *to, tjs_int tw,
                  tjs_int th, tjs_int tpitchBytes, CropResult &out) {
    if(fw != tw || fh != th)
        return false;
    const tjs_int nc = 4;
    tjs_int x1 = 0, y1 = 0, x2 = tw - 1, y2 = th - 1;

    const tjs_uint8 *fp = from, *tp = to;
    for(; x1 < tw; x1++, fp += nc, tp += nc)
        if(rowDiffers(fp, fpitchBytes, tp, tpitchBytes, th))
            break;
    if(x1 >= tw)
        return true; // 完全相同
    fp = from + x2 * nc;
    tp = to + x2 * nc;
    for(; x2 >= 0; x2--, fp -= nc, tp -= nc)
        if(rowDiffers(fp, fpitchBytes, tp, tpitchBytes, th))
            break;
    const tjs_int rw = x2 - x1 + 1;
    fp = from + x1 * nc;
    tp = to + x1 * nc;
    for(; y1 < th; y1++, fp += fpitchBytes, tp += tpitchBytes)
        if(rowDiffers(fp, nc, tp, nc, rw))
            break;
    fp = from + x1 * nc + y2 * fpitchBytes;
    tp = to + x1 * nc + y2 * tpitchBytes;
    for(; y2 >= 0; y2--, fp -= fpitchBytes, tp -= tpitchBytes)
        if(rowDiffers(fp, nc, tp, nc, rw))
            break;

    out.hasCrop = true;
    out.x = x1;
    out.y = y1;
    out.w = rw;
    out.h = y2 - y1 + 1;
    return true;
}

tjs_int CalcDiffPixel(const tjs_uint8 *from, tjs_int fw, tjs_int fh,
                      tjs_int fpitchBytes, tjs_uint8 *to, tjs_int tw,
                      tjs_int th, tjs_int tpitchBytes, bool sameFill,
                      tjs_uint32 sameColor, bool diffFill,
                      tjs_uint32 diffColor) {
    if(fw != tw || fh != th)
        TVPThrowExceptionMessage(TJS_W("layerExSave: Different layer size."));
    const tjs_uint32 sameWord = tjsColorToMemoryWord(sameColor);
    const tjs_uint32 diffWord = tjsColorToMemoryWord(diffColor);
    tjs_int count = 0;
    for(tjs_int y = 0; y < fh; y++) {
        const tjs_uint8 *fp = from + static_cast<std::size_t>(y) * fpitchBytes;
        tjs_uint8 *tp = to + static_cast<std::size_t>(y) * tpitchBytes;
        for(tjs_int x = 0; x < fw; x++, fp += 4, tp += 4) {
            if(isSameColor(fp, tp)) {
                if(sameFill)
                    std::memcpy(tp, &sameWord, 4);
            } else {
                if(diffFill)
                    std::memcpy(tp, &diffWord, 4);
                count++;
            }
        }
    }
    return count;
}

void OozeColor(tjs_uint8 *pixels, tjs_int width, tjs_int height,
               tjs_int pitchBytes, tjs_int level, tjs_int threshold,
               tjs_uint32 fillColor) {
    if(level <= 0)
        TVPThrowExceptionMessage(TJS_W("layerExSave: Invalid level count."));
    tjs_uint8 th = static_cast<tjs_uint8>(threshold < 1 ? 1
                                                        : (threshold > 255
                                                               ? 255
                                                               : threshold));
    const tjs_uint8 fillR = static_cast<tjs_uint8>((fillColor >> 16) & 0xff);
    const tjs_uint8 fillG = static_cast<tjs_uint8>((fillColor >> 8) & 0xff);
    const tjs_uint8 fillB = static_cast<tjs_uint8>(fillColor & 0xff);

    // oozed 标记图：-1=已固定，1=本轮已处理，0=未处理（外圈留白）
    const tjs_int ow = width + 2, oh = height + 2;
    std::vector<signed char> oozed(static_cast<std::size_t>(ow) * oh, 0);
    signed char *otop = oozed.data() + ow + 1;

    for(tjs_int y = 0; y < height; y++) {
        signed char *o = otop + static_cast<std::ptrdiff_t>(y) * ow;
        tjs_uint8 *p = pixels + static_cast<std::size_t>(y) * pitchBytes;
        for(tjs_int x = 0; x < width; x++, o++, p += 4) {
            if(p[3] >= th) {
                *o = -1;
            } else {
                p[0] = fillR; // fork 内存序：p[0]=R, p[1]=G, p[2]=B
                p[1] = fillG;
                p[2] = fillB;
            }
        }
    }

    for(tjs_int round = 0; round < level; round++) {
        for(tjs_int y = 0; y < height; y++) {
            signed char *o = otop + static_cast<std::ptrdiff_t>(y) * ow;
            tjs_uint8 *p = pixels + static_cast<std::size_t>(y) * pitchBytes;
            for(tjs_int x = 0; x < width; x++, p += 4, o++) {
                if(*o)
                    continue;
                const bool up = o[-ow] < 0, down = o[ow] < 0;
                const bool left = o[-1] < 0, right = o[1] < 0;
                if(!up && !down && !left && !right)
                    continue;
                tjs_uint32 cr = 0, cg = 0, cb = 0;
                int cnt = 0;
                const auto addNeighbor = [&](tjs_int dy, tjs_int dx) {
                    const tjs_uint8 *q = p + static_cast<std::ptrdiff_t>(dy) *
                                                   pitchBytes +
                                         dx * 4;
                    cr += q[0]; // fork 内存序读取
                    cg += q[1];
                    cb += q[2];
                    cnt++;
                };
                if(up)
                    addNeighbor(-1, 0);
                if(down)
                    addNeighbor(1, 0);
                if(left)
                    addNeighbor(0, -1);
                if(right)
                    addNeighbor(0, 1);
                p[0] = static_cast<tjs_uint8>(cr / cnt);
                p[1] = static_cast<tjs_uint8>(cg / cnt);
                p[2] = static_cast<tjs_uint8>(cb / cnt);
                *o = 1;
            }
        }
        for(tjs_int y = 0; y < height; y++) {
            signed char *o = otop + static_cast<std::ptrdiff_t>(y) * ow;
            for(tjs_int x = 0; x < width; x++, o++)
                if(*o > 0)
                    *o = -1;
        }
    }
}

void CopyBlueToAlpha(tjs_uint8 *dst, tjs_int dw, tjs_int dh,
                     tjs_int dpitchBytes, const tjs_uint8 *src, tjs_int sw,
                     tjs_int sh, tjs_int spitchBytes) {
    const tjs_int w = sw < dw ? sw : dw;
    const tjs_int h = sh < dh ? sh : dh;
    for(tjs_int y = 0; y < h; y++) {
        const tjs_uint8 *p =
            src + static_cast<std::size_t>(y) * spitchBytes + 2; // B 通道
        tjs_uint8 *q =
            dst + static_cast<std::size_t>(y) * dpitchBytes + 3; // A 通道
        for(tjs_int x = 0; x < w; x++, p += 4, q += 4)
            *q = *p;
    }
}

bool IsBlank(const tjs_uint8 *pixels, tjs_int width, tjs_int height,
             tjs_int pitchBytes, tjs_int left, tjs_int top, tjs_int regionW,
             tjs_int regionH) {
    // 源范围裁剪（上游语义）
    if(left < 0) {
        regionW += left;
        left = 0;
    }
    if(top < 0) {
        regionH += top;
        top = 0;
    }
    tjs_int cut;
    if((cut = left + regionW - width) > 0)
        regionW -= cut;
    if((cut = top + regionH - height) > 0)
        regionH -= cut;

    if(regionW >= 0 && regionH >= 0) {
        for(tjs_int y = top; y < top + regionH; y++) {
            const tjs_uint8 *buffer =
                pixels + static_cast<std::size_t>(y) * pitchBytes +
                static_cast<std::size_t>(left) * 4;
            for(tjs_int x = 0; x < regionW; x++, buffer += 4)
                if(buffer[0] || buffer[1] || buffer[2] || buffer[3])
                    return false;
        }
    }
    return true;
}

void ClearAlpha(tjs_uint8 *pixels, tjs_int width, tjs_int height,
                tjs_int pitchBytes, tjs_int threshold, tjs_uint32 fillColor) {
    // fillColor 0x00RRGGBB → fork 内存序（alpha 字节为 0，上游语义）
    const tjs_uint32 word = tjsColorToMemoryWord(fillColor & 0xffffff);
    for(tjs_int y = 0; y < height; y++) {
        tjs_uint8 *q = pixels + static_cast<std::size_t>(y) * pitchBytes;
        for(tjs_int x = 0; x < width; x++, q += 4) {
            if(q[3] <= threshold)
                std::memcpy(q, &word, 4);
        }
    }
}

tjs_uint32 CalcAverageColor(const tjs_uint8 *pixels, tjs_int width,
                            tjs_int height, tjs_int pitchBytes, tjs_int left,
                            tjs_int top, tjs_int regionW, tjs_int regionH) {
    // 源范围裁剪（上游语义）；空区域由 TJS 壳抛异常
    if(left < 0) {
        regionW += left;
        left = 0;
    }
    if(top < 0) {
        regionH += top;
        top = 0;
    }
    tjs_int cut;
    if((cut = left + regionW - width) > 0)
        regionW -= cut;
    if((cut = top + regionH - height) > 0)
        regionH -= cut;
    if(regionW <= 0 || regionH <= 0)
        TVPThrowExceptionMessage(TJS_W("layerExSave: invalid layer range"));

    double a = 0, r = 0, g = 0, b = 0;
    const double size = static_cast<double>(regionW) * regionH;
    for(tjs_int y = top; y < top + regionH; y++) {
        const tjs_uint8 *buffer =
            pixels + static_cast<std::size_t>(y) * pitchBytes +
            static_cast<std::size_t>(left) * 4;
        for(tjs_int x = 0; x < regionW; x++, buffer += 4) {
            r += buffer[0]; // fork 内存序
            g += buffer[1];
            b += buffer[2];
            a += buffer[3];
        }
    }
    const tjs_uint32 ia = static_cast<tjs_uint32>(a / size) & 0xff;
    const tjs_uint32 ir = static_cast<tjs_uint32>(r / size) & 0xff;
    const tjs_uint32 ig = static_cast<tjs_uint32>(g / size) & 0xff;
    const tjs_uint32 ib = static_cast<tjs_uint32>(b / size) & 0xff;
    return (ia << 24) | (ir << 16) | (ig << 8) | ib;
}

tjs_uint64 FingerprintValue(const tjs_uint8 *pixels, tjs_int width,
                            tjs_int height, tjs_int pitchBytes,
                            bool ignoreTransparent) {
    constexpr tjs_uint64 kBasis = 0xCBF29CE484222325ull;
    constexpr tjs_uint64 kPrime = 0x00000100000001B3ull;
    const auto mix = [](tjs_uint64 hash, tjs_uint8 v) {
        return (hash ^ v) * kPrime;
    };

    tjs_uint64 hash = kBasis;
    for(tjs_int y = 0; y < height; y++) {
        const tjs_uint8 *p =
            pixels + static_cast<std::size_t>(y) * pitchBytes;
        for(tjs_int x = 0; x < width; x++, p += 4) {
            if(ignoreTransparent && !p[3]) {
                hash = mix(hash, 0);
                hash = mix(hash, 0);
                hash = mix(hash, 0);
                hash = mix(hash, 0);
            } else {
                // B,G,R,A 语义序喂入，与上游 Windows 输出一致
                hash = mix(hash, p[2]);
                hash = mix(hash, p[1]);
                hash = mix(hash, p[0]);
                hash = mix(hash, p[3]);
            }
        }
    }
    // 尺寸混入尾部（上游语义）
    for(int i = 24; i >= 0; i -= 8) {
        hash = mix(hash, static_cast<tjs_uint8>((width >> i) & 255));
        hash = mix(hash, static_cast<tjs_uint8>((height >> i) & 255));
    }
    return hash;
}

std::vector<tjs_uint32>
ShrinkVector(const tjs_uint8 *pixels, tjs_int width, tjs_int height,
             tjs_int pitchBytes, tjs_int shrinkW, tjs_int shrinkH) {
    // 上游对 svh==0 会除零（只挡负值），此处显式拒绝
    if(shrinkW <= 0 || shrinkH <= 0)
        TVPThrowExceptionMessage(TJS_W("layerExSave: invalid shrink size"));

    const tjs_int stepW = (width + shrinkW - 1) / shrinkW;
    const tjs_int stepH = (height + shrinkH - 1) / shrinkH;

    std::vector<tjs_uint32> out;
    for(tjs_int y = 0; y < height; y += stepH) {
        const tjs_int bh = (height - y) > stepH ? stepH : (height - y);
        for(tjs_int x = 0; x < width; x += stepW) {
            const tjs_int bw = (width - x) > stepW ? stepW : (width - x);
            tjs_uint32 s[4] = {0, 0, 0, 0}; // b,g,r,a 语义序累加
            tjs_uint32 total = 0;
            for(tjs_int yy = 0; yy < bh; yy++) {
                const tjs_uint8 *p =
                    pixels + static_cast<std::size_t>(y + yy) * pitchBytes +
                    static_cast<std::size_t>(x) * 4;
                for(tjs_int xx = 0; xx < bw; xx++, p += 4, total++) {
                    if(p[3]) {
                        s[0] += p[2]; // B（fork p[2]）
                        s[1] += p[1]; // G
                        s[2] += p[0]; // R（fork p[0]）
                        s[3] += p[3]; // A
                    }
                }
            }
            const tjs_uint32 bias = total >> 1;
            out.push_back(
                ((((s[0] + bias) / total) & 0xFF)) |
                ((((s[1] + bias) / total) & 0xFF) << 8) |
                ((((s[2] + bias) / total) & 0xFF) << 16) |
                ((((s[3] + bias) / total) & 0xFF) << 24));
        }
    }
    return out;
}

bool OctetVectorSum(const tjs_uint8 *a, std::size_t aLen, const tjs_uint8 *b,
                    std::size_t bLen, VectorSumWork &out) {
    if(!a)
        return false;
    if(!b) {
        for(std::size_t n = 0; n < aLen; n++) {
            const tjs_int64 va = a[n];
            out.dsum += va * va;
            out.nsum1 += va;
            if(a[n] != 0)
                ++out.diff;
        }
        out.psum1 = out.dsum;
        return true;
    }
    if(aLen != bLen)
        return false;
    for(std::size_t n = 0; n < aLen; n++) {
        const tjs_int64 va = a[n];
        const tjs_int64 vb = b[n];
        const tjs_int64 vd = va > vb ? va - vb : vb - va;
        out.csum += va * vb;
        out.dsum += vd * vd;
        out.nsum1 += va;
        out.nsum2 += vb;
        out.psum1 += va * va;
        out.psum2 += vb * vb;
        if(a[n] != b[n])
            ++out.diff;
    }
    return true;
}

} // namespace LayerExSave
