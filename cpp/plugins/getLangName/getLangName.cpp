#include "ncbind.hpp"

#include <algorithm>
#include <cctype>
#include <string>

#include <spdlog/spdlog.h>

//----------------------------------------------------------------------
// 链接锚点
//
// getLangName.cpp 是 PRIVATE source，只编译进聚合库 krkr2plugin。聚合库
// 里的翻译单元若没有任何被外部引用的全局符号，链接器会做死代码剥离
// （dead-strip），整个 .o 被丢弃，插件注册永远不会发生。
// 这个锚点被 PluginImpl.cpp 的 TVPLoadInternalPlugins() 引用，强制链接器
// 保留本翻译单元，从而让 NCB_ATTACH_FUNCTION 的静态注册对象生效。
//----------------------------------------------------------------------
extern "C" void TVPGetLangNamePluginAnchor() {}

#define NCB_MODULE_NAME TJS_W("getLangName.dll")

//----------------------------------------------------------------------
// 平台语言查询
//
// 与原版 getLangName.dll（ねこぱら Windows 版）行为保持一致：
//   - getCurrentLocaleName(flag?)  → BCP-47 区域名（如 "ja-JP"、"zh-CN"）
//   - getCurrentUILangName(flag?)  → BCP-47 区域名（同格式）
//   - flag=true  → 系统默认；flag=false / 无参 → 用户默认
//----------------------------------------------------------------------

#if defined(_WIN32)

#include <windows.h>

static std::string wideToUTF8(const wchar_t *wide, int length) {
    if (length <= 0)
        return {};
    const int utf8Length =
        WideCharToMultiByte(CP_UTF8, 0, wide, length, nullptr, 0, nullptr, nullptr);
    if (utf8Length <= 0)
        return {};
    std::string result(utf8Length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, length, &result[0], utf8Length, nullptr,
                        nullptr);
    return result;
}

// 原 DLL：GetLocaleInfoW(LCID, LOCALE_SNAME) 拿 BCP-47 区域名
static std::string queryLocaleName(bool useSystem) {
    const LCID lcid = useSystem ? LOCALE_SYSTEM_DEFAULT : LOCALE_USER_DEFAULT;
    wchar_t buffer[LOCALE_NAME_MAX_LENGTH] = {0};
    const int length = GetLocaleInfoW(lcid, LOCALE_SNAME, buffer, LOCALE_NAME_MAX_LENGTH);
    std::string name = wideToUTF8(buffer, length > 0 ? length - 1 : 0);
    return name.empty() ? "en-US" : name;
}

// 原 DLL：GetSystemDefaultUILanguage / GetUserDefaultUILanguage → LOCALE_SNAME
static std::string queryUILangName(bool useSystem) {
    const LANGID langid =
        useSystem ? GetSystemDefaultUILanguage() : GetUserDefaultUILanguage();
    wchar_t buffer[LOCALE_NAME_MAX_LENGTH] = {0};
    const int length = GetLocaleInfoW(MAKELCID(langid, SORT_DEFAULT), LOCALE_SNAME,
                                      buffer, LOCALE_NAME_MAX_LENGTH);
    std::string name = wideToUTF8(buffer, length > 0 ? length - 1 : 0);
    return name.empty() ? "en-US" : name;
}

#elif defined(__APPLE__)

#include <CoreFoundation/CoreFoundation.h>

static std::string cfStringToUTF8(CFStringRef str) {
    if (!str)
        return {};
    const CFIndex length = CFStringGetLength(str);
    const CFIndex maxSize =
        CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8);
    if (maxSize <= 0)
        return {};
    std::string out(static_cast<size_t>(maxSize), '\0');
    if (!CFStringGetCString(str, &out[0], maxSize, kCFStringEncodingUTF8))
        return {};
    out.resize(std::strlen(out.c_str()));
    return out;
}

// 用语言代码 + 国家代码拼 BCP-47（"zh" + "CN" → "zh-CN"），不带 script 段
static std::string localeIdentifierBCP47(CFLocaleRef locale) {
    CFStringRef language =
        static_cast<CFStringRef>(CFLocaleGetValue(locale, kCFLocaleLanguageCode));
    CFStringRef country =
        static_cast<CFStringRef>(CFLocaleGetValue(locale, kCFLocaleCountryCode));
    std::string result = cfStringToUTF8(language);
    if (result.empty())
        result = "en";
    const std::string region = cfStringToUTF8(country);
    if (!region.empty())
        result += "-" + region;
    return result;
}

