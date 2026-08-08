// Native reimplementation of Windows extrans.dll transition providers.
// Registers as "extrans.dll" via ncbind so Plugins.link("extrans.dll") succeeds
// and TVPFindTransHandlerProvider finds wave/turn/mosaic/etc.
//
// Effects are real (not crossfade aliases): geometric wipes, scanline wave,
// mosaic blocks, radial/angular reveals, rotate-zoom style blends — all through
// the modern texture OperateRect path used by built-in CrossFade.

#include "ncbind.hpp"

#include "tjsCommHead.h"
#include "TransIntf.h"
#include "transhandler.h"
#include "MsgIntf.h"
#include "DebugIntf.h"
#include "tvpgl.h"
#include "RenderManager.h"
#include "LayerBitmapIntf.h"

#include <algorithm>
#include <cmath>
#include <vector>

#define NCB_MODULE_NAME TJS_W("extrans.dll")

namespace {

constexpr double kPi = 3.14159265358979323846;

static tjs_int64 ReadNumber(iTVPSimpleOptionProvider *options,
                            const tjs_char *name, tjs_int64 def) {
    if(!options)
        return def;
    tjs_int64 v = def;
    if(TJS_SUCCEEDED(options->GetAsNumber(name, &v)))
        return v;
    return def;
}

static double ReadReal(iTVPSimpleOptionProvider *options, const tjs_char *name,
                       double def) {
    if(!options)
        return def;
    tTJSVariant val;
    if(TJS_SUCCEEDED(options->GetValue(name, &val))) {
        if(val.Type() != tvtVoid)
            return (double)(tTVReal)val;
    }
    tjs_int64 n = 0;
    if(TJS_SUCCEEDED(options->GetAsNumber(name, &n)))
        return (double)n;
    return def;
}

static void CopyRectTex(iTVPTexture2D *dest, tjs_int dl, tjs_int dt,
                        iTVPTexture2D *src, tjs_int sl, tjs_int st, tjs_int w,
                        tjs_int h) {
    if(w <= 0 || h <= 0 || !dest || !src)
        return;
    static iTVPRenderMethod *method =
        TVPGetRenderManager()->GetRenderMethod("Copy");
    tRenderTexRectArray::Element src_tex[] = { tRenderTexRectArray::Element(
        src, tTVPRect(sl, st, sl + w, st + h)) };
    TVPGetRenderManager()->OperateRect(
        method, dest, nullptr, tTVPRect(dl, dt, dl + w, dt + h),
        tRenderTexRectArray(src_tex));
}

// Stretch a 1×1 (or small) source sample across a destination block.
static void StretchCopyTex(iTVPTexture2D *dest, tjs_int dl, tjs_int dt,
                           tjs_int dw, tjs_int dh, iTVPTexture2D *src,
                           tjs_int sl, tjs_int st, tjs_int sw, tjs_int sh) {
    if(dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0 || !dest || !src)
        return;
    static iTVPRenderMethod *method =
        TVPGetRenderManager()->GetRenderMethod("Copy");
    tRenderTexRectArray::Element src_tex[] = { tRenderTexRectArray::Element(
        src, tTVPRect(sl, st, sl + sw, st + sh)) };
    TVPGetRenderManager()->OperateRect(
        method, dest, nullptr, tTVPRect(dl, dt, dl + dw, dt + dh),
        tRenderTexRectArray(src_tex));
}

static void AlphaBlendRect(iTVPTexture2D *dest, tjs_int dl, tjs_int dt,
                           iTVPTexture2D *src1, tjs_int s1l, tjs_int s1t,
                           iTVPTexture2D *src2, tjs_int s2l, tjs_int s2t,
                           tjs_int w, tjs_int h, tjs_int opa,
                           tTVPLayerType layerType) {
    if(w <= 0 || h <= 0 || !dest || !src1 || !src2)
        return;
    if(opa <= 0) {
        CopyRectTex(dest, dl, dt, src1, s1l, s1t, w, h);
        return;
    }
    if(opa >= 255) {
        CopyRectTex(dest, dl, dt, src2, s2l, s2t, w, h);
        return;
    }
    iTVPRenderMethod *method;
    int opa_id;
    if(TVPIsTypeUsingAlpha(layerType)) {
        static iTVPRenderMethod *_method =
            TVPGetRenderManager()->GetRenderMethod("ConstAlphaBlend_SD_d");
        static int _opa_id = _method->EnumParameterID("opacity");
        method = _method;
        opa_id = _opa_id;
    } else if(TVPIsTypeUsingAddAlpha(layerType)) {
        static iTVPRenderMethod *_method =
            TVPGetRenderManager()->GetRenderMethod("ConstAlphaBlend_SD_a");
        static int _opa_id = _method->EnumParameterID("opacity");
        method = _method;
        opa_id = _opa_id;
    } else {
        static iTVPRenderMethod *_method =
            TVPGetRenderManager()->GetRenderMethod("ConstAlphaBlend_SD");
        static int _opa_id = _method->EnumParameterID("opacity");
        method = _method;
        opa_id = _opa_id;
    }
    method->SetParameterOpa(opa_id, opa);
    tRenderTexRectArray::Element src_tex[] = {
        tRenderTexRectArray::Element(src1,
                                     tTVPRect(s1l, s1t, s1l + w, s1t + h)),
        tRenderTexRectArray::Element(src2,
                                     tTVPRect(s2l, s2t, s2l + w, s2t + h))
    };
    TVPGetRenderManager()->OperateRect(
        method, dest, nullptr, tTVPRect(dl, dt, dl + w, dt + h),
        tRenderTexRectArray(src_tex));
}

static void AlphaBlendScaled(iTVPTexture2D *dest, const tTVPRect &destRect,
                             iTVPTexture2D *src1, const tTVPRect &src1Rect,
                             iTVPTexture2D *src2, const tTVPRect &src2Rect,
                             tjs_int opa, tTVPLayerType layerType) {
    if(!dest || !src1 || !src2 || destRect.get_width() <= 0 ||
       destRect.get_height() <= 0)
        return;
    if(opa <= 0) {
        StretchCopyTex(dest, destRect.left, destRect.top, destRect.get_width(),
                       destRect.get_height(), src1, src1Rect.left, src1Rect.top,
                       src1Rect.get_width(), src1Rect.get_height());
        return;
    }
    if(opa >= 255) {
        StretchCopyTex(dest, destRect.left, destRect.top, destRect.get_width(),
                       destRect.get_height(), src2, src2Rect.left, src2Rect.top,
                       src2Rect.get_width(), src2Rect.get_height());
        return;
    }
    iTVPRenderMethod *method;
    int opa_id;
    if(TVPIsTypeUsingAlpha(layerType)) {
        static iTVPRenderMethod *_method =
            TVPGetRenderManager()->GetRenderMethod("ConstAlphaBlend_SD_d");
        static int _opa_id = _method->EnumParameterID("opacity");
        method = _method;
        opa_id = _opa_id;
    } else if(TVPIsTypeUsingAddAlpha(layerType)) {
        static iTVPRenderMethod *_method =
            TVPGetRenderManager()->GetRenderMethod("ConstAlphaBlend_SD_a");
        static int _opa_id = _method->EnumParameterID("opacity");
        method = _method;
        opa_id = _opa_id;
    } else {
        static iTVPRenderMethod *_method =
            TVPGetRenderManager()->GetRenderMethod("ConstAlphaBlend_SD");
        static int _opa_id = _method->EnumParameterID("opacity");
        method = _method;
        opa_id = _opa_id;
    }
    method->SetParameterOpa(opa_id, opa);
    tRenderTexRectArray::Element src_tex[] = {
        tRenderTexRectArray::Element(src1, src1Rect),
        tRenderTexRectArray::Element(src2, src2Rect)
    };
    TVPGetRenderManager()->OperateRect(method, dest, nullptr, destRect,
                                       tRenderTexRectArray(src_tex));
}

enum class ExEffect {
    Wave,
    Ripple,
    Mosaic,
    Twist,
    TwistAccel,
    Turn,
    RotateZoom,
    RotateSwap,
    RotateVanish
};

class tExtransHandler : public iTVPDivisibleTransHandler {
protected:
    tjs_int RefCount = 1;
    iTVPSimpleOptionProvider *Options = nullptr;
    tTVPLayerType DestLayerType = ltOpaque;
    tjs_uint64 StartTick = 0;
    tjs_uint64 Time = 2;
    tjs_int PhaseMax = 1000;
    tjs_int Phase = 0;
    bool First = true;
    ExEffect Effect = ExEffect::Wave;
    tjs_int Width = 0;
    tjs_int Height = 0;
    double Factor = 1.0;
    double Accel = 0.0;
    double Twist = 1.0;
    double TwistAccel = 0.0;
    tjs_int CenterX = 0;
    tjs_int CenterY = 0;
    tjs_int WaveType = 0;
    tjs_int Reverse = 0;
    tTVPBaseTexture *Mask = nullptr;
    tTVPBaseTexture *Mosaic1 = nullptr;
    tTVPBaseTexture *Mosaic2 = nullptr;
    tjs_int MosaicWidth = 0;
    tjs_int MosaicHeight = 0;
    std::vector<tjs_uint32> MaskPixels;
    iTVPRenderMethod *MaskMethod = nullptr;
    int MaskPhaseID = 0;
    int MaskVagueID = 0;

public:
    tExtransHandler(iTVPSimpleOptionProvider *options, tTVPLayerType layertype,
                    tjs_uint64 time, ExEffect effect, tjs_int w, tjs_int h) :
        Options(options), DestLayerType(layertype), Time(time), Effect(effect),
        Width(w), Height(h) {
        if(Options)
            Options->AddRef();
        if(Time < 2)
            Time = 2;
        Factor = ReadReal(Options, TJS_W("factor"), 1.0);
        Accel = ReadReal(Options, TJS_W("accel"), 0.0);
        Twist = ReadReal(Options, TJS_W("twist"), 1.0);
        TwistAccel = ReadReal(Options, TJS_W("twistaccel"), 0.0);
        CenterX = (tjs_int)ReadNumber(Options, TJS_W("centerx"), w / 2);
        CenterY = (tjs_int)ReadNumber(Options, TJS_W("centery"), h / 2);
        WaveType = (tjs_int)ReadNumber(Options, TJS_W("wavetype"), 0);
        Reverse = (tjs_int)ReadNumber(Options, TJS_W("reverse"), 0);
        if(TVPIsTypeUsingAlpha(DestLayerType)) {
            MaskMethod =
                TVPGetRenderManager()->GetRenderMethod("UnivTransBlend_d");
        } else if(TVPIsTypeUsingAddAlpha(DestLayerType)) {
            MaskMethod =
                TVPGetRenderManager()->GetRenderMethod("UnivTransBlend_a");
        } else {
            MaskMethod =
                TVPGetRenderManager()->GetRenderMethod("UnivTransBlend");
        }
        MaskPhaseID = MaskMethod->EnumParameterID("phase");
        MaskVagueID = MaskMethod->EnumParameterID("vague");
    }

