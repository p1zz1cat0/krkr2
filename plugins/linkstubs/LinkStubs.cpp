// 内部模块名空壳 (Yoghourt)
//
// 旧 KAG 脚本用 Plugins.link("x.dll") 的成败选分支。本构建不携带这些 DLL。
// 模块名缺失时 link 失败，脚本进入错误分支。
//
// 本文件登记 4 个模块名。本文件不注册 TJS 类，也不提供 TJS 成员。
//
// 名字与核心实现的对应：
//   kagexopt.dll  KAG 解析。核心实现在 core/base/KAGParser.cpp。
//   krmovie.dll   视频播放。核心实现在 core/visual/impl/VideoOvlImpl.cpp
//                 与 core/movie/。
//   menu.dll      MenuItem 类。核心实现在 core/base/ScriptMgnIntf.cpp。
//   wuopus.dll    Ogg Opus 解码。核心实现在 core/sound/VorbisWaveDecoder.cpp。
//
// 不要登记 dmmcloud.dll、krkrsteam.dll、win32ole.dll、k2compat.dll、
// kztouch.dll。核心没有它们的功能，也没有它们注册的 TJS 类
// （k2compat: WindowTouchMouse、ModelessOwnerWindow；
// kztouch: KzTouchWindow、TouchHandler）。
// 脚本 link 成功后再调用它们的成员，会抛 TJS 异常。
//
// 链接保留：静态库剥离无引用的翻译单元。core/plugin/PluginImpl.cpp 调用
// 下面 4 个锚点。一个锚点就能保住整个翻译单元（4 个模块桶在同一个 .o 里）。
// 分开定义 4 个是 gate manifest 的要求：每条目一个唯一的 anchor。

#include "ncbind.hpp"

#include <spdlog/spdlog.h>

#define NCB_MODULE_NAME TJS_W("kagexopt.dll")
static void KagexoptPreRegist() {
    if(auto logger = spdlog::get("plugin"))
        logger->info("[kagexopt] linked; the core provides KAG parsing "
                     "(core/base/KAGParser.cpp)");
}
NCB_PRE_REGIST_CALLBACK(KagexoptPreRegist);

#undef NCB_MODULE_NAME
#define NCB_MODULE_NAME TJS_W("krmovie.dll")
static void KrmoviePreRegist() {
    if(auto logger = spdlog::get("plugin"))
        logger->info("[krmovie] linked; the core provides movie playback "
                     "(core/visual/impl/VideoOvlImpl.cpp)");
}
NCB_PRE_REGIST_CALLBACK(KrmoviePreRegist);

#undef NCB_MODULE_NAME
#define NCB_MODULE_NAME TJS_W("menu.dll")
static void MenuPreRegist() {
    if(auto logger = spdlog::get("plugin"))
        logger->info("[menu] linked; the core provides the MenuItem class "
                     "(core/base/ScriptMgnIntf.cpp)");
}
NCB_PRE_REGIST_CALLBACK(MenuPreRegist);

#undef NCB_MODULE_NAME
#define NCB_MODULE_NAME TJS_W("wuopus.dll")
static void WuopusPreRegist() {
    if(auto logger = spdlog::get("plugin"))
        logger->info("[wuopus] linked; the core provides Opus decoding "
                     "(core/sound/VorbisWaveDecoder.cpp)");
}
NCB_PRE_REGIST_CALLBACK(WuopusPreRegist);

// 每个名字一个锚点。引用任一个就保住整个翻译单元。
extern "C" void TVPKagexoptPluginAnchor() {}
extern "C" void TVPKrmoviePluginAnchor() {}
extern "C" void TVPMenuPluginAnchor() {}
extern "C" void TVPWuopusPluginAnchor() {}
