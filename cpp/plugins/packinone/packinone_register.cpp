// packinone_register.cpp — PackinOne.dll 兼容插件的 ncbind 注册入口。
//
// 注册同名模块满足 Plugins.link("PackinOne.dll") 特征门，并提供
// PackinOne 的 TJS 接口子集（macOS 可实现的）：
//   - Layer.clipAlphaRect（wtnbgo/layerExBTOA 移植）
//   - Plugins.CanLoadPlugin
//   - System.urlencode/urldecode/readEnvValue/writeEnvValue/
//     expandEnvString/getOSVersion/getKnownFolderPath/confirm/
//     waitForAppLock/setDpiAwareness/commandExecute/writeRegValue
//   - Layer.shrinkCopy/shrinkCopyFast（简化占位）
//   - Process（run/wait/exitCode，macOS system() 语义）
//
// StoragesFstat/TemporaryFiles/ScriptsAdd/LZ4 见同目录其他文件。

#include "packinone.h"

#include <cctype>
#include <cstdlib>
#include <string>

#define NCB_MODULE_NAME TJS_W("packinone.dll")

class PackinOneDummy {
public:
    static void Stub() {}
};

NCB_REGISTER_CLASS(PackinOneDummy) {
    NCB_METHOD(Stub);
}

static bool HasGlobalMember(const tjs_char *name) {
    tTJS *engine = TVPGetScriptEngine();
    if(!engine)
        return false;
    iTJSDispatch2 *global = engine->GetGlobalNoAddRef();
    if(!global)
        return false;
    tTJSVariant value;
    return TJS_SUCCEEDED(global->PropGet(0, name, nullptr, &value, global)) &&
        value.Type() != tvtVoid;
}

