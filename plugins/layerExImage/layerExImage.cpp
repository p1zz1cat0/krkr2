// layerExImage.dll — standalone wtnbgo/layerExImage compatibility module.
//
// Games call Layer.light/colorize/modulate/noise/generateWhiteNoise/
// gaussianBlur after Plugins.link("layerExImage.dll") directly, without
// bundling PackinOne.  The upstream reference (wamsoft layerExImage) attaches
// the same six members to Layer via a class registration; a second class
// registration under a different module name would throw
// "Already registerd class:" when both PackinOne.dll and layerExImage.dll are
// linked, so this module uses plain function registration instead — repeat
// registration overwrites the earlier member and stays benign.
//
// Pixel algorithms are shared with the PackinOne surface through
// common/LayerExImageOps.h; parameter semantics follow the upstream
// manual.tjs (light: -255..255/-100..100, colorize: hue/sat/blend 0..1,
// modulate: hue -180..180/sat/lum -100..100, noise: 0..255, gaussianBlur:
// float radius with the same 64.0 resource-safety cap as PackinOne).

#include "common/LayerExImageOps.h"
#include "common/PluginSafety.h"
#include "MsgIntf.h"
#include "ncbind.hpp"

#include <spdlog/spdlog.h>

#define NCB_MODULE_NAME TJS_W("layerExImage.dll")

extern "C" void TVPLayerExImagePluginAnchor() {}