    virtual ~tExtransHandler() {
        delete Mosaic2;
        delete Mosaic1;
        delete Mask;
        if(Options)
            Options->Release();
    }

    tjs_error AddRef() override {
        RefCount++;
        return TJS_S_OK;
    }
    tjs_error Release() override {
        if(RefCount == 1)
            delete this;
        else
            RefCount--;
        return TJS_S_OK;
    }
    tjs_error SetOption(iTVPSimpleOptionProvider *options) override {
        if(Options)
            Options->Release();
        Options = options;
        if(Options)
            Options->AddRef();
        return TJS_S_OK;
    }

    tjs_error StartProcess(tjs_uint64 tick) override {
        if(First) {
            First = false;
            StartTick = tick;
        }
        double t = (double)(tick - StartTick) / (double)Time;
        if(t < 0)
            t = 0;
        if(t > 1)
            t = 1;
        // Optional acceleration curve (extrans accel)
        if(Accel != 0.0) {
            double a = Accel;
            if(a < -0.99)
                a = -0.99;
            if(a > 0.99)
                a = 0.99;
            // smoothstep-ish remap
            t = t + a * t * (1.0 - t) * (2.0 * t - 1.0);
            if(t < 0)
                t = 0;
            if(t > 1)
                t = 1;
        }
        Phase = (tjs_int)(t * PhaseMax + 0.5);
        if(Phase > PhaseMax)
            Phase = PhaseMax;
        return TJS_S_TRUE;
    }

