// krmovie.dll：链接空壳 (Yoghourt)
//
// 旧 KAG 脚本用 Plugins.link("krmovie.dll") 的成败选分支。本构建不携带这个
// DLL。视频播放由核心 core/visual/impl/VideoOvlImpl.cpp 与 core/movie/ 提供。
// 本文件不注册 TJS 类，也不提供 TJS 成员。
//
// 接线位置：plugins/CMakeLists.txt 的 PRIVATE 源列表、
// core/plugin/PluginImpl.cpp 的锚点声明与调用、tests/plugin-quality-gates.json
// 的 manifest 条目、tests/test_files/krmovie/startup.tjs。
// 加新名字的完整步骤见 guides/0017-krkr2-compat.md。

#include "ncbind.hpp"

#include <spdlog/spdlog.h>

#define NCB_MODULE_NAME TJS_W("krmovie.dll")
static void KrmoviePreRegist() {
    if(auto logger = spdlog::get("plugin"))
        logger->info("[krmovie] linked; the core provides movie playback "
                     "(core/visual/impl/VideoOvlImpl.cpp)");
}
NCB_PRE_REGIST_CALLBACK(KrmoviePreRegist);

// 链接保留：静态库剥离无引用的翻译单元。PluginImpl.cpp 调用这个锚点。
extern "C" void TVPKrmoviePluginAnchor() {}