namespace {

using layerExImageOps::ImageMutableView;

// Same resource-safety cap as the PackinOne surface
// (packinone::layerExImage::kMaxGaussianBlurRadius); not part of the
// upstream ABI.
constexpr double kMaxGaussianBlurRadius = 64.0;

// Functions attached to Layer resolve the clip rectangle on every call (the
// upstream class cached it per instance via reset(); stateless functions
// must re-read it each time to stay correct across clip changes).
bool GetClipAdjustedView(iTJSDispatch2 *layer, ImageMutableView &view) {
    const auto writeView = pluginSafety::LayerWriteView::create(layer);
    if(!writeView)
        return false;

    iTJSDispatch2 *layerClass = nullptr;
    {
        tTJSVariant value;
        TVPExecuteExpression(TJS_W("Layer"), &value);
        if(value.Type() != tvtObject)
            return false;
        layerClass = value.AsObjectNoAddRef();
        if(!layerClass)
            return false;
    }

    tTJSVariant value;
    if(TJS_FAILED(layerClass->PropGet(0, TJS_W("clipLeft"), nullptr, &value,
                                      layer)))
        return false;
    const auto clipLeft = value.AsInteger();
    if(TJS_FAILED(layerClass->PropGet(0, TJS_W("clipTop"), nullptr, &value,
                                      layer)))
        return false;
    const auto clipTop = value.AsInteger();
    if(TJS_FAILED(layerClass->PropGet(0, TJS_W("clipWidth"), nullptr, &value,
                                      layer)))
        return false;
    const auto clipWidth = value.AsInteger();
    if(TJS_FAILED(layerClass->PropGet(0, TJS_W("clipHeight"), nullptr, &value,
                                      layer)))
        return false;
    const auto clipHeight = value.AsInteger();

    if(clipLeft < 0 || clipTop < 0 || clipWidth <= 0 || clipHeight <= 0 ||
       static_cast<tjs_int64>(clipLeft) + clipWidth > writeView.value.width() ||
       static_cast<tjs_int64>(clipTop) + clipHeight > writeView.value.height())
        return false;

    view.buffer = writeView.value.pixels() +
        static_cast<std::ptrdiff_t>(clipTop) * writeView.value.pitchBytes() +
        static_cast<std::ptrdiff_t>(clipLeft) * 4;
    view.width = static_cast<int>(clipWidth);
    view.height = static_cast<int>(clipHeight);
    view.pitchBytes = writeView.value.pitchBytes();
    return true;
}

void ThrowInvalidLayer() {
    TVPThrowExceptionMessage(
        TJS_W("layerExImage requires a valid Layer image."));
}

bool ValidateBlurRadius(double raw, float &normalized) {
    // Finite negative values keep the reference implementation's fabs() rule.
    if(!std::isfinite(raw) || std::fabs(raw) > kMaxGaussianBlurRadius)
        return false;
    const float narrowed = static_cast<float>(raw);
    if(!std::isfinite(narrowed))
        return false;
    normalized = static_cast<float>(std::fabs(narrowed));
    return std::isfinite(normalized);
}

void RedrawClip(iTJSDispatch2 *layer) {
    ncbPropAccessor layerObj(layer);
    layerObj.FuncCall(0, TJS_W("update"), nullptr, nullptr,
                      layerObj.getIntValue(TJS_W("clipLeft")),
                      layerObj.getIntValue(TJS_W("clipTop")),
                      layerObj.getIntValue(TJS_W("clipWidth")),
                      layerObj.getIntValue(TJS_W("clipHeight")));
}

tjs_error Light(tTJSVariant *, tjs_int numparams, tTJSVariant **param,
                iTJSDispatch2 *layer) {
    if(numparams < 2)
        return TJS_E_BADPARAMCOUNT;
    ImageMutableView view;
    if(!GetClipAdjustedView(layer, view))
        ThrowInvalidLayer();
    layerExImageOps::ApplyLight(
        view, static_cast<int>(param[0]->AsInteger()),
        static_cast<int>(param[1]->AsInteger()));
    RedrawClip(layer);
    return TJS_S_OK;
}

tjs_error Colorize(tTJSVariant *, tjs_int numparams, tTJSVariant **param,
                   iTJSDispatch2 *layer) {
    if(numparams < 3)
        return TJS_E_BADPARAMCOUNT;
    ImageMutableView view;
    if(!GetClipAdjustedView(layer, view))
        ThrowInvalidLayer();
    layerExImageOps::ApplyColorize(
        view, static_cast<int>(param[0]->AsInteger()),
        static_cast<int>(param[1]->AsInteger()), param[2]->AsReal());
    RedrawClip(layer);
    return TJS_S_OK;
}

tjs_error Modulate(tTJSVariant *, tjs_int numparams, tTJSVariant **param,
                   iTJSDispatch2 *layer) {
    if(numparams < 3)
        return TJS_E_BADPARAMCOUNT;
    ImageMutableView view;
    if(!GetClipAdjustedView(layer, view))
        ThrowInvalidLayer();
    layerExImageOps::ApplyModulate(
        view, static_cast<int>(param[0]->AsInteger()),
        static_cast<int>(param[1]->AsInteger()),
        static_cast<int>(param[2]->AsInteger()));
    RedrawClip(layer);
    return TJS_S_OK;
}

tjs_error Noise(tTJSVariant *, tjs_int numparams, tTJSVariant **param,
                iTJSDispatch2 *layer) {
    if(numparams < 1)
        return TJS_E_BADPARAMCOUNT;
    ImageMutableView view;
    if(!GetClipAdjustedView(layer, view))
        ThrowInvalidLayer();
    layerExImageOps::ApplyNoise(view,
                                static_cast<int>(param[0]->AsInteger()));
    RedrawClip(layer);
    return TJS_S_OK;
}

tjs_error GenerateWhiteNoise(tTJSVariant *, tjs_int, tTJSVariant **,
                             iTJSDispatch2 *layer) {
    ImageMutableView view;
    if(!GetClipAdjustedView(layer, view))
        ThrowInvalidLayer();
    layerExImageOps::ApplyGenerateWhiteNoise(view);
    RedrawClip(layer);
    return TJS_S_OK;
}

tjs_error GaussianBlur(tTJSVariant *, tjs_int numparams, tTJSVariant **param,
                       iTJSDispatch2 *layer) {
    if(numparams < 1)
        return TJS_E_BADPARAMCOUNT;
    if(param[0]->Type() != tvtInteger && param[0]->Type() != tvtReal)
        return TJS_E_INVALIDTYPE;

    float radius = 0.0f;
    if(!ValidateBlurRadius(param[0]->AsReal(), radius)) {
        if(auto logger = spdlog::get("plugin"))
            logger->warn("[layerExImage] gaussianBlur rejected non-finite, "
                         "out-of-range, or unsafe input");
        return TJS_E_INVALIDPARAM;
    }

    ImageMutableView view;
    if(!GetClipAdjustedView(layer, view))
        ThrowInvalidLayer();
    if(!layerExImageOps::ApplyGaussianBlur(view, radius)) {
        if(auto logger = spdlog::get("plugin"))
            logger->warn("[layerExImage] gaussianBlur failed to blur safely");
        return TJS_E_FAIL;
    }
    RedrawClip(layer);
    return TJS_S_OK;
}

} // namespace

NCB_ATTACH_FUNCTION(light, Layer, Light);
NCB_ATTACH_FUNCTION(colorize, Layer, Colorize);
NCB_ATTACH_FUNCTION(modulate, Layer, Modulate);
NCB_ATTACH_FUNCTION(noise, Layer, Noise);
NCB_ATTACH_FUNCTION(generateWhiteNoise, Layer, GenerateWhiteNoise);
NCB_ATTACH_FUNCTION(gaussianBlur, Layer, GaussianBlur);
