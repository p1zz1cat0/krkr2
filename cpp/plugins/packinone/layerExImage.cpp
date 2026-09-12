// PackinOne.dll layerExImage compatibility surface.
//
// The six methods in this file are the only layerExImage members confirmed by
// the latest PackinOne fixture.  The pixel algorithms live in
// common/LayerExImageOps.h, shared with the standalone layerExImage.dll
// registration surface; both derive from the official krkrz/layerExImage
// reference and the CxImage 7.0.2-derived routines it carries.  CxImage
// notices are retained in the source tree's third-party notice; this file
// does not import the old Windows codec stack.

#include "layerExImage.h"

#include "common/LayerExImageOps.h"
#include "layerExBase.hpp"
#include "MsgIntf.h"
#include "ncbind.hpp"

#include <spdlog/spdlog.h>

#define NCB_MODULE_NAME TJS_W("packinone.dll")

namespace packinone::layerExImage {

bool ValidateGaussianBlurRadius(double raw, float &normalized) {
    if(!std::isfinite(raw) || std::fabs(raw) > kMaxGaussianBlurRadius)
        return false;

    const float narrowed = static_cast<float>(raw);
    if(!std::isfinite(narrowed))
        return false;

    normalized = static_cast<float>(std::fabs(narrowed));
    return std::isfinite(normalized);
}

void GenerateWhiteNoise(unsigned char *buffer, int width, int height,
                        std::ptrdiff_t pitch) {
    layerExImageOps::GenerateWhiteNoise(buffer, width, height, pitch);
}

} // namespace packinone::layerExImage

namespace {

class layerExImage : public layerExBase {
public:
    explicit layerExImage(DispatchT object) : layerExBase(object) {}

    void reset() override {
        layerExBase::reset();
        _buffer += static_cast<std::ptrdiff_t>(_clipTop) * _pitch +
                   static_cast<std::ptrdiff_t>(_clipLeft) * 4;
        _width = _clipWidth;
        _height = _clipHeight;
    }

    bool IsValidImage() const {
        // layerExBase exposes the first (top) scanline.  The native Layer
        // implementation is top-down, so reject a negative pitch instead of
        // walking backwards from an unnormalised origin.
        if(!_buffer || _width <= 0 || _height <= 0 || _pitch <= 0)
            return false;
        const std::int64_t rowBytes = static_cast<std::int64_t>(_width) * 4;
        const std::int64_t pitch = static_cast<std::int64_t>(_pitch);
        return rowBytes > 0 && pitch >= rowBytes;
    }

    layerExImageOps::ImageMutableView View() const {
        return layerExImageOps::ImageMutableView{
            _buffer, _width, _height, _pitch};
    }

    void light(int brightness, int contrast) {
        if(!IsValidImage())
            ThrowInvalidImage();
        layerExImageOps::ApplyLight(View(), brightness, contrast);
        redraw();
    }

    void colorize(int hue, int saturation, double blend) {
        if(!IsValidImage())
            ThrowInvalidImage();
        layerExImageOps::ApplyColorize(View(), hue, saturation, blend);
        redraw();
    }

    void modulate(int hue, int saturation, int luminance) {
        if(!IsValidImage())
            ThrowInvalidImage();
        layerExImageOps::ApplyModulate(View(), hue, saturation, luminance);
        redraw();
    }

    void noise(int level) {
        if(!IsValidImage())
            ThrowInvalidImage();
        layerExImageOps::ApplyNoise(View(), level);
        redraw();
    }

    void generateWhiteNoise() {
        if(!IsValidImage())
            ThrowInvalidImage();
        layerExImageOps::ApplyGenerateWhiteNoise(View());
        redraw();
    }

    tjs_error gaussianBlur(double rawRadius) {
        float radius = 0.0f;
        if(!packinone::layerExImage::ValidateGaussianBlurRadius(rawRadius,
                                                                   radius)) {
            LogRejectedBlur();
            return TJS_E_INVALIDPARAM;
        }
        if(!IsValidImage()) {
            LogRejectedBlur();
            return TJS_E_INVALIDPARAM;
        }

        if(!layerExImageOps::ApplyGaussianBlur(View(), radius)) {
            LogRejectedBlur();
            return TJS_E_FAIL;
        }
        redraw();
        return TJS_S_OK;
    }

private:
    static void ThrowInvalidImage() {
        TVPThrowExceptionMessage(
            TJS_W("PackinOne layerExImage requires a valid Layer image."));
    }

    static void LogRejectedBlur() {
        if(auto logger = spdlog::get("plugin"))
            logger->warn("[packinone] layerExImage.gaussianBlur rejected "
                         "non-finite, out-of-range, or unsafe input");
    }
};

void EnsureLayerClassRegistered() {
    tTJSVariant layerClass;
    TVPExecuteExpression(TJS_W("Layer"), &layerClass);
    if(layerClass.Type() != tvtObject)
        TVPThrowExceptionMessage(
            TJS_W("PackinOne layerExImage requires the Layer class."));
}

tjs_error GaussianBlurCallback(tTJSVariant *, tjs_int numparams,
                               tTJSVariant **param,
                               layerExImage *nativeInstance) {
    if(numparams < 1 || !param || !param[0] || !nativeInstance)
        return numparams < 1 ? TJS_E_BADPARAMCOUNT : TJS_E_INVALIDPARAM;

    if(param[0]->Type() != tvtInteger && param[0]->Type() != tvtReal)
        return TJS_E_INVALIDTYPE;

    const double rawRadius = static_cast<double>(param[0]->AsReal());
    return nativeInstance->gaussianBlur(rawRadius);
}

} // namespace

extern "C" void TVPPackinOneLayerExImageAnchor() {}

NCB_PRE_REGIST_CALLBACK(EnsureLayerClassRegistered);

NCB_GET_INSTANCE_HOOK(layerExImage) {
    NCB_INSTANCE_GETTER(objthis) {
        ClassT *instance = GetNativeInstance(objthis);
        if(!instance) {
            instance = new ClassT(objthis);
            SetNativeInstance(objthis, instance);
        }
        instance->reset();
        return instance;
    }
    ~NCB_GET_INSTANCE_HOOK_CLASS() {}
};

NCB_ATTACH_CLASS_WITH_HOOK(layerExImage, Layer) {
    NCB_METHOD(light);
    NCB_METHOD(colorize);
    NCB_METHOD(modulate);
    NCB_METHOD(noise);
    NCB_METHOD(generateWhiteNoise);
    NCB_METHOD_RAW_CALLBACK(gaussianBlur, GaussianBlurCallback, 0);
}