    tjs_error EndProcess() override {
        if(Phase >= PhaseMax)
            return TJS_S_FALSE;
        return TJS_S_TRUE;
    }

    tjs_error MakeFinalImage(iTVPScanLineProvider **dest,
                             iTVPScanLineProvider *src1,
                             iTVPScanLineProvider *src2) override {
        *dest = Reverse ? src1 : src2;
        return TJS_S_OK;
    }

    tjs_error Process(tTVPDivisibleData *data) override {
        if(Phase <= 0) {
            data->Dest = const_cast<iTVPScanLineProvider *>(
                Reverse ? data->Src2 : data->Src1);
            data->DestLeft = Reverse ? data->Src2Left : data->Src1Left;
            data->DestTop = Reverse ? data->Src2Top : data->Src1Top;
            return TJS_S_OK;
        }
        if(Phase >= PhaseMax) {
            data->Dest = const_cast<iTVPScanLineProvider *>(
                Reverse ? data->Src1 : data->Src2);
            data->DestLeft = Reverse ? data->Src1Left : data->Src2Left;
            data->DestTop = Reverse ? data->Src1Top : data->Src2Top;
            return TJS_S_OK;
        }
        switch(Effect) {
            case ExEffect::Wave:
                BlendWave(data);
                break;
            case ExEffect::Ripple:
                BlendRipple(data);
                break;
            case ExEffect::Mosaic:
                BlendMosaic(data);
                break;
            case ExEffect::Twist:
            case ExEffect::TwistAccel:
                BlendTwist(data);
                break;
            case ExEffect::Turn:
                BlendTurn(data);
                break;
            case ExEffect::RotateZoom:
            case ExEffect::RotateSwap:
            case ExEffect::RotateVanish:
                BlendRotateZoom(data);
                break;
        }
        return TJS_S_OK;
    }

private:
    double Progress() const {
        const double p = (double)Phase / (double)PhaseMax;
        return Reverse ? 1.0 - p : p;
    }

