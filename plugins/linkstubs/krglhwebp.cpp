// krglhwebp.dll：链接空壳 (Yoghourt)
//
// 上游 uyjulian/krglhwebp 给 ".webp" 注册加载、头解析、保存、保存接受四个
// 图像处理器。本构建不携带这个 DLL，WebP 编解码由核心
// core/visual/LoadWEBP.cpp 提供，处理器登记在
// core/visual/GraphicsLoaderIntf.cpp 的 ".webp" 条目上。
// 本文件不注册 TJS 类，也不提供 TJS 成员。
//
// 接线位置：plugins/CMakeLists.txt 的 PRIVATE 源列表、
// core/plugin/PluginImpl.cpp 的锚点声明与调用、tests/plugin-quality-gates.json
// 的 manifest 条目、tests/test_files/krglhwebp/startup.tjs。
// 加新名字的完整步骤见 guides/0017-krkr2-compat.md。

#include "ncbind.hpp"

#include <spdlog/spdlog.h>

#define NCB_MODULE_NAME TJS_W("krglhwebp.dll")
static void KrglhwebpPreRegist() {
    if(auto logger = spdlog::get("plugin"))
        logger->info("[krglhwebp] linked; the core decodes and encodes WebP "
                     "(core/visual/LoadWEBP.cpp)");
}
NCB_PRE_REGIST_CALLBACK(KrglhwebpPreRegist);

// 链接保留：静态库剥离无引用的翻译单元。PluginImpl.cpp 调用这个锚点。
extern "C" void TVPKrglhwebpPluginAnchor() {}
