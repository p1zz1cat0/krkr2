// getLangName.dll 插件行为测试
//
// 验证与原版 getLangName.dll（ねこぱら Windows 版）的兼容契约：
//   - System.getCurrentLocaleName() / System.getCurrentUILangName() 可调用
//   - 返回 BCP-47 连字符格式（如 "en-US"、"zh-CN"），而非下划线或语言全名
//   - flag=true → 系统默认；flag=false / 无参 → 用户默认

#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <string>

#include <spdlog/spdlog.h>

#include "DebugImpl.h"
#include "Platform.h"
#include "SystemIntf.h"
#include "ncbind.hpp"
#include "tjsScriptBlock.h"

#include "test_config.h"

class TJSConsoleOutputDef final : public iTJSConsoleOutput {
public:
    void ExceptionPrint(const tjs_char *msg) override {
        spdlog::get("tjs2")->critical(ttstr{ msg }.AsStdString());
    }

    void Print(const tjs_char *msg) override {
        spdlog::get("tjs2")->info(ttstr{ msg }.AsStdString());
    }
} static iTJSConsoleOutputDef{};

static bool localeLooksBCP47(const ttstr &value) {
    // BCP-47: "xx-YY" 或 "xx"，语言段小写、地区段大写、连字符分隔
    const std::string s = value.AsStdString();
    if (s.empty())
        return false;
    const size_t sep = s.find('-');
    const std::string lang = sep == std::string::npos ? s : s.substr(0, sep);
    if (lang.size() < 2 || lang.size() > 3)
        return false;
    for (char c : lang)
        if (!std::isalpha(static_cast<unsigned char>(c)))
            return false;
    if (sep != std::string::npos) {
        const std::string region = s.substr(sep + 1);
        if (region.size() != 2)
            return false;
        for (char c : region)
            if (!std::isalpha(static_cast<unsigned char>(c)))
                return false;
    }
    return true;
}

TEST_CASE("getLangName plugin registers on System and returns BCP-47 locale names") {
    const auto tvPScriptEngine = new tTJS();
    tvPScriptEngine->SetPPValue(TJS_W("krkr2"), 1);
    tvPScriptEngine->SetConsoleOutput(&iTJSConsoleOutputDef);

    // TVPGetScriptDispatch() 依赖全局 TVPScriptEngine 指针，ncbind 的
    // GetDispatch 通过它找 System 对象；不设置则插件注册时挂载失败。
    extern tTJS *TVPScriptEngine;
    TVPScriptEngine = tvPScriptEngine;

    // 注册 System 类到全局（NCB_ATTACH_FUNCTION 的挂载目标）
    iTJSDispatch2 *global = tvPScriptEngine->GetGlobalNoAddRef();
    tTJSVariant systemValue(TVPCreateNativeClass_System());
    global->PropSet(TJS_MEMBERENSURE | TJS_IGNOREPROP, TJS_W("System"), nullptr,
                    &systemValue, global);

    // 加载 getLangName 内部插件模块
    ncbAutoRegister::AllRegist();
    REQUIRE(ncbAutoRegister::LoadModule(TJS_W("getLangName.dll")));

    // 无参数调用（用户默认）：返回 BCP-47 格式
    tTJSVariant userLocale;
    REQUIRE_NOTHROW(tvPScriptEngine->EvalExpression(
        TJS_W("System.getCurrentLocaleName()"), &userLocale));
    REQUIRE(userLocale.Type() == tvtString);
    INFO("user locale name = " << ttstr(userLocale.AsString()).AsStdString());
    REQUIRE(localeLooksBCP47(ttstr(userLocale.AsString())));

    // flag=false（用户默认）
    tTJSVariant explicitUser;
    REQUIRE_NOTHROW(tvPScriptEngine->EvalExpression(
        TJS_W("System.getCurrentLocaleName(false)"), &explicitUser));
    REQUIRE(explicitUser.Type() == tvtString);
    REQUIRE(localeLooksBCP47(ttstr(explicitUser.AsString())));
    REQUIRE(ttstr(explicitUser.AsString()) == ttstr(userLocale.AsString()));

    // flag=true（系统默认）
    tTJSVariant systemLocale;
    REQUIRE_NOTHROW(tvPScriptEngine->EvalExpression(
        TJS_W("System.getCurrentLocaleName(true)"), &systemLocale));
    REQUIRE(systemLocale.Type() == tvtString);
    INFO("system locale name = " << ttstr(systemLocale.AsString()).AsStdString());
    REQUIRE(localeLooksBCP47(ttstr(systemLocale.AsString())));

    // getCurrentUILangName 同样返回 BCP-47 locale 名（不是 "English" 这类全名）
    tTJSVariant uiLang;
    REQUIRE_NOTHROW(tvPScriptEngine->EvalExpression(
        TJS_W("System.getCurrentUILangName()"), &uiLang));
    REQUIRE(uiLang.Type() == tvtString);
    INFO("ui lang name = " << ttstr(uiLang.AsString()).AsStdString());
    REQUIRE(localeLooksBCP47(ttstr(uiLang.AsString())));

    tvPScriptEngine->Release();
}
