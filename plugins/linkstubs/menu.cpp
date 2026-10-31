// menu.dll：链接空壳 (Yoghourt)
//
// 旧 KAG 脚本用 Plugins.link("menu.dll") 的成败选分支。本构建不携带这个 DLL。
// MenuItem 类由核心 core/base/ScriptMgnIntf.cpp 注册。本文件不注册 TJS 类，
// 也不提供 TJS 成员。
//
// 接线位置：plugins/CMakeLists.txt 的 PRIVATE 源列表、
// core/plugin/PluginImpl.cpp 的锚点声明与调用、tests/plugin-quality-gates.json
// 的 manifest 条目、tests/test_files/menu/startup.tjs。
// 加新名字的完整步骤见 guides/0017-krkr2-compat.md。

#include "ncbind.hpp"

#include <spdlog/spdlog.h>

#define NCB_MODULE_NAME TJS_W("menu.dll")
static void MenuPreRegist() {
    if(auto logger = spdlog::get("plugin"))
        logger->info("[menu] linked; the core provides the MenuItem class "
                     "(core/base/ScriptMgnIntf.cpp)");
}
NCB_PRE_REGIST_CALLBACK(MenuPreRegist);

// 链接保留：静态库剥离无引用的翻译单元。PluginImpl.cpp 调用这个锚点。
extern "C" void TVPMenuPluginAnchor() {}
