// wuopus.dll：链接空壳 (Yoghourt)
//
// 旧 KAG 脚本用 Plugins.link("wuopus.dll") 的成败选分支。本构建不携带这个
// DLL。Ogg Opus 解码由核心 core/sound/VorbisWaveDecoder.cpp 提供。本文件不
// 注册 TJS 类，也不提供 TJS 成员。
//
// 接线位置：plugins/CMakeLists.txt 的 PRIVATE 源列表、
// core/plugin/PluginImpl.cpp 的锚点声明与调用、tests/plugin-quality-gates.json
// 的 manifest 条目、tests/test_files/wuopus/startup.tjs。
// 加新名字的完整步骤见 guides/0017-krkr2-compat.md。

#include "ncbind.hpp"

#include <spdlog/spdlog.h>

#define NCB_MODULE_NAME TJS_W("wuopus.dll")
static void WuopusPreRegist() {
    if(auto logger = spdlog::get("plugin"))
        logger->info("[wuopus] linked; the core provides Opus decoding "
                     "(core/sound/VorbisWaveDecoder.cpp)");
}
NCB_PRE_REGIST_CALLBACK(WuopusPreRegist);

// 链接保留：静态库剥离无引用的翻译单元。PluginImpl.cpp 调用这个锚点。
extern "C" void TVPWuopusPluginAnchor() {}