// macOS 没有严格的「系统默认 vs 用户默认」语言之分，近似映射：
//   系统默认 → CFLocaleCopyCurrent()（系统当前区域）
//   用户默认 → 首选语言列表第一项（用户界面语言）
static std::string queryLocaleName(bool useSystem) {
    if (useSystem) {
        CFLocaleRef locale = CFLocaleCopyCurrent();
        if (!locale)
            return "en-US";
        std::string result = localeIdentifierBCP47(locale);
        CFRelease(locale);
        return result;
    }
    CFArrayRef preferred = CFLocaleCopyPreferredLanguages();
    if (!preferred || CFArrayGetCount(preferred) == 0) {
        if (preferred)
            CFRelease(preferred);
        return "en-US";
    }
    CFStringRef first =
        static_cast<CFStringRef>(CFArrayGetValueAtIndex(preferred, 0));
    CFLocaleRef locale = CFLocaleCreate(nullptr, first);
    std::string result = locale ? localeIdentifierBCP47(locale) : "en-US";
    if (locale)
        CFRelease(locale);
    CFRelease(preferred);
    return result;
}

static std::string queryUILangName(bool useSystem) {
    return queryLocaleName(useSystem);
}

#else // Linux / Android 等

// 复用现有平台实现（返回 "ja_jp" 下划线小写格式），转成 BCP-47 连字符格式
std::string TVPGetCurrentLanguage();

static std::string underscoreToBCP47(std::string locale) {
    std::transform(locale.begin(), locale.end(), locale.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const size_t separator = locale.find('_');
    if (separator != std::string::npos && separator + 1 < locale.size()) {
        locale[separator] = '-';
        for (size_t i = separator + 1; i < locale.size(); ++i)
            locale[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(locale[i])));
    }
    return locale;
}

// 现有平台实现无系统/用户之分，统一走用户默认
static std::string queryLocaleName(bool useSystem) {
    std::string locale = underscoreToBCP47(TVPGetCurrentLanguage());
    return locale.empty() ? "en-US" : locale;
}

static std::string queryUILangName(bool useSystem) {
    return queryLocaleName(useSystem);
}

#endif

//----------------------------------------------------------------------
// TJS 接口
//
// 原 DLL 用 SimpleBinder 老式回调（tjs_error (*)(result, numparams, param,
// objthis)），支持可选参数。ncbind 的 NCB_ATTACH_FUNCTION 对精确匹配
// ncbTypedefs::CallbackT 的函数指针走 TJSCreateNativeClassMethod 老式回调
// 路径，参数个数不受限，与 TJS 脚本的可选参数语义一致。
//----------------------------------------------------------------------

namespace {
    // 显式调用 operator bool() 而不是 static_cast<bool>：tTJSVariant 同时有
    // operator bool / operator tTVInteger / operator iTJSDispatch2*，重载决议
    // 可能选中 iTJSDispatch2*（AsObject），对整数参数抛 "Cannot convert to
    // Object"。显式调用保证走 bool 转换（与原 DLL 的 tTJSVariant::operator
    // bool 行为一致）。
    inline bool variantAsBool(tTJSVariant const *v) {
        return v ? (*v).operator bool() : false;
    }

    tjs_error getCurrentLocaleName(tTJSVariant *result, tjs_int numparams,
                                   tTJSVariant **param, iTJSDispatch2 *objthis) {
        if (result) {
            const bool useSystem =
                numparams > 0 && param[0] && variantAsBool(param[0]);
            *result = ttstr(queryLocaleName(useSystem));
        }
        return TJS_S_OK;
    }

    tjs_error getCurrentUILangName(tTJSVariant *result, tjs_int numparams,
                                   tTJSVariant **param, iTJSDispatch2 *objthis) {
        if (result) {
            const bool useSystem =
                numparams > 0 && param[0] && variantAsBool(param[0]);
            *result = ttstr(queryUILangName(useSystem));
        }
        return TJS_S_OK;
    }
} // namespace

NCB_ATTACH_FUNCTION(getCurrentLocaleName, System, getCurrentLocaleName);
NCB_ATTACH_FUNCTION(getCurrentUILangName, System, getCurrentUILangName);
