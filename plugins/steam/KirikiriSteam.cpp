// KirikiriSteam.dll
//
// しゅがてん！-sugarfull tempering- 英文 Steam 版的移植方自制插件，没有公开源码。
// 接口来自对原版 DLL 的反编译（Yoghourt feature 0013 README）：
//   - V2Link 只在 global 注册函数 setAchievement(id)，没有其他成员。
//   - 参数个数不等于 1 时返回 TJS_E_BADPARAMCOUNT。
//   - 从不写 result，返回 void；Steam 失败也不报告给脚本。
// 本实现保留该接口，调用成功，不保存成就。

#define NCB_MODULE_NAME TJS_W("KirikiriSteam.dll")

#include "ncbind.hpp"

#include <spdlog/spdlog.h>

namespace {
tjs_error kirikiriSteamSetAchievement(tTJSVariant *, tjs_int numparams,
                                      tTJSVariant **param, iTJSDispatch2 *) {
    if(numparams != 1)
        return TJS_E_BADPARAMCOUNT;
    if(auto logger = spdlog::get("plugin"))
        logger->info("[KirikiriSteam] setAchievement({}) ignored",
                     ttstr(*param[0]).AsStdString());
    return TJS_S_OK;
}
} // namespace

NCB_REGISTER_FUNCTION(setAchievement, kirikiriSteamSetAchievement);

// Registration-only translation units in static libraries need an explicit
// reference from PluginImpl.cpp, otherwise the linker drops this object.
extern "C" void TVPKirikiriSteamPluginAnchor() {}
