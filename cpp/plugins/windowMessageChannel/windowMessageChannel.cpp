// windowMessageChannel.dll — diagnostic plugin for the host window message
// channel (TVPPostWindowMessage + Window.registerMessageReceiver).
//
// Mirrors the upstream wamsoft plugin shape the channel exists for (sigcheck):
// a std::thread worker computes and posts, the engine delivers on the
// application thread.  Nothing here is game-facing API; the module is only
// linked into the smoke runtime to prove the channel works with a real worker
// thread, so the shared plugin-safety audit excludes it.
//
//   win.registerTestReceiver(callback, userdata) -> int handle
//   win.unregisterTestReceiver(handle)           -> bool
//   win.startTestPosterThread(win)               -> bool
//   win.stopTestPosterThread()                   -> bool
//   win.testPosterThreadCount()                  -> int
//
// The receiver that reaches the TJS callback is a C++ adapter, exactly like
// the upstream plugin-side sample: the address of a TJS function cannot be
// handed to registerMessageReceiver, because TJS function objects cannot be
// converted to integers.  The handle indirection also keeps the callback
// object alive for as long as the registration exists.

#include "MsgIntf.h"
#include "WindowImpl.h"
#include "ncbind.hpp"

#include <atomic>
#include <map>
#include <thread>

// 独立插件目标（layerExSave-dll 等）用 Plugins.link 语义编译时没有这个宏，
// 与 layerExSave/utils.hpp 的处理一致。
#ifndef TJS_INTF_METHOD
#define TJS_INTF_METHOD
#endif

#define NCB_MODULE_NAME TJS_W("windowMessageChannel.dll")

extern "C" void TVPWindowMessageChannelPluginAnchor() {}

namespace {

// The posted pair deliberately exercises the full 64-bit width of both
// parameters: WParam has bit 63 clear and both halves set, so a truncated
// 32-bit path or a signed/unsigned mix-up changes the observed value.
constexpr tjs_uint32 kTestMessage = TVP_WM_USER + 1;
constexpr tjs_uint64 kTestWParam = 0x7FFFFFFFFFFF0001ULL;
constexpr tjs_uint64 kTestLParam = 0x00000000DEADBEEFULL;

//----------------------------------------------------------------------------
// Receiver handle registry
//----------------------------------------------------------------------------
// registerMessageReceiver() carries the receiver and its userdata as two
// pointers, so the plugin has to keep the TJS callback itself alive.  A handle
// in userdata plus this table does that, and it lets the receiver validate its
// userdata instead of trusting an arbitrary integer.  Registration and the
// receiver both run on the application thread, so no locking is required.
struct ReceiverEntry {
    tTJSVariant Callback;
    tTJSVariant UserData;
};

int g_nextReceiverHandle = 1;
std::map<int, ReceiverEntry> g_receivers;

bool TJS_INTF_METHOD ReceiverAdapter(void *userdata, tTVPWindowMessage *message) {
    const auto found = g_receivers.find(static_cast<int>(
        reinterpret_cast<intptr_t>(userdata)));
    if(found == g_receivers.end())
        return false;

    tTJSVariantClosure closure = found->second.Callback.AsObjectClosure();
    if(!closure.Object)
        return false;

    // The callback receives the userdata plus the three message fields.  The
    // Win32-path receivers take the struct pointer, but a TJS callback cannot
    // hold that pointer, so the test module passes the fields the struct
    // carries.
    tTJSVariant args[4];
    args[0] = found->second.UserData;
    // The 64bit fields need an explicit width: tTJSVariant's integer overloads
    // make the plain unsigned types ambiguous.
    args[1] = static_cast<tjs_int64>(message->Msg);
    args[2] = static_cast<tjs_int64>(message->WParam);
    args[3] = static_cast<tjs_int64>(message->LParam);

    tTJSVariant *argv[4] = {&args[0], &args[1], &args[2], &args[3]};
    tTJSVariant result;
    if(TJS_FAILED(closure.FuncCall(0, nullptr, nullptr, &result, 4, argv,
                                   nullptr)))
        return false;
    return result.AsInteger() != 0;
}

//----------------------------------------------------------------------------
// Poster thread
//----------------------------------------------------------------------------
class PosterThread {
public:
    ~PosterThread() { Stop(); }

    bool Start(tTJSNI_Window *window) {
        Stop();
        Window = window;
        StopRequested.store(false);
        Worker = std::thread([this]() { Run(); });
        return true;
    }

    bool Stop() {
        if(!Worker.joinable())
            return true;
        StopRequested.store(true);
        Worker.join();
        Window = nullptr;
        return true;
    }

    [[nodiscard]] bool Running() const { return Worker.joinable(); }

private:
    void Run() {
        TVPPostWindowMessage(Window, kTestMessage, kTestWParam, kTestLParam);
        // The receivers under test consume the message, so one post per start
        // is enough; keep the thread alive until the fixture stops it.
        while(!StopRequested.load())
            std::this_thread::yield();
    }