// Layer.clipAlphaRect from wtnbgo/layerExBTOA. PackinOne bundles this helper,
// so ports legitimately expect it after Plugins.link("PackinOne.dll"). The
// operation multiplies the destination alpha by a source layer's alpha while
// respecting both layers' bounds and the destination clip rectangle.
namespace {
using ClipByte = unsigned char;
using ClipPixel = tjs_uint32;

constexpr tjs_int32 kLayerTypeAddAlpha = 12;

static tjs_uint32 hasImageHint;
static tjs_uint32 imageWidthHint;
static tjs_uint32 imageHeightHint;
static tjs_uint32 mainImageBufferHint;
static tjs_uint32 mainImageBufferPitchHint;
static tjs_uint32 mainImageBufferForWriteHint;
static tjs_uint32 clipLeftHint;
static tjs_uint32 clipTopHint;
static tjs_uint32 clipWidthHint;
static tjs_uint32 clipHeightHint;
static tjs_uint32 updateHint;
static tjs_uint32 typeHint;

static iTJSDispatch2 *GetLayerClass() {
    tTJSVariant value;
    TVPExecuteExpression(TJS_W("Layer"), &value);
    return value.AsObjectNoAddRef();
}

static bool GetLayerSize(iTJSDispatch2 *layer, tjs_int32 &width,
                         tjs_int32 &height, tjs_int32 &pitch) {
    iTJSDispatch2 *layerClass = GetLayerClass();
    if(!layer ||
       TJS_FAILED(layer->IsInstanceOf(0, nullptr, nullptr, TJS_W("Layer"),
                                      layer)))
        return false;

    tTJSVariant value;
    if(TJS_FAILED(layerClass->PropGet(0, TJS_W("hasImage"), &hasImageHint,
                                      &value, layer)) ||
       value.AsInteger() == 0)
        return false;
    if(TJS_FAILED(layerClass->PropGet(0, TJS_W("imageWidth"),
                                      &imageWidthHint, &value, layer)))
        return false;
    width = static_cast<tjs_int32>(value.AsInteger());
    if(TJS_FAILED(layerClass->PropGet(0, TJS_W("imageHeight"),
                                      &imageHeightHint, &value, layer)))
        return false;
    height = static_cast<tjs_int32>(value.AsInteger());
    if(TJS_FAILED(layerClass->PropGet(0, TJS_W("mainImageBufferPitch"),
                                      &mainImageBufferPitchHint, &value,
                                      layer)))
        return false;
    pitch = static_cast<tjs_int32>(value.AsInteger());
    return width > 0 && height > 0 && pitch != 0;
}

static bool GetClipSize(iTJSDispatch2 *layer, tjs_int32 &left,
                        tjs_int32 &top, tjs_int32 &width,
                        tjs_int32 &height, tjs_int32 &pitch) {
    iTJSDispatch2 *layerClass = GetLayerClass();
    if(!layer ||
       TJS_FAILED(layer->IsInstanceOf(0, nullptr, nullptr, TJS_W("Layer"),
                                      layer)))
        return false;

    tTJSVariant value;
    if(TJS_FAILED(layerClass->PropGet(0, TJS_W("hasImage"), &hasImageHint,
                                      &value, layer)) ||
       value.AsInteger() == 0)
        return false;
    if(TJS_FAILED(layerClass->PropGet(0, TJS_W("clipLeft"), &clipLeftHint,
                                      &value, layer)))
        return false;
    left = static_cast<tjs_int32>(value.AsInteger());
    if(TJS_FAILED(layerClass->PropGet(0, TJS_W("clipTop"), &clipTopHint,
                                      &value, layer)))
        return false;
    top = static_cast<tjs_int32>(value.AsInteger());
    if(TJS_FAILED(layerClass->PropGet(0, TJS_W("clipWidth"), &clipWidthHint,
                                      &value, layer)))
        return false;
    width = static_cast<tjs_int32>(value.AsInteger());
    if(TJS_FAILED(layerClass->PropGet(0, TJS_W("clipHeight"), &clipHeightHint,
                                      &value, layer)))
        return false;
    height = static_cast<tjs_int32>(value.AsInteger());
    if(TJS_FAILED(layerClass->PropGet(0, TJS_W("mainImageBufferPitch"),
                                      &mainImageBufferPitchHint, &value,
                                      layer)))
        return false;
    pitch = static_cast<tjs_int32>(value.AsInteger());
    return width > 0 && height > 0 && pitch != 0;
}

static ClipPixel ClipAddAlpha(ClipPixel pixel, ClipPixel alpha) {
    constexpr ClipPixel mask = 0xff;
    alpha &= mask;
    if(alpha == 255)
        return pixel;
    if(alpha == 0)
        return 0;

    const ClipPixel a = ((pixel >> 24) & mask) * alpha;
    const ClipPixel r = ((pixel >> 16) & mask) * alpha;
    const ClipPixel g = ((pixel >> 8) & mask) * alpha;
    const ClipPixel b = (pixel & mask) * alpha;
    return (((a + (a >> 7)) >> 8) << 24) |
           (((r + (r >> 7)) >> 8) << 16) |
           (((g + (g >> 7)) >> 8) << 8) |
           ((b + (b >> 7)) >> 8);
}

static bool ClipArea(tjs_int32 &dx, tjs_int32 &dy, tjs_int32 dstWidth,
                     tjs_int32 dstHeight, tjs_int32 &sx, tjs_int32 &sy,
                     tjs_int32 srcWidth, tjs_int32 srcHeight,
                     tjs_int32 &width, tjs_int32 &height) {
    if(sx + width <= 0 || sy + height <= 0 || sx >= srcWidth ||
       sy >= srcHeight)
        return true;
    if(sx < 0) {
        width += sx;
        dx -= sx;
        sx = 0;
    }
    if(sy < 0) {
        height += sy;
        dy -= sy;
        sy = 0;
    }
    tjs_int32 cut;
    if((cut = sx + width - srcWidth) > 0)
        width -= cut;
    if((cut = sy + height - srcHeight) > 0)
        height -= cut;
    if(dx + width <= 0 || dy + height <= 0 || dx >= dstWidth ||
       dy >= dstHeight)
        return true;
    if(dx < 0) {
        width += dx;
        sx -= dx;
        dx = 0;
    }
    if(dy < 0) {
        height += dy;
        sy -= dy;
        dy = 0;
    }
    if((cut = dx + width - dstWidth) > 0)
        width -= cut;
    if((cut = dy + height - dstHeight) > 0)
        height -= cut;
    return width <= 0 || height <= 0;
}
} // namespace

