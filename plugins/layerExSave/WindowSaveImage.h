#ifndef YOGHOURT_LAYEREXSAVE_WINDOW_SAVE_IMAGE_H
#define YOGHOURT_LAYEREXSAVE_WINDOW_SAVE_IMAGE_H

// [krkr2] Window 异步保存扩展（移植自上游 Main.cpp 的 WindowSaveImage /
// SaveInfo，commit 83b0ef7）。
//
// 与上游的结构差异只有一处，但决定了整个线程模型：上游 worker 线程自己读
// Layer 的 TJS 属性、读 tags 字典、并直接写文件；本 fork 把这些 TJS 接触点
// 全部收回主线程（见 SaveSnapshot.h），worker 只做纯编码。事件回传按上游
// 语义走窗口消息：worker 用 TVPPostWindowMessage 投
// TVP_WM_USER+4（进度）/ +5（完成），本 fork 的窗口消息通道在主线程派发给
// 本文件注册的 receiver。
//
// 事件语义与上游一致：
//   - onSaveLayerImageProgress(handler, progress, layer, filename)：进度整
//     数变化时才发（上游同类去重）；
//   - onSaveLayerImageDone(handler, canceled, layer, filename)：cancel 后照
//     发（canceled=1），stop 后不发；
//   - handler 为窗口本地槽位（可复用空洞），脚本不应对数值做假设。

// 静态库保活锚点：由 Main.cpp 的 TVPLayerExSavePluginAnchor 引用。
void LayerExSaveWindowSaveImageAnchor();

#endif // YOGHOURT_LAYEREXSAVE_WINDOW_SAVE_IMAGE_H
