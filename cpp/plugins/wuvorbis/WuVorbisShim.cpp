//---------------------------------------------------------------------------
// wuvorbis.dll 兼容性模块 (Yoghourt)
//
// 旧版 wuvorbis.dll (kirikiri2/trunk/kirikiri2/src/plugins/win32/wuvorbis)
// 是 Windows 专有的 TSS(COM) 声音解码插件。新核心 (krkrz/krkrz) 已把
// Ogg Vorbis 解码原生并入核心：
//   cpp/core/sound/VorbisWaveDecoder.cpp  +  WaveIntf.cpp 注册 ".ogg" 解码器
// 因此 Yoghourt runtime 无需移植旧 COM 插件；本模块只提供旧游戏脚本
// 依赖的模块契约 —— Plugins.link("wuvorbis.dll") 成功。
//
// 旧 KAG 游戏的 system 脚本经常以 Plugins.link("wuvorbis.dll") 的成败
// 决定是否启用 .ogg 分支；链接失败会让这些游戏回退到 wav 分支或报错。
// 链接成功后 .ogg 的实际解码走核心原生解码器（优于旧版 patched libvorbis）。
//
// 注意：本模块不注册任何 TJS 可见类/函数；旧 wuvorbis 也没有 TJS 可见成员。
//---------------------------------------------------------------------------

#include "ncbind.hpp"

#include <spdlog/spdlog.h>

#define NCB_MODULE_NAME TJS_W("wuvorbis.dll")

static void WuVorbisPreRegist() {
    if(auto logger = spdlog::get("plugin"))
        logger->info(
            "[wuvorbis] linked; .ogg decoding is provided natively by the "
            "core (sound/VorbisWaveDecoder)");
}

// 模块桶存在即满足 Plugins.link / Plugins.CanLoadPlugin 契约。
NCB_PRE_REGIST_CALLBACK(WuVorbisPreRegist);

// 注册型翻译单元在静态库中可能被死代码剥离；PluginImpl.cpp 显式引用此锚点。
extern "C" void TVPWuVorbisPluginAnchor() {}