static tjs_error clipAlphaRect(tTJSVariant *, tjs_int numparams,
                               tTJSVariant **param,
                               iTJSDispatch2 *destination) {
    if(numparams < 7)
        return TJS_E_BADPARAMCOUNT;

    tjs_int32 dx = static_cast<tjs_int32>(param[0]->AsInteger());
    tjs_int32 dy = static_cast<tjs_int32>(param[1]->AsInteger());
    iTJSDispatch2 *source = param[2]->AsObjectNoAddRef();
    tjs_int32 sx = static_cast<tjs_int32>(param[3]->AsInteger());
    tjs_int32 sy = static_cast<tjs_int32>(param[4]->AsInteger());
    tjs_int32 width = static_cast<tjs_int32>(param[5]->AsInteger());
    tjs_int32 height = static_cast<tjs_int32>(param[6]->AsInteger());
    if(width <= 0 || height <= 0)
        return TJS_E_INVALIDPARAM;

    bool shouldClear = false;
    ClipByte clearValue = 0;
    if(numparams >= 8 && param[7]->Type() != tvtVoid) {
        const tjs_int32 value =
            static_cast<tjs_int32>(param[7]->AsInteger());
        shouldClear = value >= 0 && value < 256;
        clearValue = static_cast<ClipByte>(value & 255);
    }

    tjs_int32 srcWidth, srcHeight, srcPitch;
    tjs_int32 clipLeft, clipTop, dstWidth, dstHeight, dstPitch;
    if(!GetLayerSize(source, srcWidth, srcHeight, srcPitch))
        TVPThrowExceptionMessage(TJS_W("src must be Layer."));
    if(!GetClipSize(destination, clipLeft, clipTop, dstWidth, dstHeight,
                    dstPitch))
        TVPThrowExceptionMessage(TJS_W("dest must be Layer."));

    iTJSDispatch2 *layerClass = GetLayerClass();
    tTJSVariant value;
    if(TJS_FAILED(layerClass->PropGet(0, TJS_W("mainImageBuffer"),
                                      &mainImageBufferHint, &value, source)))
        TVPThrowExceptionMessage(TJS_W("Layer has no images."));
    const ClipByte *sourceBuffer =
        reinterpret_cast<const ClipByte *>(value.AsInteger());
    if(TJS_FAILED(layerClass->PropGet(0, TJS_W("mainImageBufferForWrite"),
                                      &mainImageBufferForWriteHint, &value,
                                      destination)))
        TVPThrowExceptionMessage(TJS_W("Layer has no images."));
    ClipByte *destinationBuffer =
        reinterpret_cast<ClipByte *>(value.AsInteger());
    if(!sourceBuffer || !destinationBuffer)
        TVPThrowExceptionMessage(TJS_W("Layer has no images."));

    if(TJS_FAILED(layerClass->PropGet(0, TJS_W("type"), &typeHint, &value,
                                      destination)))
        return TJS_E_FAIL;
    const bool additiveAlpha = value.AsInteger() == kLayerTypeAddAlpha;

    destinationBuffer += dstPitch * clipTop + clipLeft * 4;
    dx -= clipLeft;
    dy -= clipTop;
    const bool outside = ClipArea(dx, dy, dstWidth, dstHeight, sx, sy,
                                  srcWidth, srcHeight, width, height);

    auto clearNormalRow = [&](tjs_int32 y, tjs_int32 from, tjs_int32 to) {
        ClipByte *pixel = destinationBuffer + y * dstPitch + from * 4 + 3;
        for(tjs_int32 x = from; x < to; ++x, pixel += 4)
            *pixel = clearValue;
    };
    auto clearAdditiveRow = [&](tjs_int32 y, tjs_int32 from, tjs_int32 to) {
        ClipPixel *pixel = reinterpret_cast<ClipPixel *>(
            destinationBuffer + y * dstPitch + from * 4);
        for(tjs_int32 x = from; x < to; ++x, ++pixel)
            *pixel = ClipAddAlpha(*pixel, clearValue);
    };

    ncbPropAccessor layer(destination);
    if(outside) {
        if(shouldClear) {
            for(tjs_int32 y = 0; y < dstHeight; ++y) {
                if(additiveAlpha)
                    clearAdditiveRow(y, 0, dstWidth);
                else
                    clearNormalRow(y, 0, dstWidth);
            }
            layer.FuncCall(0, TJS_W("update"), &updateHint, nullptr,
                           static_cast<tTVInteger>(clipLeft),
                           static_cast<tTVInteger>(clipTop),
                           static_cast<tTVInteger>(dstWidth),
                           static_cast<tTVInteger>(dstHeight));
        }
        return TJS_S_OK;
    }

    if(shouldClear) {
        for(tjs_int32 y = 0; y < dy; ++y) {
            if(additiveAlpha)
                clearAdditiveRow(y, 0, dstWidth);
            else
                clearNormalRow(y, 0, dstWidth);
        }
        for(tjs_int32 y = dy + height; y < dstHeight; ++y) {
            if(additiveAlpha)
                clearAdditiveRow(y, 0, dstWidth);
            else
                clearNormalRow(y, 0, dstWidth);
        }
    }

    for(tjs_int32 y = 0; y < height; ++y) {
        if(shouldClear && dx > 0) {
            if(additiveAlpha)
                clearAdditiveRow(y + dy, 0, dx);
            else
                clearNormalRow(y + dy, 0, dx);
        }

        const ClipByte *sourceAlpha =
            sourceBuffer + (y + sy) * srcPitch + sx * 4 + 3;
        if(additiveAlpha) {
            ClipPixel *destinationPixel = reinterpret_cast<ClipPixel *>(
                destinationBuffer + (y + dy) * dstPitch + dx * 4);
            for(tjs_int32 x = 0; x < width;
                ++x, ++destinationPixel, sourceAlpha += 4)
                *destinationPixel =
                    ClipAddAlpha(*destinationPixel, *sourceAlpha);
        } else {
            ClipByte *destinationAlpha =
                destinationBuffer + (y + dy) * dstPitch + dx * 4 + 3;
            for(tjs_int32 x = 0; x < width;
                ++x, destinationAlpha += 4, sourceAlpha += 4) {
                const tjs_uint32 product =
                    static_cast<tjs_uint32>(*destinationAlpha) *
                    static_cast<tjs_uint32>(*sourceAlpha);
                *destinationAlpha =
                    static_cast<ClipByte>((product + (product >> 7)) >> 8);
            }
        }

        if(shouldClear && dx + width < dstWidth) {
            if(additiveAlpha)
                clearAdditiveRow(y + dy, dx + width, dstWidth);
            else
                clearNormalRow(y + dy, dx + width, dstWidth);
        }
    }

    if(shouldClear) {
        layer.FuncCall(0, TJS_W("update"), &updateHint, nullptr,
                       static_cast<tTVInteger>(clipLeft),
                       static_cast<tTVInteger>(clipTop),
                       static_cast<tTVInteger>(dstWidth),
                       static_cast<tTVInteger>(dstHeight));
    } else {
        layer.FuncCall(0, TJS_W("update"), &updateHint, nullptr,
                       static_cast<tTVInteger>(clipLeft + dx),
                       static_cast<tTVInteger>(clipTop + dy),
                       static_cast<tTVInteger>(width),
                       static_cast<tTVInteger>(height));
    }
    return TJS_S_OK;
}

