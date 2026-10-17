// kirikiroid2.dll: compatibility helpers that Android ports expose.
//
// The archive formats (ZIP/7z/TAR) and image formats (JPEG/PNG/WebP/BPG) that
// the original plugin registered are native core features in this build, so
// this module only provides the two script-side helpers games call.
//
// Ported from krkrsdl3 plugins/Kirikiroid2/kirikiroid2.cpp and
// AetherKrkr plugins/kirikiroid2.cpp.

#include "TextStream.h"
#include "ncbind.hpp"

#ifndef TJS_INTF_METHOD
#define TJS_INTF_METHOD
#endif

#define NCB_MODULE_NAME TJS_W("kirikiroid2.dll")

namespace {

// _str_ord returns the Unicode code point of the first character. TJS strings
// are UTF-16, so a surrogate pair must be combined; returning only the high
// surrogate would break scripts that compare ordinals with numeric literals.
tjs_error TJS_INTF_METHOD krkrStrOrd(tTJSVariant *result, tjs_int numparams,
                                     tTJSVariant **param, iTJSDispatch2 *) {
    if(numparams < 1)
        return TJS_E_BADPARAMCOUNT;

    if(param[0]->Type() != tvtString) {
        if(result)
            *result = *param[0];
        return TJS_S_OK;
    }

    const tjs_char *text = param[0]->GetString();
    if(!text || !*text) {
        if(result)
            *result = 0;
        return TJS_S_OK;
    }

    tjs_uint32 code = static_cast<tjs_uint32>(text[0]);
    if(code >= 0xd800 && code <= 0xdbff && text[1] >= 0xdc00 &&
       text[1] <= 0xdfff) {
        code = 0x10000 + ((code - 0xd800) << 10) +
               (static_cast<tjs_uint32>(text[1]) - 0xdc00);
    }

    if(result)
        *result = static_cast<tTVInteger>(code);
    return TJS_S_OK;
}

tjs_error TJS_INTF_METHOD krkrSetTextEncoding(tTJSVariant *, tjs_int numparams,
                                              tTJSVariant **param,
                                              iTJSDispatch2 *) {
    if(numparams < 1)
        return TJS_E_BADPARAMCOUNT;
    if(param[0]->Type() == tvtString)
        TVPSetDefaultReadEncoding(*param[0]);
    return TJS_S_OK;
}

} // namespace

NCB_REGISTER_FUNCTION(_str_ord, krkrStrOrd);
NCB_ATTACH_FUNCTION(setTextEncoding, Storages, krkrSetTextEncoding);

// Registration-only translation units in static libraries need an explicit
// reference from PluginImpl.cpp, otherwise the linker drops this object.
extern "C" void TVPKirikiroid2PluginAnchor() {}
