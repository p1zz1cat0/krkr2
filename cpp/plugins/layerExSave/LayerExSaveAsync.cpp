// layerExSave.dll 异步保存管理实现
// 头文件记录与上游的架构差异和线程约定，此处不重复。

#include "LayerExSaveAsync.h"

#include "EventIntf.h"
#include "MsgIntf.h"
#include "StorageIntf.h"
#include "WindowIntf.h"

#include <spdlog/spdlog.h>

#include <chrono>

// 上游为 Windows 调用约定声明；本工程无此宏时保持为空
#ifndef TJS_INTF_METHOD
#define TJS_INTF_METHOD
#endif

namespace LayerExSave {

namespace {
constexpr std::chrono::milliseconds kTickBudget{4};

void LogPluginError(const char *msg) {
    if(auto logger = spdlog::get("plugin"))
        logger->error("[layerExSave] {}", msg);
}

} // namespace

// 每 tick 派发的 native 闭包，转发给 SaveManager。与头文件的前向声明
// 同名（LayerExSave::TickDispatch），不能放进匿名 namespace。
class TickDispatch final : public tTJSDispatch {
    tjs_error TJS_INTF_METHOD
    FuncCall(tjs_uint32 flag, const tjs_char *membername, tjs_uint32 *hint,
             tTJSVariant *result, tjs_int numparams, tTJSVariant **param,
             iTJSDispatch2 *objthis) override {
        (void)flag;
        (void)membername;
        (void)hint;
        (void)result;
        (void)numparams;
        (void)param;
        (void)objthis;
        SaveManager::Instance().Tick();
        return TJS_S_OK;
    }
};

namespace {

// 主线程直接调用 window 上的 onSaveLayerImageProgress / onSaveLayerImageDone。
// 成员缺失等 FuncCall 失败与上游一致静默（记日志），handler 内部抛出的
// TJS 异常向上传播（事件泵捕获）。window 的 native instance 在引擎
// teardown / 窗口关闭时先行销毁而 TJS 对象仍存活，派发前检查
// CanDeliverEvents，避免在已失效的 window 上触发成员访问。
void CallWindowMethod(iTJSDispatch2 *window, const tjs_char *name,
                      tTJSVariant *args, tjs_int argc) {
    tTJSNI_BaseWindow *native = nullptr;
    if(TJS_FAILED(window->NativeInstanceSupport(
           TJS_NIS_GETINSTANCE, tTJSNC_Window::ClassID,
           (iTJSNativeInstance **)&native)) ||
       !native || !native->CanDeliverEvents())
        return;
    tTJSVariant *params[4] = {&args[0], &args[1], &args[2], &args[3]};
    tTJSVariant result;
    if(TJS_FAILED(window->FuncCall(0, name, nullptr, &result, argc, params,
                                   window)))
        LogPluginError("window event callback failed");
}

} // namespace

SaveManager &SaveManager::Instance() {
    static SaveManager instance;
    return instance;
}

tjs_int64 SaveManager::Start(iTJSDispatch2 *window, iTJSDispatch2 *layer,
                             const ttstr &filename,
                             std::vector<tjs_uint8> &&pixels,
                             const ImageSource &source,
                             std::unique_ptr<ChunkedEncoder> encoder) {
    auto job = std::make_unique<Job>();
    job->handler = nextHandler_++;
    job->window = tTJSVariant(window, window);
    if(layer)
        job->layer = tTJSVariant(layer, layer);
    job->filename = filename;
    job->pixels = std::move(pixels);
    job->source = source; // pixels 已 move，buffer 地址不变
    job->source.pixels = job->pixels.data();
    job->encoder = std::move(encoder);

    const tjs_int64 handler = job->handler;
    EnsureHook();
    jobs_.emplace(handler, std::move(job));
    return handler;
}

void SaveManager::Cancel(tjs_int64 handler) {
    const auto it = jobs_.find(handler);
    if(it != jobs_.end())
        it->second->cancelRequested = true;
}

void SaveManager::Stop(tjs_int64 handler) { jobs_.erase(handler); }

void SaveManager::Tick() {
    const auto deadline = std::chrono::steady_clock::now() + kTickBudget;
    for(auto it = jobs_.begin(); it != jobs_.end();) {
        Job &job = *it->second;
        if(job.cancelRequested) {
            // 取消：不写文件，done(canceled=1)（上游语义）
            FireDone(job, true);
            it = jobs_.erase(it);
            continue;
        }
        try {
            while(!job.encoder->finished()) {
                job.encoder->step();
                if(std::chrono::steady_clock::now() >= deadline)
                    break;
            }
        } catch(...) {
            LogPluginError("encoding failed; job aborted");
            it = jobs_.erase(it);
            throw;
        }
        const int percent = job.encoder->progressPercent();
        if(percent != job.lastProgress) {
            job.lastProgress = percent;
            FireProgress(job, percent);
        }
        if(job.encoder->finished()) {
            try {
                WriteFile(job);
            } catch(...) {
                LogPluginError("writing saved file failed; job aborted");
                it = jobs_.erase(it);
                throw;
            }
            FireDone(job, false);
            it = jobs_.erase(it);
        } else {
            ++it;
        }
    }
    ReleaseHookIfIdle();
}

void SaveManager::EnsureHook() {
    if(hookRegistered_)
        return;
    if(!tickDispatch_)
        tickDispatch_ = new TickDispatch();
    tTJSVariantClosure clo(tickDispatch_, nullptr);
    TVPAddContinuousHandler(clo);
    hookRegistered_ = true;
}

void SaveManager::ReleaseHookIfIdle() {
    if(!hookRegistered_ || !jobs_.empty())
        return;
    tTJSVariantClosure clo(tickDispatch_, nullptr);
    TVPRemoveContinuousHandler(clo);
    hookRegistered_ = false;
}

void SaveManager::FireProgress(const Job &job, int percent) {
    iTJSDispatch2 *window = job.window.AsObjectNoAddRef();
    if(!window)
        return;
    tTJSVariant args[4] = {tTJSVariant(job.handler),
                           tTJSVariant(static_cast<tjs_int>(percent)),
                           job.layer, job.filename};
    CallWindowMethod(window, TJS_W("onSaveLayerImageProgress"), args, 4);
}

void SaveManager::FireDone(const Job &job, bool canceled) {
    iTJSDispatch2 *window = job.window.AsObjectNoAddRef();
    if(!window)
        return;
    tTJSVariant args[4] = {tTJSVariant(job.handler),
                           tTJSVariant(static_cast<tjs_int>(canceled ? 1 : 0)),
                           job.layer, job.filename};
    CallWindowMethod(window, TJS_W("onSaveLayerImageDone"), args, 4);
}

void SaveManager::WriteFile(const Job &job) {
    const std::vector<tjs_uint8> &data = job.encoder->data();
    // 本树 tTJSBinaryStream 是普通 C++ 类（虚析构），用 delete 替代上游
    // 插件的 out->Destruct()
    tTJSBinaryStream *out = TVPCreateStream(job.filename, TJS_BS_WRITE);
    if(!out)
        TVPThrowExceptionMessage(TJS_W("%1:can't open"), job.filename);
    try {
        out->Write(data.data(), static_cast<tjs_uint>(data.size()));
    } catch(...) {
        delete out;
        throw;
    }
    delete out;
}

} // namespace LayerExSave
