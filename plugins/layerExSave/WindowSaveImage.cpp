// [krkr2] Window 异步保存扩展的实现。上游对应物是 Main.cpp 的
// WindowSaveImage / SaveInfo（commit 83b0ef7）；本 fork 的差异见
// WindowSaveImage.h 顶部说明。
//
// 线程模型：
//   start（主线程）   校验、拷贝像素与 tags、占槽并起 worker
//   worker            只做纯编码，进度经 TVPPostWindowMessage 投递
//   receiver（主线程）发 TJS 事件；完成时写文件并回收槽位

#include "compress.hpp" // [krkr2] 同期経路と共用するエンコーダ
#include "ncbind.hpp"
#include "savepng.hpp"
#include "savetlg5.hpp"
#include "SaveSnapshot.h"
#include "WindowSaveImage.h"
#include "utils.hpp"

#include "WindowIntf.h" // TVPPostWindowMessage

#include <spdlog/spdlog.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

// TVP_WM_USER は全バリアント共通のユーザメッセージ基点 (0x8000)。
// このモジュールは registerMessageReceiver で登録した receiver へ
// TVPPostWindowMessage 経由で配信する。
#define WM_SAVE_TLG_PROGRESS (TVP_WM_USER + 4)
#define WM_SAVE_TLG_DONE (TVP_WM_USER + 5)

// receiver の呼び出し規約は tTVPWindowMessageReceiver に合わせる
// (WIN 版=__stdcall / Generic 版=規約指定なし。x86 ビルドでのみ実差が出る)
#ifdef __WINVER__
#define RECEIVER_CALLCONV STDCALL
#else
#define RECEIVER_CALLCONV
#endif

namespace {

void LogWindowSaveError(const char *what) {
    if(auto logger = spdlog::get("plugin"))
        logger->error("[layerExSave] {}", what);
}

//---------------------------------------------------------------------------
// 実行中ジョブ
//---------------------------------------------------------------------------
// 参照カウントで持つ：worker が自分のコピーを持っている間に完了イベントで
// スロットを空けても、worker が走っている限り実体は生き残る。
class SaveJob;

// エンコード本体（定義は SaveJob の後ろ）
void RunSaveJobImpl(SaveJob &job);

class SaveJob {
public:
    SaveJob(int handler, SaveSnapshot snapshot, ttstr filename,
            tTJSVariant layer) :
        handler_(handler), snapshot_(std::move(snapshot)),
        filename_(std::move(filename)), layer_(layer) {}

    [[nodiscard]] int Handler() const { return handler_; }
    [[nodiscard]] const SaveSnapshot &Snapshot() const { return snapshot_; }
    [[nodiscard]] const ttstr &Filename() const { return filename_; }
    [[nodiscard]] tTJSVariant LayerVariant() const { return layer_; }
    [[nodiscard]] tTJSVariant FilenameVariant() const {
        return tTJSVariant(filename_);
    }
    [[nodiscard]] int LastPercent() const { return lastPercent_.load(); }
    [[nodiscard]] bool IsCanceled() const { return canceled_.load(); }

    void Cancel() { canceled_.store(true); }

    // worker：進捗（整数が変化したときだけ通知）
    bool ReportProgress(int percent) {
        if(lastPercent_.exchange(percent) == percent)
            return canceled_.load();
        TVPPostWindowMessage(window_, WM_SAVE_TLG_PROGRESS,
                             static_cast<tjs_uint64>(
                                 reinterpret_cast<tjs_intptr_t>(this)),
                             0);
        return canceled_.load();
    }

