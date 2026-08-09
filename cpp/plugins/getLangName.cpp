#include "ncbind.hpp"

#include <algorithm>
#include <cctype>
#include <string>

#define NCB_MODULE_NAME TJS_W("getLangName.dll")

std::string TVPGetCurrentLanguage();

namespace {
    std::string normalizedLocale() {
        std::string locale = TVPGetCurrentLanguage();
        std::replace(locale.begin(), locale.end(), '-', '_');
        return locale;
    }

    ttstr getCurrentLocaleName() { return ttstr(normalizedLocale()); }

    ttstr getCurrentUILangName() {
        std::string locale = normalizedLocale();
        std::transform(locale.begin(), locale.end(), locale.begin(),
                       [](unsigned char value) {
                           return static_cast<char>(std::tolower(value));
                       });

        const auto matchesLanguage = [&locale](const char *language) {
            return locale == language ||
                (locale.size() > 2 && locale.compare(0, 2, language) == 0 &&
                 locale[2] == '_');
        };

        if(matchesLanguage("ja"))
            return TJS_W("Japanese");
        if(matchesLanguage("zh"))
            return TJS_W("Chinese");
        if(matchesLanguage("en"))
            return TJS_W("English");
        return ttstr(locale);
    }
} // namespace

NCB_ATTACH_FUNCTION(getCurrentLocaleName, System, getCurrentLocaleName);
NCB_ATTACH_FUNCTION(getCurrentUILangName, System, getCurrentUILangName);