    void BeginMask(const tTVPDivisibleData *data) {
        MaskPixels.resize((size_t)data->Width * (size_t)data->Height);
    }

    void SetMaskBlock(const tTVPDivisibleData *data, tjs_int x, tjs_int y,
                      tjs_int w, tjs_int h, tjs_int src2Opacity) {
        if(src2Opacity < 0)
            src2Opacity = 0;
        if(src2Opacity > 255)
            src2Opacity = 255;
        // UnivTransBlend mixes s2 -> s1 as the rule value grows, so the
        // procedural rule stores the inverse of the desired source-2 opacity.
        const tjs_uint32 v = (tjs_uint32)(255 - src2Opacity);
        const tjs_uint32 pixel = 0xff000000u | (v << 16) | (v << 8) | v;
        for(tjs_int yy = y; yy < y + h; ++yy) {
            auto *row = MaskPixels.data() + (size_t)yy * data->Width + x;
            std::fill(row, row + w, pixel);
        }
    }

    void ApplyMask(tTVPDivisibleData *data) {
        if(MaskPixels.empty())
            return;
        if(!Mask)
            Mask = new tTVPBaseTexture(Width, Height, 32);
        Mask->Update(MaskPixels.data(),
                     (unsigned int)(data->Width * sizeof(tjs_uint32)),
                     data->Left, data->Top, data->Width, data->Height);
        // With vague=255 and phase=255 the shader uses rule.r directly.
        MaskMethod->SetParameterInt(MaskVagueID, 255);
        MaskMethod->SetParameterInt(MaskPhaseID, 255);
        iTVPTexture2D *dest = data->Dest->GetTextureForRender();
        iTVPTexture2D *s1 = const_cast<iTVPScanLineProvider *>(data->Src1)
                                ->GetTexture();
        iTVPTexture2D *s2 = const_cast<iTVPScanLineProvider *>(data->Src2)
                                ->GetTexture();
        tRenderTexRectArray::Element src_tex[] = {
            tRenderTexRectArray::Element(
                s1, tTVPRect(data->Src1Left, data->Src1Top,
                             data->Src1Left + data->Width,
                             data->Src1Top + data->Height)),
            tRenderTexRectArray::Element(
                s2, tTVPRect(data->Src2Left, data->Src2Top,
                             data->Src2Left + data->Width,
                             data->Src2Top + data->Height)),
            tRenderTexRectArray::Element(
                Mask->GetTexture(),
                tTVPRect(data->Left, data->Top, data->Left + data->Width,
                         data->Top + data->Height))
        };
        TVPGetRenderManager()->OperateRect(
            MaskMethod, dest, nullptr,
            tTVPRect(data->DestLeft, data->DestTop,
                     data->DestLeft + data->Width,
                     data->DestTop + data->Height),
            tRenderTexRectArray(src_tex));
    }

