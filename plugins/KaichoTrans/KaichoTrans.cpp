// KaichoTrans.dll
//
// Ported from KaichoTrans-0251 (kaede-software). The author released it
// with a permissive license: use, modify, redistribute and sell freely,
// without warranty. See the oracle directory for the original text.
//
// The plugin adds two transition providers:
//   blur  box blur based transition
//   dim   rule-image based dim transition
//
// SSE2 is disabled: the original uses MSVC x86 inline assembly, and the
// scalar path is complete.

#include "common.h"
#include "blur.h"
#include "dim.h"

#include "ncbind.hpp"

#include <spdlog/spdlog.h>

#define NCB_MODULE_NAME TJS_W("KaichoTrans.dll")

extern "C" void TVPKaichoTransPluginAnchor() {}

static void RegisterKaichoTransProviders() {
    RegisterBlurTransHandlerProvider();
    RegisterDimTransHandlerProvider();

    if(auto logger = spdlog::get("plugin"))
        logger->info("[KaichoTrans] linked; blur and dim transitions registered");
}

static void UnregisterKaichoTransProviders() {
    UnregisterBlurTransHandlerProvider();
    UnregisterDimTransHandlerProvider();
}

NCB_PRE_REGIST_CALLBACK(RegisterKaichoTransProviders);
NCB_POST_UNREGIST_CALLBACK(UnregisterKaichoTransProviders);