    // worker：エンコード、ファイル書き出し、終了通知。例外は投げない
    // （worker を抜けると std::terminate になるため、失敗は canceled として
    // 完了イベントで伝える）。
    void Run(tTJSNI_Window *window) {
        window_ = window;
        try {
            RunSaveJobImpl(*this);
        } catch(...) {
            LogWindowSaveError("asynchronous save failed; reported as canceled");
            canceled_.store(true);
        }
        TVPPostWindowMessage(window_, WM_SAVE_TLG_DONE,
                             static_cast<tjs_uint64>(
                                 reinterpret_cast<tjs_intptr_t>(this)),
                             0);
    }

private:
    int handler_;
    SaveSnapshot snapshot_;
    ttstr filename_;
    tTJSVariant layer_;               //< TJS イベント引数（主线程でコピー）
    tTJSNI_Window *window_ = nullptr; //< 投递先（HWND = Window インスタンス）
    std::atomic<bool> canceled_{false};
    std::atomic<int> lastPercent_{-1};
};

// 進捗コールバック（worker 线程から呼ばれる）
bool SnapshotProgress(int percent, void *userdata) {
    return static_cast<SaveJob *>(userdata)->ReportProgress(percent);
}

// エンコード＋ファイル書き出し（worker 线程）。
// 同期経路と同じ「拡張子で PNG / TLG5 を選ぶ」規則。
void RunSaveJobImpl(SaveJob &job) {
    const SaveSnapshot &snapshot = job.Snapshot();
    const ttstr &filename = job.Filename();
    ttstr ext(TVPExtractStorageExt(filename));
    ext.ToLowerCase();

    if(ext == TJS_W(".png")) {
        CompressPNG work(SnapshotProgress, &job);
        if(!work.compress(snapshot.width, snapshot.height,
                          snapshot.buffer.data(), snapshot.pitchBytes,
                          snapshot.tags))
            work.writeTo(filename.c_str());
    } else {
        CompressTLG5 work(SnapshotProgress, &job);
        if(!work.compress(snapshot.width, snapshot.height,
                          snapshot.buffer.data(), snapshot.pitchBytes,
                          snapshot.tags))
            work.writeTo(filename.c_str());
    }
}

//---------------------------------------------------------------------------
// ウィンドウごとの状態
//---------------------------------------------------------------------------
// クラス登録（ncbInstanceAdaptor）ではなく objthis で引く表に持つ：3 メソッド
// は関数登録なので objthis がそのまま渡ってくる。状態と登録はすべて主线程。
struct WindowState {
    iTJSDispatch2 *window = nullptr;
    tTJSVariant windowVariant;
    std::vector<std::shared_ptr<SaveJob>> jobs; //< 槽位（nullptr = 空き）
    bool registered = false;
};

std::vector<std::unique_ptr<WindowState>> g_states;

WindowState *FindState(iTJSDispatch2 *objthis) {
    for(auto &state : g_states) {
        if(state->window == objthis)
            return state.get();
    }
    return nullptr;
}

tTJSNI_Window *NativeWindowOf(void *handle) {
    if(!handle)
        return nullptr;
    return static_cast<tTJSNI_Window *>(handle);
}

tTJSNI_Window *NativeWindowOf(iTJSDispatch2 *objthis) {
    tTJSNI_Window *window = nullptr;
    if(TJS_FAILED(objthis->NativeInstanceSupport(
           TJS_NIS_GETINSTANCE, tTJSNC_Window::ClassID,
           (iTJSNativeInstance **)&window)))
        return nullptr;
    return window;
}

bool RECEIVER_CALLCONV Receiver(void *userdata, tTVPWindowMessage *msg);

void CallRegisterMessageReceiver(iTJSDispatch2 *objthis, tTVPWMRRegMode mode,
                                 tTJSNI_Window *window) {
    tTJSVariant modeArg = (tTVInteger)(tjs_int)mode;
    tTJSVariant procArg = (tTVInteger)(tjs_intptr_t)Receiver;
    tTJSVariant userdataArg = (tTVInteger)(tjs_intptr_t)window;
    tTJSVariant *p[] = {&modeArg, &procArg, &userdataArg};
    if(objthis->FuncCall(0, TJS_W("registerMessageReceiver"), NULL, NULL, 3, p,
                         objthis) != TJS_S_OK) {
        TVPThrowExceptionMessage(TJS_W("can't regist user message receiver"));
    }
}

void FireEvent(WindowState *state, SaveJob *job, bool done) {
    iTJSDispatch2 *window = state->windowVariant.AsObjectThisNoAddRef();
    if(!window)
        return;

    tTJSVariant handler = job->Handler();
    tTJSVariant layer = job->LayerVariant();
    tTJSVariant filename = job->FilenameVariant();

    if(done) {
        tTJSVariant canceled = job->IsCanceled() ? 1 : 0;
        tTJSVariant *vars[] = {&handler, &canceled, &layer, &filename};
        window->FuncCall(0, TJS_W("onSaveLayerImageDone"), NULL, NULL, 4, vars,
                         window);
    } else {
        tTJSVariant progress = job->LastPercent();
        tTJSVariant *vars[] = {&handler, &progress, &layer, &filename};
        window->FuncCall(0, TJS_W("onSaveLayerImageProgress"), NULL, NULL, 4,
                         vars, window);
    }
}

// ウィンドウイベント処理レシーバ（主线程）
bool RECEIVER_CALLCONV Receiver(void *userdata, tTVPWindowMessage *msg) {
    if(msg->Msg != WM_SAVE_TLG_PROGRESS && msg->Msg != WM_SAVE_TLG_DONE)
        return false;

    // userdata は登録時に渡したウィンドウハンドル（Window.HWND）。表から
    // そのウィンドウの状態を探す：登録が解除済みなら見つからず、そのまま
    // メッセージを捨てる。
    WindowState *state = nullptr;
    for(auto &entry : g_states) {
        if(NativeWindowOf(entry->window) == userdata) {
            state = entry.get();
            break;
        }
    }
    auto *job =
        reinterpret_cast<SaveJob *>(static_cast<tjs_intptr_t>(msg->WParam));
    if(!state || !job)
        return true;

    const bool done = msg->Msg == WM_SAVE_TLG_DONE;
    FireEvent(state, job, done);

    if(done) {
        for(auto &slot : state->jobs) {
            if(slot.get() == job) {
                slot = nullptr;
                break;
            }
        }
    }
    return true;
}

// 空きスロットしか無くなったウィンドウの登録を解除して表から外す
void ReleaseStateIfIdle(WindowState *state) {
    for(auto &slot : state->jobs) {
        if(slot)
            return;
    }
    if(state->registered) {
        if(auto *window = NativeWindowOf(state->window)) {
            CallRegisterMessageReceiver(state->window, wrmUnregister, window);
        }
        state->registered = false;
    }
    for(auto it = g_states.begin(); it != g_states.end(); ++it) {
        if(it->get() == state) {
            g_states.erase(it);
            return;
        }
    }
}

//---------------------------------------------------------------------------
// TJS メソッド
//---------------------------------------------------------------------------
WindowState *EnsureState(iTJSDispatch2 *objthis) {
    WindowState *state = FindState(objthis);
    tTJSNI_Window *window = NativeWindowOf(objthis);
    if(!window)
        TVPThrowExceptionMessage(TJS_W("layerExSave.dll requires a Window."));

    if(!state) {
        g_states.push_back(std::make_unique<WindowState>());
        state = g_states.back().get();
        state->window = objthis;
        state->windowVariant = tTJSVariant(objthis, objthis);
    }
    if(!state->registered) {
        CallRegisterMessageReceiver(objthis, wrmRegister, window);
        state->registered = true;
    }
    return state;
}

int StartSave(iTJSDispatch2 *objthis, tTJSVariant layer,
              const tjs_char *filename, tTJSVariant info) {
    WindowState *state = EnsureState(objthis);
    tTJSNI_Window *window = NativeWindowOf(objthis);

    int handler = static_cast<int>(state->jobs.size());
    for(size_t i = 0; i < state->jobs.size(); i++) {
        if(!state->jobs[i]) {
            handler = static_cast<int>(i);
            break;
        }
    }
    if(handler >= static_cast<int>(state->jobs.size()))
        state->jobs.resize(handler + 1);

    // [krkr2] ここがフォークの核心：像素と tags を主线程でスナップショット
    SaveSnapshot snapshot;
    if(!CaptureSaveSnapshot(layer.AsObjectNoAddRef(),
                            info.Type() == tvtObject ? info.AsObjectNoAddRef()
                                                     : nullptr,
                            snapshot)) {
        TVPThrowExceptionMessage(TJS_W("%1:invalid layer"), filename);
    }

    auto job = std::make_shared<SaveJob>(handler, std::move(snapshot),
                                         ttstr(filename), layer);
    state->jobs[handler] = job;
    std::thread([job, window]() { job->Run(window); }).detach();
    return handler;
}

void CancelSave(iTJSDispatch2 *objthis, int handler) {
    WindowState *state = FindState(objthis);
    if(!state || handler < 0 || handler >= static_cast<int>(state->jobs.size()))
        return;
    if(state->jobs[handler])
        state->jobs[handler]->Cancel();
}

void StopSave(iTJSDispatch2 *objthis, int handler) {
    WindowState *state = FindState(objthis);
    if(!state || handler < 0 || handler >= static_cast<int>(state->jobs.size()))
        return;
    // 完了イベントは来ない（上游 stop と同じ）
    state->jobs[handler] = nullptr;
    ReleaseStateIfIdle(state);
}

} // namespace