    void EnsureMosaicTextures(tjs_int width, tjs_int height) {
        if(Mosaic1 && MosaicWidth == width && MosaicHeight == height)
            return;
        delete Mosaic2;
        delete Mosaic1;
        Mosaic1 = new tTVPBaseTexture(width, height, 32);
        Mosaic2 = new tTVPBaseTexture(width, height, 32);
        MosaicWidth = width;
        MosaicHeight = height;
    }

    void BlendWave(tTVPDivisibleData *data) {
        iTVPTexture2D *dest = data->Dest->GetTextureForRender();
        iTVPTexture2D *s1 = const_cast<iTVPScanLineProvider *>(data->Src1)
                                ->GetTexture();
        iTVPTexture2D *s2 = const_cast<iTVPScanLineProvider *>(data->Src2)
                                ->GetTexture();
        const double p = Progress();
        const double amp =
            (double)data->Height * 0.08 * Factor * std::sin(p * kPi);
        const double freq = 0.045 + 0.02 * (WaveType % 3);
        const tjs_int opa = (tjs_int)(p * 255.0 + 0.5);
        const tjs_int strip = 2;

        for(tjs_int y = 0; y < data->Height; y += strip) {
            tjs_int h = strip;
            if(y + h > data->Height)
                h = data->Height - y;
            double yy = (double)(data->Top + y);
            tjs_int ox =
                (tjs_int)(amp * std::sin(yy * freq + p * 12.0 * kPi) + 0.5);
            // Clamp source offsets into layer
            tjs_int s1l = data->Src1Left;
            tjs_int s2l = data->Src2Left + ox;
            tjs_int w = data->Width;
            tjs_int s1t = data->Src1Top + y;
            tjs_int s2t = data->Src2Top + y;
            if(s2l < 0) {
                tjs_int d = -s2l;
                s2l = 0;
                s1l += d;
                // dest also shifts
            }
            // Prefer full-width blend; if shift clips, fall back to unshifted
            // for remaining edge pixels.
            tjs_int maxW = Width - std::max(s1l, s2l);
            if(maxW < 1) {
                AlphaBlendRect(dest, data->DestLeft, data->DestTop + y, s1,
                               data->Src1Left, s1t, s2, data->Src2Left, s2t, w,
                               h, opa, DestLayerType);
                continue;
            }
            tjs_int ww = std::min(w, maxW);
            AlphaBlendRect(dest, data->DestLeft, data->DestTop + y, s1, s1l,
                           s1t, s2, s2l, s2t, ww, h, opa, DestLayerType);
            if(ww < w) {
                AlphaBlendRect(dest, data->DestLeft + ww, data->DestTop + y, s1,
                               data->Src1Left + ww, s1t, s2,
                               data->Src2Left + ww, s2t, w - ww, h, opa,
                               DestLayerType);
            }
        }
    }