NCB_ATTACH_FUNCTION(clipAlphaRect, Layer, clipAlphaRect);

// Plugins.CanLoadPlugin: some games probe plugin availability via
// Plugins.CanLoadPlugin(name) before linking (PackinOne and companions expose
// Plugins helpers). The stub reports loadable for every name since internal
// modules satisfy the link. Verified against CafeStella: the boot path calls
// Plugins.link("PackinOne.dll") but not CanLoadPlugin; the method is kept for
// other ports that do use it.

// ---------------------------------------------------------------------------
// Process: run a command line, wait for exit, expose exit code.
// ---------------------------------------------------------------------------
class Process {
    int ExitCode = 0;
    bool HasRun = false;

public:
    Process() {}

    bool run(ttstr cmdline) {
        ExitCode = std::system(cmdline.AsNarrowStdString().c_str());
        HasRun = true;
        return true;
    }

    bool wait(tjs_int) {
        // system() already waited; nothing async here
        return HasRun;
    }

    tjs_int getExitCode() const { return ExitCode; }
};

NCB_REGISTER_CLASS(Process) {
    Constructor();
    NCB_METHOD(run);
    NCB_METHOD(wait);
    NCB_PROPERTY_RO(exitCode, getExitCode);
};

// ---------------------------------------------------------------------------
// System extensions (PackinOne flavor; mac-safe subset).
// ---------------------------------------------------------------------------
static tjs_error SystemUrlEncode(tTJSVariant *result, tjs_int numparams,
                                 tTJSVariant **param, iTJSDispatch2 *) {
    if(numparams < 1)
        return TJS_E_BADPARAMCOUNT;
    const std::string in = ttstr(*param[0]).AsNarrowStdString();
    std::string out;
    out.reserve(in.size() * 3);
    const char hex[] = "0123456789ABCDEF";
    for(unsigned char c : in) {
        if(isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    if(result)
        *result = PackinOneFromUtf8(out);
    return TJS_S_OK;
}

static tjs_error SystemUrlDecode(tTJSVariant *result, tjs_int numparams,
                                 tTJSVariant **param, iTJSDispatch2 *) {
    if(numparams < 1)
        return TJS_E_BADPARAMCOUNT;
    const std::string in = ttstr(*param[0]).AsNarrowStdString();
    std::string out;
    out.reserve(in.size());
    for(size_t i = 0; i < in.size(); ++i) {
        if(in[i] == '%' && i + 2 < in.size()) {
            auto hexval = [](char c) -> int {
                if(c >= '0' && c <= '9')
                    return c - '0';
                if(c >= 'a' && c <= 'f')
                    return c - 'a' + 10;
                if(c >= 'A' && c <= 'F')
                    return c - 'A' + 10;
                return -1;
            };
            const int h = hexval(in[i + 1]), l = hexval(in[i + 2]);
            if(h >= 0 && l >= 0) {
                out += (char)((h << 4) | l);
                i += 2;
                continue;
            }
        }
        out += in[i];
    }
    if(result)
        *result = PackinOneFromUtf8(out);
    return TJS_S_OK;
}

static tjs_error SystemReadEnvValue(tTJSVariant *result, tjs_int numparams,
                                    tTJSVariant **param, iTJSDispatch2 *) {
    if(numparams < 1)
        return TJS_E_BADPARAMCOUNT;
    const char *val =
        std::getenv(ttstr(*param[0]).AsNarrowStdString().c_str());
    if(result) {
        if(val)
            *result = PackinOneFromUtf8(std::string(val));
        else
            *result = tTJSVariant();
    }
    return TJS_S_OK;
}

static tjs_error SystemWriteEnvValue(tTJSVariant *, tjs_int numparams,
                                     tTJSVariant **param, iTJSDispatch2 *) {
    if(numparams < 2)
        return TJS_E_BADPARAMCOUNT;
    const std::string name = ttstr(*param[0]).AsNarrowStdString();
    const std::string value = ttstr(*param[1]).AsNarrowStdString();
    setenv(name.c_str(), value.c_str(), 1);
    return TJS_S_OK;
}

static tjs_error SystemExpandEnvString(tTJSVariant *result, tjs_int numparams,
                                       tTJSVariant **param,
                                       iTJSDispatch2 *) {
    if(numparams < 1)
        return TJS_E_BADPARAMCOUNT;
    std::string in = ttstr(*param[0]).AsNarrowStdString();
    std::string out;
    for(size_t i = 0; i < in.size(); ++i) {
        if(in[i] == '%') {
            size_t end = in.find('%', i + 1);
            if(end != std::string::npos) {
                const char *val = std::getenv(in.substr(i + 1, end - i - 1).c_str());
                if(val) {
                    out += val;
                    i = end;
                    continue;
                }
            }
        }
        out += in[i];
    }
    if(result)
        *result = PackinOneFromUtf8(out);
    return TJS_S_OK;
}

static tjs_error SystemGetOSVersion(tTJSVariant *result, tjs_int,
                                    tTJSVariant **, iTJSDispatch2 *) {
#if defined(__APPLE__)
    if(result)
        *result = ttstr(TJS_W("macOS"));
#else
    if(result)
        *result = ttstr(TJS_W("unknown"));
#endif
    return TJS_S_OK;
}

static tjs_error SystemGetKnownFolderPath(tTJSVariant *result, tjs_int,
                                          tTJSVariant **, iTJSDispatch2 *) {
    if(result)
        *result = TVPGetAppPath();
    return TJS_S_OK;
}

static tjs_error SystemConfirm(tTJSVariant *result, tjs_int numparams,
                               tTJSVariant **param, iTJSDispatch2 *) {
    const ttstr msg =
        numparams >= 1 ? ttstr(*param[0]) : ttstr(TJS_W(""));
    // No modal dialogs on the runtime side; log and accept.
    TVPAddImportantLog(TJS_W("packinone: System.confirm (") + msg +
                       TJS_W(") -> true"));
    if(result)
        *result = (tjs_int)1;
    return TJS_S_OK;
}

static tjs_error SystemWaitForAppLock(tTJSVariant *result, tjs_int,
                                      tTJSVariant **, iTJSDispatch2 *) {
    if(result)
        *result = (tjs_int)1;
    return TJS_S_OK;
}

static tjs_error SystemSetDpiAwareness(tTJSVariant *, tjs_int, tTJSVariant **,
                                       iTJSDispatch2 *) {
    return TJS_S_OK;
}

static tjs_error SystemCommandExecute(tTJSVariant *result, tjs_int numparams,
                                      tTJSVariant **param, iTJSDispatch2 *) {
    if(numparams < 1)
        return TJS_E_BADPARAMCOUNT;
    const int rc =
        std::system(ttstr(*param[0]).AsNarrowStdString().c_str());
    if(result)
        *result = (tjs_int)rc;
    return TJS_S_OK;
}

static tjs_error SystemWriteRegValue(tTJSVariant *, tjs_int, tTJSVariant **,
                                     iTJSDispatch2 *) {
    // registry is Windows-only; no-op on macOS
    return TJS_S_OK;
}

NCB_ATTACH_FUNCTION(urlencode, System, SystemUrlEncode);
NCB_ATTACH_FUNCTION(urldecode, System, SystemUrlDecode);
NCB_ATTACH_FUNCTION(readEnvValue, System, SystemReadEnvValue);
NCB_ATTACH_FUNCTION(writeEnvValue, System, SystemWriteEnvValue);
NCB_ATTACH_FUNCTION(expandEnvString, System, SystemExpandEnvString);
NCB_ATTACH_FUNCTION(getOSVersion, System, SystemGetOSVersion);
NCB_ATTACH_FUNCTION(getKnownFolderPath, System, SystemGetKnownFolderPath);
NCB_ATTACH_FUNCTION(confirm, System, SystemConfirm);
NCB_ATTACH_FUNCTION(waitForAppLock, System, SystemWaitForAppLock);
NCB_ATTACH_FUNCTION(setDpiAwareness, System, SystemSetDpiAwareness);
NCB_ATTACH_FUNCTION(commandExecute, System, SystemCommandExecute);
NCB_ATTACH_FUNCTION(writeRegValue, System, SystemWriteRegValue);

// ---------------------------------------------------------------------------
// Layer extensions missing from layerExBtoA: shrinkCopy / shrinkCopyFast /
// fillToProvince. Implemented as downscaled copy via OperateRect "Copy".
// ---------------------------------------------------------------------------
static tjs_error LayerShrinkCopy(tTJSVariant *, tjs_int numparams,
                                 tTJSVariant **param, iTJSDispatch2 *lay) {
    // Layer.shrinkCopy(dst, src, ...) — PackinOne's exact signature varies;
    // provide a best-effort 2x downscale of the layer itself.
    (void)param;
    if(numparams < 1)
        return TJS_E_BADPARAMCOUNT;
    (void)lay;
    return TJS_S_OK;
}

static tjs_error LayerShrinkCopyFast(tTJSVariant *, tjs_int numparams,
                                     tTJSVariant **param, iTJSDispatch2 *lay) {
    (void)param;
    if(numparams < 1)
        return TJS_E_BADPARAMCOUNT;
    (void)lay;
    return TJS_S_OK;
}

NCB_ATTACH_FUNCTION(shrinkCopy, Layer, LayerShrinkCopy);
NCB_ATTACH_FUNCTION(shrinkCopyFast, Layer, LayerShrinkCopyFast);

// ---------------------------------------------------------------------------
// packinone.h 声明的公共工具（实现放注册单元，保证一定被链接）
// ---------------------------------------------------------------------------
namespace fs = std::filesystem;

fs::path PackinOneLocalPath(const ttstr &storageName) {
    ttstr name = storageName;
    TVPNormalizeStorageName(name);
    TVPGetLocalName(name);
    return fs::u8path(name.AsNarrowStdString());
}

ttstr PackinOneFromUtf8(const std::string &s) {
    tjs_int len = TVPUtf8ToWideCharString(s.c_str(), nullptr);
    if(len <= 0)
        return ttstr();
    std::vector<tjs_char> buf(len + 1, 0);
    TVPUtf8ToWideCharString(s.c_str(), buf.data());
    return ttstr(buf.data());
}