//---------------------------------------------------------------------------
// 関数登録（NCB_ATTACH_FUNCTION は objthis を末尾引数で渡す）
//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD StartSaveLayerImage(tTJSVariant *result,
                                              tjs_int numparams,
                                              tTJSVariant **param,
                                              iTJSDispatch2 *objthis) {
    if(numparams < 2)
        return TJS_E_BADPARAMCOUNT;
    tTJSVariant info = numparams > 2 ? *param[2] : tTJSVariant();
    const int handler =
        StartSave(objthis, *param[0], param[1]->GetString(), info);
    if(result)
        *result = handler;
    return TJS_S_OK;
}

tjs_error TJS_INTF_METHOD CancelSaveLayerImage(tTJSVariant *result,
                                               tjs_int numparams,
                                               tTJSVariant **param,
                                               iTJSDispatch2 *objthis) {
    if(numparams < 1)
        return TJS_E_BADPARAMCOUNT;
    CancelSave(objthis, (int)param[0]->AsInteger());
    return TJS_S_OK;
}

tjs_error TJS_INTF_METHOD StopSaveLayerImage(tTJSVariant *result,
                                             tjs_int numparams,
                                             tTJSVariant **param,
                                             iTJSDispatch2 *objthis) {
    if(numparams < 1)
        return TJS_E_BADPARAMCOUNT;
    StopSave(objthis, (int)param[0]->AsInteger());
    return TJS_S_OK;
}

// Window クラスは pre-regist 時点でまだ存在しないため、ここで解決しておく。
void EnsureWindowClassRegistered_() {
    tTJSVariant windowClass;
    TVPExecuteExpression(TJS_W("Window"), &windowClass);
    if(windowClass.Type() != tvtObject)
        TVPThrowExceptionMessage(
            TJS_W("layerExSave.dll requires the Window class."));
}
NCB_PRE_REGIST_CALLBACK(EnsureWindowClassRegistered_);

NCB_ATTACH_FUNCTION(startSaveLayerImage, Window, StartSaveLayerImage);
NCB_ATTACH_FUNCTION(cancelSaveLayerImage, Window, CancelSaveLayerImage);
NCB_ATTACH_FUNCTION(stopSaveLayerImage, Window, StopSaveLayerImage);

void LayerExSaveWindowSaveImageAnchor() {}