    void BlendRipple(tTVPDivisibleData *data) {
        // Radial reveal with soft ring: inside radius → src2, outside → src1,
        // ring band crossfades. Build a CPU rule texture and blend it with one
        // GPU draw instead of issuing one draw per 4x4 block.
        const double p = Progress();
        const double maxR =
            std::sqrt((double)Width * Width + (double)Height * Height) * 0.5 *
            (0.85 + 0.3 * Factor);
        const double radius = p * maxR * 1.15;
        const double band = std::max(8.0, maxR * 0.08);
        const tjs_int block = 4;

        BeginMask(data);
        for(tjs_int y = 0; y < data->Height; y += block) {
            tjs_int h = std::min(block, data->Height - y);
            for(tjs_int x = 0; x < data->Width; x += block) {
                tjs_int w = std::min(block, data->Width - x);
                double cx = (double)(data->Left + x) + w * 0.5 - CenterX;
                double cy = (double)(data->Top + y) + h * 0.5 - CenterY;
                double r = std::sqrt(cx * cx + cy * cy);
                // ripple modulation
                r += 6.0 * Factor * std::sin(r * 0.08 - p * 10.0);
                tjs_int opa;
                if(r < radius - band)
                    opa = 255;
                else if(r > radius + band)
                    opa = 0;
                else
                    opa = (tjs_int)(((radius + band - r) / (2.0 * band)) *
                                        255.0 +
                                    0.5);
                SetMaskBlock(data, x, y, w, h, opa);
            }
        }
        ApplyMask(data);
    }

    void BlendMosaic(tTVPDivisibleData *data) {
        iTVPTexture2D *dest = data->Dest->GetTextureForRender();
        iTVPTexture2D *s1 = const_cast<iTVPScanLineProvider *>(data->Src1)
                                ->GetTexture();
        iTVPTexture2D *s2 = const_cast<iTVPScanLineProvider *>(data->Src2)
                                ->GetTexture();
        const double p = Progress();
        // Block size peaks mid-transition (classic mosaic dissolve).
        double mid = 1.0 - std::fabs(p * 2.0 - 1.0);
        tjs_int block = 2 + (tjs_int)(mid * 48.0 * Factor + 0.5);
        if(block < 2)
            block = 2;
        if(block > 64)
            block = 64;
        const tjs_int opa = (tjs_int)(p * 255.0 + 0.5);

        const tjs_int reducedWidth =
            std::max<tjs_int>(1, (data->Width + block - 1) / block);
        const tjs_int reducedHeight =
            std::max<tjs_int>(1, (data->Height + block - 1) / block);
        EnsureMosaicTextures(reducedWidth, reducedHeight);
        StretchCopyTex(Mosaic1->GetTextureForRender(false, nullptr), 0, 0,
                       reducedWidth, reducedHeight, s1, data->Src1Left,
                       data->Src1Top, data->Width, data->Height);
        StretchCopyTex(Mosaic2->GetTextureForRender(false, nullptr), 0, 0,
                       reducedWidth, reducedHeight, s2, data->Src2Left,
                       data->Src2Top, data->Width, data->Height);
        AlphaBlendScaled(
            dest,
            tTVPRect(data->DestLeft, data->DestTop,
                     data->DestLeft + data->Width,
                     data->DestTop + data->Height),
            Mosaic1->GetTexture(), tTVPRect(0, 0, reducedWidth, reducedHeight),
            Mosaic2->GetTexture(), tTVPRect(0, 0, reducedWidth, reducedHeight),
            opa, DestLayerType);
    }

