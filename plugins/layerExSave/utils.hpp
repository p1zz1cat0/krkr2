#ifndef _layerexsave_utils_hpp_
#define _layerexsave_utils_hpp_

// [krkr2] 本目录移植自 https://github.com/wtnbgo/layerExSave
// (83b0ef721656a89eb8fbda250d90eac073686db0)。与上游的差异：
//   - Window 的异步保存（startSaveLayerImage 系列）在 WindowSaveImage.cpp：
//     上游 worker 直接读 TJS 并写文件，本 fork 把这些收回主线程，worker 只
//     编码（见 SaveSnapshot.h），事件经窗口消息通道回主线程；
//   - 不移植 LicensesGen.cpp（本 fork 无 TVPRegisterLicense）；
//   - 缓冲获取经 PluginSafety 的 LayerReadView/LayerWriteView；
//   - mainImageBuffer 通道布局见 common/LayerPixel.h，上游按 Windows 的
//     BGRA 读写。凡读写 R/B 通道处标注 [krkr2]，对外结果
//     （文件字节、TJS 颜色值、哈希、octet）与上游 Windows 版一致。

#include "ncbind.hpp" // [krkr2] iTJSDispatch2/ttstr：本头被 compress.hpp 直接引用

#ifndef NCB_MODULE_NAME
#define NCB_MODULE_NAME TJS_W("layerExSave.dll")
#endif
#ifndef TJS_INTF_METHOD
#define TJS_INTF_METHOD
#endif

// [krkr2] 通道偏移与颜色值互转见 common/LayerPixel.h
#include "common/LayerPixel.h"

// バッファ参照用の型
typedef unsigned char const *BufRefT;
typedef unsigned char       *WrtRefT;

bool GetLayerSize(iTJSDispatch2 *lay, long &w, long &h, long *pitch=0);
bool GetLayerBufferAndSize(iTJSDispatch2 *lay, long &w, long &h, BufRefT &ptr, long &pitch);
bool GetLayerBufferAndSize(iTJSDispatch2 *lay, long &w, long &h, WrtRefT &ptr, long &pitch);

bool GetProvinceBufferAndSize(iTJSDispatch2 *lay, long &w, long &h, BufRefT &ptr, long &pitch);

#endif
