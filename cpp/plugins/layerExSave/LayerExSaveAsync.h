// layerExSave.dll 异步保存管理
//
// 对应上游 Main.cpp 的 WindowSaveImage / SaveInfo（Window.startSaveLayerImage
// / cancelSaveLayerImage / stopSaveLayerImage + onSaveLayerImageProgress /
// onSaveLayerImageDone 事件）。
//
// 与上游的架构差异（fork 适配，正常行为语义不变）：
//   - 上游用 detached std::thread + 自定义窗口消息（TVP_WM_USER+4/5）把
//     进度/完成事件送回主线程。本 fork 的窗口层（Kirikiroid2 血统）的
//     RegisterWindowMessageReceiver 是空实现，且 core 事件队列是单线程
//     事件泵假设（TVPPostEvent 无锁、IMMEDIATE 在调用线程直接派发），
//     工作线程不能触碰 TJS/core。因此改为无线程的主线程分片模型：
//     start 时在主线程一次性拷贝像素（Layer 视图不跨帧持有），编码器
//     按 step() 工作单元在 continuous handler（每 tick 派发）内推进，
//     每个 tick 有时间预算，进度/完成回调在主线程直接调用 window 方法。
//   - handler 为进程内全局递增整数；上游是 per-window 索引（可复用空洞），
//     脚本不应对 handler 数值本身做假设，取消/中止按返回的 handler 传递。
//   - 取消（cancel）在下一次 tick 生效：停止编码、不写文件、
//     onSaveLayerImageDone(canceled=1) 照发（与上游一致）；
//     中止（stop）立即移除任务且不发完成事件（与上游一致）。
//   - 编码或写文件异常：清理该任务并记录日志，异常继续传播给事件泵；
//     其余任务不受影响。上游在线程内抛异常会直接崩溃。

#ifndef YOGHOURT_LAYEREXSAVE_ASYNC_H
#define YOGHOURT_LAYEREXSAVE_ASYNC_H

#include "LayerExSaveEncoders.h"

#include <map>
#include <memory>
#include <vector>

namespace LayerExSave {

class SaveManager {
public:
    static SaveManager &Instance();

    // 启动一个异步保存。pixels 为已拷贝的紧凑像素缓冲（主线程完成拷贝），
    // source 指向该缓冲，encoder 由调用方按扩展名创建。返回全局递增的
    // handler。本函数与后续 Tick 都只在主线程调用。
    tjs_int64 Start(iTJSDispatch2 *window, iTJSDispatch2 *layer,
                    const ttstr &filename, std::vector<tjs_uint8> &&pixels,
                    const ImageSource &source,
                    std::unique_ptr<ChunkedEncoder> encoder);

    void Cancel(tjs_int64 handler);
    void Stop(tjs_int64 handler);

    // continuous handler 驱动；主线程专用。
    void Tick();

private:
    SaveManager() = default;

    struct Job {
        tjs_int64 handler = 0;
        tTJSVariant window; // 事件目标（CopyRef）
        tTJSVariant layer;  // 事件参数（CopyRef）
        ttstr filename;
        std::vector<tjs_uint8> pixels; // 像素拷贝（source 指向它）
        ImageSource source;
        std::unique_ptr<ChunkedEncoder> encoder;
        bool cancelRequested = false;
        int lastProgress = -1;
    };

    void EnsureHook();
    void ReleaseHookIfIdle();
    void FireProgress(const Job &job, int percent);
    void FireDone(const Job &job, bool canceled);
    static void WriteFile(const Job &job);

    std::map<tjs_int64, std::unique_ptr<Job>> jobs_;
    tjs_int64 nextHandler_ = 1;
    class TickDispatch *tickDispatch_ = nullptr;
    bool hookRegistered_ = false;
};

} // namespace LayerExSave

#endif
