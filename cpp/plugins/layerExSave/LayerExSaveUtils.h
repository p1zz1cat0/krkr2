// layerExSave.dll Layer 杂项函数的算法核心
//
// 移植自上游 wtnbgo/layerExSave 的 utils.cpp（getCropRect / getCropRectZero
// / getDiffRect / getDiffPixel / oozeColor / copyBlueToAlpha / isBlank /
// clearAlpha / getAverageColor / getFingerPrintValue / getShrinkVectorOctet
// / Math.octetVectorSum）。
//
// 全部函数接收原始缓冲 + 已校验几何（几何校验与 TJS 视图获取在
// LayerExSaveModule.cpp 完成），便于单元测试在无 TJS 环境下构造。
//
// 通道序（fork 的 mainImageBuffer 内存序 A<<24|B<<16|G<<8|R，低字节 R）：
//   - alpha 恒为 p[3]，颜色比较/裁剪逻辑与通道语义无关，照抄上游；
//   - 颜色写入（getDiffPixel 的填充色、clearAlpha 的 fillColor）按
//     TJS 0xAARRGGBB 语义换算成 fork 内存序（r,g,b,a）；
//   - getFingerPrintValue / getShrinkVectorOctet 的输出依赖通道喂入序，
//     按 B,G,R,A 语义序处理（p[2],p[1],p[0],p[3]），与上游 Windows 版
//     对同一图像产生相同数值。

#ifndef YOGHOURT_LAYEREXSAVE_UTILS_H
#define YOGHOURT_LAYEREXSAVE_UTILS_H

#include "tjs.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace LayerExSave {

// %[x,y,w,h] 裁剪/差分矩形；hasCrop=false 表示全部透明或完全相同。
struct CropResult {
    bool hasCrop = false;
    tjs_int x = 0, y = 0, w = 0, h = 0;
};

// getCropRect / getCropRectZero。zeroMode=true 时只认 0x00000000 为透明。
CropResult CalcCropRect(const tjs_uint8 *pixels, tjs_int width, tjs_int height,
                        tjs_int pitchBytes, bool zeroMode);

// getDiffRect。尺寸不一致返回 false（TJS 层转异常）。
bool CalcDiffRect(const tjs_uint8 *from, tjs_int fw, tjs_int fh,
                  tjs_int fpitchBytes, const tjs_uint8 *to, tjs_int tw,
                  tjs_int th, tjs_int tpitchBytes, CropResult &out);

// getDiffPixel。sameFill/diffFill 指示对应色是否填充；颜色为 TJS
// 0xAARRGGBB（内部换算 fork 内存序）。返回差分像素数。
tjs_int CalcDiffPixel(const tjs_uint8 *from, tjs_int fw, tjs_int fh,
                      tjs_int fpitchBytes, tjs_uint8 *to, tjs_int tw,
                      tjs_int th, tjs_int tpitchBytes, bool sameFill,
                      tjs_uint32 sameColor, bool diffFill,
                      tjs_uint32 diffColor);

// oozeColor：把边缘颜色向低 alpha 区域延伸 level 轮。fillColor 为
// 0xRRGGBB（低 alpha 像素先填此色）。
void OozeColor(tjs_uint8 *pixels, tjs_int width, tjs_int height,
               tjs_int pitchBytes, tjs_int level, tjs_int threshold,
               tjs_uint32 fillColor);

// copyBlueToAlpha：src 的 B 通道（fork p[2]）复制到 dst 的 alpha。
void CopyBlueToAlpha(tjs_uint8 *dst, tjs_int dw, tjs_int dh,
                     tjs_int dpitchBytes, const tjs_uint8 *src, tjs_int sw,
                     tjs_int sh, tjs_int spitchBytes);

// isBlank：指定区域（先按源范围裁剪）是否全为 0x00000000。
bool IsBlank(const tjs_uint8 *pixels, tjs_int width, tjs_int height,
             tjs_int pitchBytes, tjs_int left, tjs_int top, tjs_int regionW,
             tjs_int regionH);

// clearAlpha：alpha <= threshold 的像素整字填 fillColor（0x00RRGGBB，
// alpha 字节为 0，上游语义）。
void ClearAlpha(tjs_uint8 *pixels, tjs_int width, tjs_int height,
                tjs_int pitchBytes, tjs_int threshold, tjs_uint32 fillColor);

// getAverageColor：区域平均色，返回 0xAARRGGBB（TJS 语义，已含 alpha）。
tjs_uint32 CalcAverageColor(const tjs_uint8 *pixels, tjs_int width,
                            tjs_int height, tjs_int pitchBytes, tjs_int left,
                            tjs_int top, tjs_int regionW, tjs_int regionH);

// getFingerPrintValue：FNV-1a 64bit，通道按 B,G,R,A 语义序喂入，
// 尺寸混入尾部。
tjs_uint64 FingerprintValue(const tjs_uint8 *pixels, tjs_int width,
                            tjs_int height, tjs_int pitchBytes,
                            bool ignoreTransparent);

// getShrinkVectorOctet：块平均向量，每元素按 (b,g,r,a) 低字节起打包
// （与上游 Windows 输出一致）。
std::vector<tjs_uint32>
ShrinkVector(const tjs_uint8 *pixels, tjs_int width, tjs_int height,
             tjs_int pitchBytes, tjs_int shrinkW, tjs_int shrinkH);

// Math.octetVectorSum。b 为空（blen==0）时按单向量为工作。
struct VectorSumWork {
    tjs_int64 csum = 0;  // Σ(a*b)
    tjs_int64 dsum = 0;  // Σ(a-b)^2（单向量为 Σa^2）
    tjs_int64 psum1 = 0; // Σa^2
    tjs_int64 psum2 = 0; // Σb^2（单向量为 0）
    tjs_int64 nsum1 = 0; // Σa
    tjs_int64 nsum2 = 0; // Σb（单向量为 0）
    tjs_uint64 diff = 0; // a!=b 的元素数（单向量为非零元素数）
};
bool OctetVectorSum(const tjs_uint8 *a, std::size_t aLen, const tjs_uint8 *b,
                    std::size_t bLen, VectorSumWork &out);

} // namespace LayerExSave

#endif
