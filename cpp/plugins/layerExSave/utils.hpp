#ifndef _layerexsave_utils_hpp_
#define _layerexsave_utils_hpp_

// [krkr2] 本目录移植自 https://github.com/wtnbgo/layerExSave
// (83b0ef721656a89eb8fbda250d90eac073686db0)。与上游的差异：
//   - 不移植 Window.startSaveLayerImage/cancelSaveLayerImage/stopSaveLayerImage：
//     依赖 registerMessageReceiver + TVPPostWindowMessage，本 fork 的
//     TVPWindow::RegisterWindowMessageReceiver 是空实现，无主线程投递通道；
//   - 不移植 LicensesGen.cpp（本 fork 无 TVPRegisterLicense）；
//   - 缓冲获取经 PluginSafety 的 LayerReadView/LayerWriteView；
//   - mainImageBuffer 通道布局见 common/LayerPixel.h，上游按 Windows 的
//     BGRA 读写。凡读写 R/B 通道处标注 [krkr2]，对外结果
//     （文件字节、TJS 颜色值、哈希、octet）与上游 Windows 版一致。

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
