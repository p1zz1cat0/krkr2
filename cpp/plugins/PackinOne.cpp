// Compatibility stub for PackinOne.dll on native KrKr2.
//
// Many mobile/PC fan ports ship PackinOne.dll as a Windows plugin. Scripts call
// Plugins.link("PackinOne.dll") as a feature gate and expect a bundle of helpers
// (fstat/saveStruct/layerExMovie, AffineSourceMovie, etc.). Yoghourt games have
// no Windows .exe, so this internal module satisfies the link without Wine.
//
// Adapted from krkrsdl3 / KiriKiri-LauncherC PackinOne compatibility plugins.

#include "ncbind.hpp"
#include "ScriptMgnIntf.h"

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
static tjs_error CanLoadPlugin(tTJSVariant *result, tjs_int numparams,
                               tTJSVariant **, iTJSDispatch2 *) {
    if(numparams < 1)
        return TJS_E_BADPARAMCOUNT;
    if(result)
        *result = (tjs_int)1;
    return TJS_S_OK;
}

NCB_ATTACH_FUNCTION(CanLoadPlugin, Plugins, CanLoadPlugin);

static void InitPlugin_PackinOne() {
    // Best-effort companion plugins already built into KrKr2.
    ncbAutoRegister::LoadModule(TJS_W("fstat.dll"));
    ncbAutoRegister::LoadModule(TJS_W("saveStruct.dll"));
    ncbAutoRegister::LoadModule(TJS_W("ScriptsEx.dll"));
    ncbAutoRegister::LoadModule(TJS_W("csvParser.dll"));
    ncbAutoRegister::LoadModule(TJS_W("layerExMovie.dll"));
    ncbAutoRegister::LoadModule(TJS_W("addFont.dll"));
    ncbAutoRegister::LoadModule(TJS_W("dirlist.dll"));

    if(HasGlobalMember(TJS_W("AffineSource")) &&
       !HasGlobalMember(TJS_W("AffineSourceMovie"))) {
        try {
            TVPExecuteScript(TJS_W(
                "class AffineSourceMovie extends AffineSource {"
                "  var _movie;"
                "  var _width = 0;"
                "  var _height = 0;"
                "  var _lastOwner = true;"
                "  function AffineSourceMovie(window) {"
                "    super.AffineSource(window);"
                "  }"
                "  function createLayer(orig=void) {"
                "    var src = new global.Layer(_window, _pool);"
                "    if (orig != void) {"
                "      src.assignImages(orig);"
                "      src.width = orig.width;"
                "      src.height = orig.height;"
                "      src.scale = orig.scale;"
                "    } else {"
                "      src.scale = 1.0;"
                "    }"
                "    return src;"
                "  }"
                "  function finalize() {"
                "    if (_lastOwner) {"
                "      clear();"
                "      invalidate _movie;"
                "    }"
                "  }"
                "  function clear() {"
                "    notifyOwner(\"onMotionStop\");"
                "    onMovieStop();"
                "    if (typeof kag != \"undefined\" && kag !== void)"
                "      kag.conductor.trigger(\"movie_world_foremovie\");"
                "  }"
                "  function clone(newwindow, instance) {"
                "    if (newwindow == void) {"
                "      newwindow = _window;"
                "    }"
                "    if (instance == void) {"
                "      instance = new global.AffineSourceMovie(newwindow);"
                "    }"
                "    instance._movie = _movie;"
                "    instance._width = _width;"
                "    instance._height = _height;"
                "    _lastOwner = false;"
                "    super.clone(newwindow, instance);"
                "    return instance;"
                "  }"
                "  function canWaitMovie() {"
                "    return _movie.isPlayingMovie();"
                "  }"
                "  function isFlip() {"
                "    if (_movie.isPlayingMovie()) {"
                "      return true;"
                "    }"
                "    clear();"
                "    return false;"
                "  }"
                "  function stopMovie() {"
                "    _movie.stopMovie();"
                "  }"
                "  function drawAffine(target, mtx, src) {"
                "    (global.Layer.copyRect incontextof target)("
                "      0, 0, _movie, 0, 0, _width, _height);"
                "  }"
                "  function loadImages(storage, colorKey=clNone, options=void) {"
                "    _movie = createLayer();"
                "    _movie.openMovie(storage, false);"
                "    _movie.setSizeToImageSize();"
                "    _width = _movie.width;"
                "    _height = _movie.height;"
                "    _movie.startMovie(false);"
                "  }"
                "};"
            ));
        } catch(...) {
        }
    }

    if(!HasGlobalMember(TJS_W("AffineSourceMovie")))
        return;

    try {
        TVPExecuteScript(TJS_W(
            "if (global.extSourceMap === void) {"
            "  global.extSourceMap = %[];"
            "}"
            "extSourceMap[\".WMV\"] = AffineSourceMovie;"
            "extSourceMap[\".MPG\"] = AffineSourceMovie;"
            "extSourceMap[\".MPEG\"] = AffineSourceMovie;"
        ));
    } catch(...) {
    }
}

NCB_PRE_REGIST_CALLBACK(InitPlugin_PackinOne);