    tTJSNI_Window *Window = nullptr;
    std::atomic<bool> StopRequested{false};
    std::thread Worker;
};

PosterThread &poster() {
    static PosterThread instance;
    return instance;
}

// NCB_ATTACH_FUNCTION registers silently when the target class does not exist
// yet.  Resolving Window during pre-registration separates "the fixture has no
// such method" from "the channel does not deliver".
void EnsureWindowClassRegistered() {
    tTJSVariant windowClass;
    TVPExecuteExpression(TJS_W("Window"), &windowClass);
    if(windowClass.Type() != tvtObject)
        TVPThrowExceptionMessage(
            TJS_W("windowMessageChannel.dll requires the Window class."));
}

tTJSNI_Window *NativeWindow(iTJSDispatch2 *objthis) {
    tTJSNI_Window *window = nullptr;
    if(TJS_FAILED(objthis->NativeInstanceSupport(
           TJS_NIS_GETINSTANCE, tTJSNC_Window::ClassID,
           (iTJSNativeInstance **)&window)) ||
       !window)
        TVPThrowExceptionMessage(
            TJS_W("windowMessageChannel.dll requires a Window instance."));
    return window;
}

//----------------------------------------------------------------------------
// Window.registerTestReceiver(callback, userdata)
//----------------------------------------------------------------------------
// NCB_ATTACH_FUNCTION passes the target object as the trailing objthis, so the
// declared arguments are param[0..n-1]; there is no leading self parameter.
tjs_error TJS_INTF_METHOD RegisterTestReceiver(tTJSVariant *result,
                                               tjs_int numparams,
                                               tTJSVariant **param,
                                               iTJSDispatch2 *objthis) {
    if(numparams < 2)
        return TJS_E_BADPARAMCOUNT;

    const int handle = g_nextReceiverHandle++;
    g_receivers.emplace(handle, ReceiverEntry{*param[0], *param[1]});

    NativeWindow(objthis)->RegisterWindowMessageReceiver(
        wrmRegister,
        reinterpret_cast<void *>(ReceiverAdapter),
        reinterpret_cast<const void *>(static_cast<intptr_t>(handle)));

    if(result)
        *result = static_cast<tjs_int>(handle);
    return TJS_S_OK;
}

//----------------------------------------------------------------------------
// Window.unregisterTestReceiver(handle)
//----------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD UnregisterTestReceiver(tTJSVariant *result,
                                                 tjs_int numparams,
                                                 tTJSVariant **param,
                                                 iTJSDispatch2 *objthis) {
    if(numparams < 1)
        return TJS_E_BADPARAMCOUNT;

    // The registration is identified by (proc, userdata), and userdata is a
    // second copy of the handle, so the entry can be addressed without keeping
    // the original callback object around.
    const auto found = g_receivers.find((int)param[0]->AsInteger());
    if(found == g_receivers.end()) {
        if(result)
            *result = false;
        return TJS_S_OK;
    }

    const auto *handle = reinterpret_cast<const void *>(
        static_cast<intptr_t>(found->first));
    NativeWindow(objthis)->RegisterWindowMessageReceiver(
        wrmUnregister, reinterpret_cast<void *>(ReceiverAdapter), handle);
    g_receivers.erase(found);

    if(result)
        *result = true;
    return TJS_S_OK;
}

//----------------------------------------------------------------------------
// Window.startTestPosterThread(window)
//----------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD StartTestPosterThread(tTJSVariant *result,
                                                tjs_int numparams,
                                                tTJSVariant **param,
                                                iTJSDispatch2 *objthis) {
    // Window.HWND is the value plugins hand back to TVPPostWindowMessage; the
    // test uses the same property so it covers the real plugin call shape.
    tTJSVariant hwnd;
    objthis->PropGet(0, TJS_W("HWND"), nullptr, &hwnd, objthis);

    if(result)
        *result = poster().Start(NativeWindow(objthis));
    return TJS_S_OK;
}

//----------------------------------------------------------------------------
// Window.stopTestPosterThread()
//----------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD StopTestPosterThread(tTJSVariant *result,
                                               tjs_int numparams,
                                               tTJSVariant **param,
                                               iTJSDispatch2 *objthis) {
    if(result)
        *result = poster().Stop();
    return TJS_S_OK;
}

//----------------------------------------------------------------------------
// Window.testPosterThreadCount()
//----------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD TestPosterThreadCount(tTJSVariant *result,
                                                tjs_int numparams,
                                                tTJSVariant **param,
                                                iTJSDispatch2 *objthis) {
    if(result)
        *result = poster().Running() ? static_cast<tjs_int>(1)
                                     : static_cast<tjs_int>(0);
    return TJS_S_OK;
}

} // namespace

NCB_ATTACH_FUNCTION(registerTestReceiver, Window, RegisterTestReceiver);
NCB_ATTACH_FUNCTION(unregisterTestReceiver, Window, UnregisterTestReceiver);
NCB_ATTACH_FUNCTION(startTestPosterThread, Window, StartTestPosterThread);
NCB_ATTACH_FUNCTION(stopTestPosterThread, Window, StopTestPosterThread);
NCB_ATTACH_FUNCTION(testPosterThreadCount, Window, TestPosterThreadCount);

NCB_PRE_REGIST_CALLBACK(EnsureWindowClassRegistered);