    void BlendTwist(tTVPDivisibleData *data) {
        // Angular wipe with spiral bias (twist)
        double p = Progress();
        if(Effect == ExEffect::TwistAccel) {
            double ta = TwistAccel;
            p = p + ta * p * p * (1.0 - p);
            if(p < 0)
                p = 0;
            if(p > 1)
                p = 1;
        }
        const double twistAmt = Twist * 2.5;
        const double open = p * (2.0 * kPi + std::fabs(twistAmt));
        const tjs_int block = 3;

        BeginMask(data);
        for(tjs_int y = 0; y < data->Height; y += block) {
            tjs_int h = std::min(block, data->Height - y);
            for(tjs_int x = 0; x < data->Width; x += block) {
                tjs_int w = std::min(block, data->Width - x);
                double cx = (double)(data->Left + x) + w * 0.5 - CenterX;
                double cy = (double)(data->Top + y) + h * 0.5 - CenterY;
                double ang = std::atan2(cy, cx); // -pi..pi
                if(ang < 0)
                    ang += 2.0 * kPi;
                double r = std::sqrt(cx * cx + cy * cy);
                double maxR =
                    std::sqrt((double)Width * Width + (double)Height * Height);
                double spiral = ang + twistAmt * (r / (maxR + 1.0));
                while(spiral < 0)
                    spiral += 2.0 * kPi;
                while(spiral >= 2.0 * kPi)
                    spiral -= 2.0 * kPi;
                tjs_int opa = (spiral < open) ? 255 : 0;
                // soft edge
                double edge = open - spiral;
                if(edge > 0 && edge < 0.15)
                    opa = (tjs_int)((edge / 0.15) * 255.0);
                if(spiral >= open && spiral - open < 0.15)
                    opa = 255 - (tjs_int)(((spiral - open) / 0.15) * 255.0);
                SetMaskBlock(data, x, y, w, h, opa);
            }
        }
        ApplyMask(data);
    }

    void BlendTurn(tTVPDivisibleData *data) {
        // Vertical door / page-turn style: reveal src2 from left with curved
        // edge.
        const double p = Progress();

        BeginMask(data);
        for(tjs_int y = 0; y < data->Height; ++y) {
            double wave =
                18.0 * Factor * std::sin((double)y * 0.07 + p * 6.0);
            tjs_int edge =
                (tjs_int)(p * (double)data->Width + wave + 0.5);
            if(edge < 0)
                edge = 0;
            if(edge > data->Width)
                edge = data->Width;
            if(edge > 0)
                SetMaskBlock(data, 0, y, edge, 1, 255);
            if(edge < data->Width)
                SetMaskBlock(data, edge, y, data->Width - edge, 1, 0);
        }
        ApplyMask(data);
    }

    void BlendRotateZoom(tTVPDivisibleData *data) {
        // Approximate rotate-zoom: circular iris + overall crossfade.
        // Full perspective rotate would need a custom shader; this gives a
        // clear zoom-ish reveal that is more than a flat fade.
        const double p = Progress();
        const tjs_int opa = (tjs_int)(p * 255.0 + 0.5);
        double maxR =
            std::sqrt((double)Width * Width + (double)Height * Height) * 0.55;
        double radius = (Effect == ExEffect::RotateVanish)
            ? (1.0 - p) * maxR
            : p * maxR * (0.7 + 0.5 * Factor);
        const tjs_int block = 4;

        BeginMask(data);
        for(tjs_int y = 0; y < data->Height; y += block) {
            tjs_int h = std::min(block, data->Height - y);
            for(tjs_int x = 0; x < data->Width; x += block) {
                tjs_int w = std::min(block, data->Width - x);
                double cx = (double)(data->Left + x) + w * 0.5 - CenterX;
                double cy = (double)(data->Top + y) + h * 0.5 - CenterY;
                double r = std::sqrt(cx * cx + cy * cy);
                // cheap angular swirl for "rotate" feel
                double ang = std::atan2(cy, cx) + p * Factor * 2.0;
                r += 4.0 * std::sin(ang * 3.0);
                tjs_int blockOpacity = opa;
                if(Effect == ExEffect::RotateVanish)
                    blockOpacity = r > radius ? 255 : opa;
                else
                    blockOpacity = r < radius ? 255 : opa;
                SetMaskBlock(data, x, y, w, h, blockOpacity);
            }
        }
        ApplyMask(data);
    }
};

class tExtransProvider : public iTVPTransHandlerProvider {
    tjs_int RefCount = 1;
    const tjs_char *Name;
    ExEffect Effect;

public:
    tExtransProvider(const tjs_char *name, ExEffect effect) :
        Name(name), Effect(effect) {}

    tjs_error AddRef() override {
        RefCount++;
        return TJS_S_OK;
    }
    tjs_error Release() override {
        if(RefCount == 1)
            delete this;
        else
            RefCount--;
        return TJS_S_OK;
    }
    tjs_error GetName(const tjs_char **name) override {
        if(!name)
            return TJS_E_FAIL;
        *name = Name;
        return TJS_S_OK;
    }

    tjs_error StartTransition(
        iTVPSimpleOptionProvider *options, iTVPSimpleImageProvider *imagepro,
        tTVPLayerType layertype, tjs_uint src1w, tjs_uint src1h, tjs_uint src2w,
        tjs_uint src2h, tTVPTransType *type, tTVPTransUpdateType *updatetype,
        iTVPBaseTransHandler **handler) override {
        (void)imagepro;
        if(type)
            *type = ttExchange;
        if(updatetype)
            *updatetype = tutDivisible; // free source sampling
        if(!handler || !options)
            return TJS_E_FAIL;
        if(src1w != src2w || src1h != src2h)
            TVPThrowExceptionMessage(
                TVPTransitionLayerSizeMismatch,
                ttstr((tjs_int)src2w) + TJS_W("x") + ttstr((tjs_int)src2h),
                ttstr((tjs_int)src1w) + TJS_W("x") + ttstr((tjs_int)src1h));

        tjs_int64 time = 0;
        if(TJS_FAILED(options->GetAsNumber(TJS_W("time"), &time)))
            TVPThrowExceptionMessage(TVPSpecifyOption, TJS_W("time"));
        if(time < 2)
            time = 2;

        *handler = new tExtransHandler(options, layertype, (tjs_uint64)time,
                                       Effect, (tjs_int)src1w, (tjs_int)src1h);
        return TJS_S_OK;
    }
};

static void RegisterOne(const tjs_char *name, ExEffect effect) {
    // Avoid double-register if plugin reloaded
    // TVPAddTransHandlerProvider throws if duplicate — swallow by checking
    // find first is not available for "exists", so try/catch via not adding
    // twice using static flag set.
    iTVPTransHandlerProvider *pro = new tExtransProvider(name, effect);
    try {
        TVPAddTransHandlerProvider(pro);
    } catch(...) {
        pro->Release();
        throw;
    }
    pro->Release(); // table holds its own ref
}

static bool s_registered = false;

static void InitPlugin_Extrans() {
    if(s_registered)
        return;
    s_registered = true;

    RegisterOne(TJS_W("wave"), ExEffect::Wave);
    RegisterOne(TJS_W("wavetype"), ExEffect::Wave);
    RegisterOne(TJS_W("ripple"), ExEffect::Ripple);
    RegisterOne(TJS_W("mosaic"), ExEffect::Mosaic);
    RegisterOne(TJS_W("twist"), ExEffect::Twist);
    RegisterOne(TJS_W("twistaccel"), ExEffect::TwistAccel);
    RegisterOne(TJS_W("turn"), ExEffect::Turn);
    RegisterOne(TJS_W("rotatezoom"), ExEffect::RotateZoom);
    RegisterOne(TJS_W("rotateswap"), ExEffect::RotateSwap);
    RegisterOne(TJS_W("rotatevanish"), ExEffect::RotateVanish);

    TVPAddImportantLog(
        TJS_W("extrans: native transition providers registered "
              "(wave/ripple/mosaic/twist/turn/rotate*)"));
}

} // namespace

NCB_PRE_REGIST_CALLBACK(InitPlugin_Extrans);
