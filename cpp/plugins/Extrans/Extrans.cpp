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
#include "ExtransMath.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#define NCB_MODULE_NAME TJS_W("extrans.dll")

namespace {

constexpr double kPi = 3.14159265358979323846;

static const char *kRippleShader = R"GLSL(
precision highp float;
uniform float u_blendRatio;
uniform float u_phase;
uniform float u_drift;
uniform float u_rippleWidth;
uniform float u_driftRows;
uniform float u_srcWidth;
uniform float u_srcHeight;
uniform float u_tex1Width;
uniform float u_tex1Height;
uniform float u_tex2Width;
uniform float u_tex2Height;
uniform float u_src1OffsetX;
uniform float u_src1OffsetY;
uniform float u_src2OffsetX;
uniform float u_src2OffsetY;
uniform float u_centerX;
uniform float u_centerY;
void main() {
    vec2 logical = floor(v_texCoord2 * vec2(u_srcWidth, u_srcHeight));
    vec4 keyBytes = floor(texture2D(tex2, v_texCoord2) * 255.0 + 0.5);
    float key = keyBytes.r + keyBytes.g * 256.0;
    vec2 offset = vec2(0.0);
    if(u_driftRows > 0.5) {
        float distance = floor(key / 32.0);
        float direction = key - distance * 32.0;
        float driftKey = mod(distance + u_phase, u_rippleWidth) * 32.0 + direction;
        vec2 lookup = vec2((driftKey + 0.5) / (u_rippleWidth * 32.0),
                           (u_drift + 0.5) / u_driftRows);
        vec2 encoded = floor(texture2D(tex3, lookup).rg * 255.0 + 0.5);
        offset = mix(encoded, encoded - 256.0, step(127.5, encoded));
    }
    vec2 signs = vec2(logical.x < u_centerX ? -1.0 : 1.0,
                      logical.y < u_centerY ? -1.0 : 1.0);
    vec2 samplePixel = logical - signs * offset;
    if(samplePixel.x < 0.0) samplePixel.x = -samplePixel.x;
    if(samplePixel.y < 0.0) samplePixel.y = -samplePixel.y;
    if(samplePixel.x >= u_srcWidth)
        samplePixel.x = u_srcWidth - 1.0 - (samplePixel.x - u_srcWidth);
    if(samplePixel.y >= u_srcHeight)
        samplePixel.y = u_srcHeight - 1.0 - (samplePixel.y - u_srcHeight);
    samplePixel = clamp(samplePixel, vec2(0.0),
                        vec2(u_srcWidth - 1.0, u_srcHeight - 1.0));
    vec2 uv1 = (samplePixel + vec2(u_src1OffsetX, u_src1OffsetY) + vec2(0.5)) /
               vec2(u_tex1Width, u_tex1Height);
    vec2 uv2 = (samplePixel + vec2(u_src2OffsetX, u_src2OffsetY) + vec2(0.5)) /
               vec2(u_tex2Width, u_tex2Height);
    vec4 source1 = floor(texture2D(tex0, uv1) * 255.0 + 0.5);
    vec4 source2 = floor(texture2D(tex1, uv2) * 255.0 + 0.5);
    // Official Blend() performs a signed arithmetic >> 8. floor reproduces
    // that behavior for both positive and negative channel deltas.
    vec4 blended = source1 +
                   floor((source2 - source1) * u_blendRatio / 256.0);
    gl_FragColor = blended / 255.0;
}
)GLSL";

static const char *kMosaicShader = R"GLSL(
precision highp float;
uniform float u_blendRatio;
uniform float u_blockSize;
uniform float u_offsetX;
uniform float u_offsetY;
uniform float u_width;
uniform float u_height;
uniform float u_tex1Width;
uniform float u_tex1Height;
uniform float u_tex2Width;
uniform float u_tex2Height;
uniform float u_src1OffsetX;
uniform float u_src1OffsetY;
uniform float u_src2OffsetX;
uniform float u_src2OffsetY;
uniform float u_logicalOffsetX;
uniform float u_logicalOffsetY;
void main() {
    vec2 logical = floor(v_texCoord0 * vec2(u_tex1Width, u_tex1Height)) +
                   vec2(u_logicalOffsetX, u_logicalOffsetY);
    vec2 block = floor((logical - vec2(u_offsetX, u_offsetY)) / u_blockSize);
    vec2 samplePixel = block * u_blockSize +
                       vec2(u_offsetX, u_offsetY) + floor(u_blockSize / 2.0);
    samplePixel = clamp(samplePixel, vec2(0.0), vec2(u_width - 1.0, u_height - 1.0));
    vec2 uv1 = (samplePixel + vec2(u_src1OffsetX, u_src1OffsetY) + vec2(0.5)) /
               vec2(u_tex1Width, u_tex1Height);
    vec2 uv2 = (samplePixel + vec2(u_src2OffsetX, u_src2OffsetY) + vec2(0.5)) /
               vec2(u_tex2Width, u_tex2Height);
    vec4 source1 = floor(texture2D(tex0, uv1) * 255.0 + 0.5);
    vec4 source2 = floor(texture2D(tex1, uv2) * 255.0 + 0.5);
    vec4 blended = source1 +
                   floor((source2 - source1) * u_blendRatio / 256.0);
    gl_FragColor = blended / 255.0;
}
)GLSL";

static const char *kTurnShader = R"GLSL(
precision highp float;
uniform float u_globalPhase;
uniform float u_srcWidth;
uniform float u_srcHeight;
uniform float u_tex1Width;
uniform float u_tex1Height;
uniform float u_tex2Width;
uniform float u_tex2Height;
uniform float u_src1OffsetX;
uniform float u_src1OffsetY;
uniform float u_src2OffsetX;
uniform float u_src2OffsetY;
uniform float u_logicalOffsetX;
uniform float u_logicalOffsetY;
uniform vec4 u_background;
float decodeSigned24(vec4 bytes) {
    bytes = floor(bytes * 255.0 + 0.5);
    float magnitude = bytes.r + bytes.g * 256.0 + bytes.b * 65536.0;
    return bytes.a > 127.5 ? -magnitude : magnitude;
}
void main() {
    vec2 logical = floor(v_texCoord0 * vec2(u_tex1Width, u_tex1Height)) +
                   vec2(u_logicalOffsetX, u_logicalOffsetY);
    vec2 tile = floor(logical / 64.0);
    vec2 within = logical - tile * 64.0;
    float phase = clamp(u_globalPhase - (tile.x - tile.y) * 2.0, 0.0, 63.0);
    phase = floor(phase + 0.5);
    if(phase < 0.5) {
        gl_FragColor = texture2D(tex0, v_texCoord0);
        return;
    }
    if(phase > 62.5) {
        gl_FragColor = texture2D(tex1, v_texCoord1);
        return;
    }
    vec2 lutUV = vec2((phase + 0.5) / 64.0, (within.y + 0.5) / 64.0);
    vec4 range = floor(texture2D(tex2, lutUV) * 255.0 + 0.5);
    float start = range.r;
    float length = range.g;
    if(within.x < start || within.x >= start + length) {
        gl_FragColor = u_background;
        return;
    }
    float sourceX = decodeSigned24(texture2D(tex3, lutUV));
    float sourceY = decodeSigned24(texture2D(tex4, lutUV));
    float stepX = decodeSigned24(texture2D(tex5, lutUV));
    float stepY = decodeSigned24(texture2D(tex6, lutUV));
    float advance = within.x - start;
    // Divide the fixed-point terms before multiplying. RGBA8 storage is
    // lossless, and keeping shader arithmetic near tile scale avoids losing
    // low bits when a signed 24-bit step is multiplied by scanline advance.
    vec2 sourceWithin = floor(vec2(sourceX, sourceY) / 65536.0 +
                              vec2(stepX, stepY) / 65536.0 * advance);
    vec2 sourcePixel = tile * 64.0 + sourceWithin;
    if(sourcePixel.x < 0.0 || sourcePixel.y < 0.0 ||
       sourcePixel.x >= u_srcWidth || sourcePixel.y >= u_srcHeight) {
        gl_FragColor = u_background;
        return;
    }
    vec2 sourceUV1 =
        (sourcePixel + vec2(u_src1OffsetX, u_src1OffsetY) + vec2(0.5)) /
        vec2(u_tex1Width, u_tex1Height);
    vec2 sourceUV2 =
        (sourcePixel + vec2(u_src2OffsetX, u_src2OffsetY) + vec2(0.5)) /
        vec2(u_tex2Width, u_tex2Height);
    vec4 color = phase < 32.0 ? texture2D(tex0, sourceUV1)
                              : texture2D(tex1, sourceUV2);
    float gloss = range.b / 256.0;
    color.rgb = color.rgb + (vec3(1.0) - color.rgb) * gloss;
    gl_FragColor = color;
}
)GLSL";

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

static tjs_int ReadInt32(iTVPSimpleOptionProvider *options,
                         const tjs_char *name, tjs_int64 def) {
    const tjs_int64 value = ReadNumber(options, name, def);
    if(value < std::numeric_limits<tjs_int>::min() ||
       value > std::numeric_limits<tjs_int>::max())
        TVPThrowExceptionMessage(TJS_W("extrans integer option is out of range"));
    return static_cast<tjs_int>(value);
}

// Official extrans common.h Clip(): clip [l, r) into [cl, cr), false if empty
static bool ClipRange(tjs_int &l, tjs_int &r, tjs_int cl, tjs_int cr) {
    if(l < cl)
        l = cl;
    if(r > cr)
        r = cr;
    return l < r;
}

// Official extrans common.h Blend(): mix a -> b by opa (0..255), 0xAARRGGBB
static tjs_uint32 BlendColor(tjs_uint32 a, tjs_uint32 b, tjs_int opa) {
    if(opa <= 0)
        return a;
    if(opa >= 255)
        return b;
    tjs_uint32 ret;
    tjs_uint32 tmp;
    tmp = a & 0x000000ff;
    ret = 0x000000ff &
          (tmp + (((b & 0x000000ff) - tmp) * opa >> 8));
    tmp = a & 0x0000ff00;
    ret |= 0x0000ff00 &
           (tmp + (((b & 0x0000ff00) - tmp) * opa >> 8));
    tmp = a & 0x00ff0000;
    ret |= 0x00ff0000 &
           (tmp + (((b & 0x00ff0000) - tmp) * opa >> 8));
    tmp = a >> 24;
    ret |= (0x000000ff & (tmp + (((b >> 24) - tmp) * opa >> 8))) << 24;
    return ret;
}

// Fill a destination rect with a flat 0xAARRGGBB color (FillARGB).
static void FillRectColor(iTVPTexture2D *dest, tjs_int dl, tjs_int dt,
                          tjs_int w, tjs_int h, tjs_uint32 color) {
    if(w <= 0 || h <= 0 || !dest)
        return;
    static iTVPRenderMethod *method =
        TVPGetRenderManager()->GetRenderMethod("FillARGB");
    static int colorID = method->EnumParameterID("color");
    method->SetParameterColor4B(colorID, color);
    TVPGetRenderManager()->OperateRect(
        method, dest, nullptr, tTVPRect(dl, dt, dl + w, dt + h),
        tRenderTexRectArray());
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

// Draw a textured quad with the official rotate* source mapping: the source
// transition rectangle -> destination parallelogram.
static void PerspectiveQuad(iTVPTexture2D *dest, iTVPTexture2D *src,
                            const tTVPPointD logicalDst[4],
                            tjs_int sourceOffsetX, tjs_int sourceOffsetY,
                            tjs_int logicalWidth, tjs_int logicalHeight,
                            tjs_int destinationOffsetX,
                            tjs_int destinationOffsetY,
                            const tTVPRect &clip) {
    if(!dest || !src)
        return;
    static iTVPRenderMethod *method =
        TVPGetRenderManager()->GetRenderMethod("PerspectiveCopy");
    static int textureSizeID = method->EnumParameterID("sourceTextureSize");
    float textureSize[] = {static_cast<float>(src->GetInternalWidth()),
                           static_cast<float>(src->GetInternalHeight())};
    method->SetParameterFloatArray(textureSizeID, textureSize, 2);
    tTVPPointD srcpt[4] = {
        {(double)sourceOffsetX, (double)sourceOffsetY},
        {(double)(sourceOffsetX + logicalWidth - 1), (double)sourceOffsetY},
        {(double)sourceOffsetX, (double)(sourceOffsetY + logicalHeight - 1)},
        {(double)(sourceOffsetX + logicalWidth - 1),
         (double)(sourceOffsetY + logicalHeight - 1)}
    };
    tTVPPointD dstpt[4];
    for(std::size_t index = 0; index < 4; ++index) {
        dstpt[index].x = logicalDst[index].x + destinationOffsetX;
        dstpt[index].y = logicalDst[index].y + destinationOffsetY;
    }
    tRenderTexQuadArray::Element src_tex[] = {
        tRenderTexQuadArray::Element(src, srcpt) };
    TVPGetRenderManager()->OperatePerspective(
        method, 1, dest, nullptr, clip, dstpt,
        tRenderTexQuadArray(src_tex));
}

enum class ExEffect {
    Wave,
    Ripple,
    Mosaic,
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
    // wave (official extrans semantics)
    tjs_int MaxH = 50;
    double MaxOmega = 0.2;
    tjs_uint32 BGColor1 = 0;
    tjs_uint32 BGColor2 = 0;
    tjs_int CurH = 0;
    double CurOmega = 0;
    double CurRadStart = 0;
    tjs_int BlendRatio = 0;
    tjs_uint32 CurBGColor = 0;
    // mosaic (official extrans semantics)
    tjs_int MaxBlockSize = 30;
    // rotate (official extrans semantics)
    double TargetFactor = 1.0;
    tjs_uint32 RotateBGColor = 0;
    bool FixSrc1 = false;
    tTVPPointD Quad1[4];
    tTVPPointD Quad2[4];
    bool DrawQuad1 = false;
    bool DrawQuad2 = false;
    tTVPBaseTexture *Mask = nullptr;
    extrans::MosaicFrame MosaicFrame;
    extrans::RippleParameters RippleParameters;
    extrans::RippleTables RippleTables;
    extrans::RippleFrame RippleFrame;
    tjs_int TurnGlobalPhase = 0;
    tjs_uint32 TurnBGColor = 0;
    tTVPBaseTexture *RippleDisplacement = nullptr;
    tTVPBaseTexture *RippleDrift = nullptr;
    std::array<tTVPBaseTexture *, 5> TurnLUT{};
    iTVPRenderMethod *RippleMethod = nullptr;
    iTVPRenderMethod *TurnMethod = nullptr;
    iTVPRenderMethod *MosaicMethod = nullptr;
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
        // Official parameter defaults per effect (SamplePlugin/extrans).
        switch(Effect) {
            case ExEffect::RotateZoom:
                Factor = ReadReal(Options, TJS_W("factor"), 1.0);
                Accel = ReadReal(Options, TJS_W("accel"), 0.0);
                Twist = ReadReal(Options, TJS_W("twist"), 2.0);
                TwistAccel = ReadReal(Options, TJS_W("twistaccel"), -2.0);
                TargetFactor = 1.0;
                FixSrc1 = true;
                break;
            case ExEffect::RotateVanish:
                Factor = 1.0;
                TargetFactor = 0.0;
                Accel = ReadReal(Options, TJS_W("accel"), 2.0);
                Twist = ReadReal(Options, TJS_W("twist"), 2.0);
                TwistAccel = ReadReal(Options, TJS_W("twistaccel"), 2.0);
                FixSrc1 = false;
                break;
            case ExEffect::RotateSwap:
                RotateBGColor =
                    (tjs_uint32)ReadNumber(Options, TJS_W("bgcolor"), 0);
                Twist = ReadReal(Options, TJS_W("twist"), 1.0);
                break;
            case ExEffect::Ripple:
                RippleParameters.width = w;
                RippleParameters.height = h;
                RippleParameters.centerX =
                    (tjs_int)ReadNumber(Options, TJS_W("centerx"), w / 2);
                RippleParameters.centerY =
                    (tjs_int)ReadNumber(Options, TJS_W("centery"), h / 2);
                RippleParameters.rippleWidth =
                    (tjs_int)ReadNumber(Options, TJS_W("rwidth"), 128);
                RippleParameters.roundness =
                    (float)ReadReal(Options, TJS_W("roundness"), 1.0);
                RippleParameters.speed =
                    (float)ReadReal(Options, TJS_W("speed"), 6.0);
                RippleParameters.maxDrift =
                    (tjs_int)ReadNumber(Options, TJS_W("maxdrift"), 24);
                if(!extrans::ValidateRippleParameters(RippleParameters))
                    TVPThrowExceptionMessage(
                        TJS_W("invalid ripple options: center/rwidth/roundness/"
                              "speed/maxdrift"));
                try {
                    RippleTables =
                        extrans::GenerateRippleTables(RippleParameters);
                } catch(const std::exception &error) {
                    TVPAddImportantLog(ttstr(TJS_W("extrans ripple table: ")) +
                                       ttstr(error.what()));
                    TVPThrowExceptionMessage(
                        TJS_W("failed to prepare ripple lookup tables"));
                }
                break;
            case ExEffect::Turn:
                TurnBGColor =
                    (tjs_uint32)ReadNumber(Options, TJS_W("bgcolor"), 0);
                break;
            default:
                Factor = ReadReal(Options, TJS_W("factor"), 1.0);
                Accel = ReadReal(Options, TJS_W("accel"), 0.0);
                Twist = ReadReal(Options, TJS_W("twist"), 1.0);
                TwistAccel = ReadReal(Options, TJS_W("twistaccel"), 0.0);
                break;
        }
        CenterX = ReadInt32(Options, TJS_W("centerx"), w / 2);
        CenterY = ReadInt32(Options, TJS_W("centery"), h / 2);
        WaveType = ReadInt32(Options, TJS_W("wavetype"), 0);
        Reverse = ReadInt32(Options, TJS_W("reverse"), 0);
        MaxH = ReadInt32(Options, TJS_W("maxh"), 50);
        MaxOmega = ReadReal(Options, TJS_W("maxomega"), 0.2);
        BGColor1 = (tjs_uint32)ReadNumber(Options, TJS_W("bgcolor1"), 0);
        BGColor2 = (tjs_uint32)ReadNumber(Options, TJS_W("bgcolor2"), 0);
        const tjs_int64 maxBlockSize =
            ReadNumber(Options, TJS_W("maxsize"), 30);
        if(Effect == ExEffect::Wave &&
           (!extrans::ValidateWaveParameters(w, MaxH, MaxOmega) ||
            std::fabs(MaxOmega) >
                std::numeric_limits<double>::max() / std::max(1, h) ||
            WaveType < 0 || WaveType > 2))
            TVPThrowExceptionMessage(
                TJS_W("invalid wave options: maxh/maxomega/wavetype"));
        if(Effect == ExEffect::Mosaic &&
           !extrans::ValidateMosaicParameters(w, h, maxBlockSize))
            TVPThrowExceptionMessage(TJS_W("invalid mosaic option: maxsize"));
        if(Effect == ExEffect::Mosaic)
            MaxBlockSize = static_cast<tjs_int>(maxBlockSize);
        if((Effect == ExEffect::RotateZoom ||
            Effect == ExEffect::RotateVanish ||
            Effect == ExEffect::RotateSwap) &&
           (!extrans::IsFiniteRotateParameter(Factor) ||
            !extrans::IsFiniteRotateParameter(Accel) ||
            !extrans::IsFiniteRotateParameter(Twist) ||
            !extrans::IsFiniteRotateParameter(TwistAccel)))
            TVPThrowExceptionMessage(
                TJS_W("invalid rotate options: non-finite value"));
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
        for(auto *texture : TurnLUT)
            delete texture;
        delete RippleDrift;
        delete RippleDisplacement;
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
		tjs_uint64 elapsed = tick >= StartTick ? tick - StartTick : 0;
		tjs_int64 raw = elapsed > static_cast<tjs_uint64>(
			std::numeric_limits<tjs_int64>::max()) ?
			std::numeric_limits<tjs_int64>::max() : static_cast<tjs_int64>(elapsed);
        tjs_int64 curtime = raw;
        if(curtime > (tjs_int64)Time)
            curtime = Time;
        if(Reverse)
            curtime = (tjs_int64)Time - curtime;
        double t = (double)raw / (double)Time;
        if(t < 0)
            t = 0;
        if(t > 1)
            t = 1;
        // Optional acceleration curve (extrans accel). rotate* effects use
        // their own official power curves in BlendRotate, so skip the global
        // remap for them.
        bool isRotate = Effect == ExEffect::RotateZoom ||
                        Effect == ExEffect::RotateSwap ||
                        Effect == ExEffect::RotateVanish;
        if(!isRotate && Accel != 0.0) {
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
        // rotate: official per-frame quad positions (tTVPBaseRotateTransHandler)
        if(isRotate) {
            const float p = static_cast<float>(curtime) /
                            static_cast<float>(Time);
            try {
                switch(Effect) {
                    case ExEffect::RotateZoom:
                    case ExEffect::RotateVanish:
                        CalcRotateZoomPos(p);
                        break;
                    case ExEffect::RotateSwap:
                        CalcRotateSwapPos(p);
                        break;
                    default:
                        break;
                }
            } catch(const std::exception &error) {
                TVPAddImportantLog(ttstr(TJS_W("extrans rotate geometry: ")) +
                                   ttstr(error.what()));
                TVPThrowExceptionMessage(
                    TJS_W("invalid rotate geometry result"));
            }
        }
        // wave: official extrans per-frame parameters
        if(Effect == ExEffect::Wave) {
            const auto frame = extrans::ComputeWaveFrame(
                static_cast<tjs_uint64>(curtime), Time, Height, MaxH,
                MaxOmega, WaveType);
            CurH = frame.height;
            CurOmega = frame.omega;
            CurRadStart = frame.radianStart;
            BlendRatio = frame.blendRatio;
            CurBGColor = BlendColor(BGColor1, BGColor2, BlendRatio);
        }
        if(Effect == ExEffect::Mosaic)
            MosaicFrame = extrans::ComputeMosaicFrame(
                static_cast<tjs_uint64>(curtime), Time, Width, Height,
                MaxBlockSize);
        if(Effect == ExEffect::Ripple)
            RippleFrame = extrans::ComputeRippleFrame(
                (tjs_uint64)curtime, Time, RippleParameters);
        if(Effect == ExEffect::Turn)
            TurnGlobalPhase = extrans::ComputeTurnGlobalPhase(
                (tjs_uint64)curtime, Time, Width, Height);
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
            case ExEffect::Turn:
                BlendTurn(data);
                break;
            case ExEffect::RotateZoom:
            case ExEffect::RotateSwap:
            case ExEffect::RotateVanish:
                BlendRotate(data);
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

    void EnsureMosaicResources() {
        if(MosaicMethod)
            return;
        static uint32_t hint = 0x2bdf13a9u;
        MosaicMethod = TVPGetRenderManager()->GetOrCompileRenderMethod(
            "ExtransMosaicOfficial", &hint, kMosaicShader, 2);
    }

    static void SetFloat(iTVPRenderMethod *method, const char *name,
                         float value) {
        method->SetParameterFloat(method->EnumParameterID(name), value);
    }

    void EnsureRippleResources() {
        if(RippleDisplacement)
            return;
        std::vector<tjs_uint32> displacement(
            static_cast<std::size_t>(Width) * Height);
        for(tjs_int y = 0; y < Height; ++y) {
            const tjs_int mapY =
                y < RippleParameters.centerY
                    ? RippleParameters.centerY - y - 1
                    : y - RippleParameters.centerY;
            for(tjs_int x = 0; x < Width; ++x) {
                const tjs_int mapX =
                    x < RippleParameters.centerX
                        ? RippleParameters.centerX - x - 1
                        : x - RippleParameters.centerX;
                const std::uint16_t key =
                    RippleTables.displacement[static_cast<std::size_t>(mapY) *
                                                  RippleTables.mapWidth +
                                              mapX];
                displacement[static_cast<std::size_t>(y) * Width + x] =
                    0xff000000u | (key & 0xffu) | ((key >> 8) << 8);
            }
        }
        RippleDisplacement = new tTVPBaseTexture(Width, Height, 32);
        RippleDisplacement->Update(displacement.data(), Width * 4, 0, 0, Width,
                                   Height);

        const tjs_int driftRows =
            RippleParameters.maxDrift * extrans::kRippleDriftPrecision;
        const tjs_int driftWidth =
            RippleParameters.rippleWidth * extrans::kRippleDirectionPrecision;
        RippleDrift = new tTVPBaseTexture(driftWidth, std::max(1, driftRows), 32);
        std::vector<tjs_uint32> driftPixels(
            static_cast<std::size_t>(driftWidth) * std::max(1, driftRows),
            0xff000000u);
        for(std::size_t index = 0; index < RippleTables.drift.size(); ++index) {
            const std::uint16_t value = RippleTables.drift[index];
            driftPixels[index] = 0xff000000u | ((value >> 8) & 0xffu) |
                                 ((value & 0xffu) << 8);
        }
        RippleDrift->Update(driftPixels.data(), driftWidth * 4, 0, 0, driftWidth,
                            std::max(1, driftRows));

        static uint32_t hint = 0xc4129373u;
        RippleMethod = TVPGetRenderManager()->GetOrCompileRenderMethod(
            "ExtransRippleOfficial", &hint, kRippleShader, 4);
    }

    void EnsureTurnResources() {
        if(TurnLUT[0])
            return;
        std::array<std::vector<tjs_uint32>, 5> pixels;
        for(auto &plane : pixels)
            plane.resize(64 * 64, 0);
        const auto &table = extrans::GetTurnTable();
        const auto &gloss = extrans::GetTurnGloss();
        for(tjs_int phase = 0; phase < 64; ++phase) {
            for(tjs_int line = 0; line < 64; ++line) {
                const auto &entry = table[phase][line];
                const std::size_t index =
                    static_cast<std::size_t>(line) * 64 + phase;
                pixels[0][index] =
                    0xff000000u | (entry.start & 0xffu) |
                    ((entry.length & 0xffu) << 8) |
                    ((gloss[phase] & 0xffu) << 16);
                pixels[1][index] = extrans::PackSigned24(entry.sourceX);
                pixels[2][index] = extrans::PackSigned24(entry.sourceY);
                pixels[3][index] = extrans::PackSigned24(entry.stepX);
                pixels[4][index] = extrans::PackSigned24(entry.stepY);
            }
        }
        for(std::size_t index = 0; index < TurnLUT.size(); ++index) {
            TurnLUT[index] = new tTVPBaseTexture(64, 64, 32);
            TurnLUT[index]->Update(pixels[index].data(), 64 * 4, 0, 0, 64, 64);
        }
        static uint32_t hint = 0xa45ee741u;
        TurnMethod = TVPGetRenderManager()->GetOrCompileRenderMethod(
            "ExtransTurnOfficial", &hint, kTurnShader, 7);
    }

    void BlendWave(tTVPDivisibleData *data) {
        // Official extrans wave: each line is shifted by d = sin(rad)*CurH,
        // src1/src2 blended at BlendRatio, exposed edges filled with
        // CurBGColor. Runs of equal d are merged into strips to limit the
        // number of GPU rects.
        iTVPTexture2D *dest = data->Dest->GetTextureForRender();
        iTVPTexture2D *s1 = const_cast<iTVPScanLineProvider *>(data->Src1)
                                ->GetTexture();
        iTVPTexture2D *s2 = const_cast<iTVPScanLineProvider *>(data->Src2)
                                ->GetTexture();

        tjs_int y = 0;
        double rad = static_cast<double>(data->Top) * CurOmega + CurRadStart;
        while(y < data->Height) {
            tjs_int d = (tjs_int)(sin(rad) * CurH);
            tjs_int h = 1;
            double nextRad = rad + CurOmega;
            while(y + h < data->Height) {
                if((tjs_int)(sin(nextRad) * CurH) != d)
                    break;
                h++;
                nextRad += CurOmega;
            }

            // left/right exposed edge -> background color
            if(d > 0) {
                tjs_int l = 0, r = d;
                if(ClipRange(l, r, data->Left, data->Left + data->Width))
                    FillRectColor(dest, data->DestLeft + l - data->Left,
                                  data->DestTop + y, r - l, h, CurBGColor);
            } else if(d < 0) {
                tjs_int l = d + Width, r = Width;
                if(ClipRange(l, r, data->Left, data->Left + data->Width))
                    FillRectColor(dest, data->DestLeft + l - data->Left,
                                  data->DestTop + y, r - l, h, CurBGColor);
            }

            // blended body shifted by d
            tjs_int l = d, r = Width + d;
            if(ClipRange(l, r, data->Left, data->Left + data->Width)) {
                AlphaBlendRect(dest, data->DestLeft + l - data->Left,
                               data->DestTop + y, s1,
                               data->Src1Left + l - d - data->Left,
                               data->Src1Top + y, s2,
                               data->Src2Left + l - d - data->Left,
                               data->Src2Top + y,
                               r - l, h, BlendRatio, DestLayerType);
            }
            y += h;
            rad = nextRad;
        }
    }

    void BlendRipple(tTVPDivisibleData *data) {
        if(TVPGetRenderManager()->IsSoftware()) {
            BlendRippleSoftware(data);
            return;
        }
        EnsureRippleResources();
        iTVPTexture2D *dest = data->Dest->GetTextureForRender();
        iTVPTexture2D *source1 =
            const_cast<iTVPScanLineProvider *>(data->Src1)->GetTexture();
        iTVPTexture2D *source2 =
            const_cast<iTVPScanLineProvider *>(data->Src2)->GetTexture();
        const tjs_int driftRows =
            RippleParameters.maxDrift * extrans::kRippleDriftPrecision;
        SetFloat(RippleMethod, "u_blendRatio", RippleFrame.blendRatio);
        SetFloat(RippleMethod, "u_phase", RippleFrame.phase);
        SetFloat(RippleMethod, "u_drift", RippleFrame.drift);
        SetFloat(RippleMethod, "u_rippleWidth", RippleParameters.rippleWidth);
        SetFloat(RippleMethod, "u_driftRows", driftRows);
        SetFloat(RippleMethod, "u_srcWidth", Width);
        SetFloat(RippleMethod, "u_srcHeight", Height);
        SetFloat(RippleMethod, "u_tex1Width", source1->GetInternalWidth());
        SetFloat(RippleMethod, "u_tex1Height", source1->GetInternalHeight());
        SetFloat(RippleMethod, "u_tex2Width", source2->GetInternalWidth());
        SetFloat(RippleMethod, "u_tex2Height", source2->GetInternalHeight());
        SetFloat(RippleMethod, "u_src1OffsetX", data->Src1Left - data->Left);
        SetFloat(RippleMethod, "u_src1OffsetY", data->Src1Top - data->Top);
        SetFloat(RippleMethod, "u_src2OffsetX", data->Src2Left - data->Left);
        SetFloat(RippleMethod, "u_src2OffsetY", data->Src2Top - data->Top);
        SetFloat(RippleMethod, "u_centerX", RippleParameters.centerX);
        SetFloat(RippleMethod, "u_centerY", RippleParameters.centerY);
        tRenderTexRectArray::Element textures[] = {
            {source1, tTVPRect(data->Src1Left, data->Src1Top,
                               data->Src1Left + data->Width,
                               data->Src1Top + data->Height)},
            {source2, tTVPRect(data->Src2Left, data->Src2Top,
                               data->Src2Left + data->Width,
                               data->Src2Top + data->Height)},
            {RippleDisplacement->GetTexture(),
             tTVPRect(data->Left, data->Top, data->Left + data->Width,
                      data->Top + data->Height)},
            {RippleDrift->GetTexture(),
             tTVPRect(0, 0,
                      RippleParameters.rippleWidth *
                          extrans::kRippleDirectionPrecision,
                      std::max(1, driftRows))},
        };
        TVPGetRenderManager()->OperateRect(
            RippleMethod, dest, nullptr,
            tTVPRect(data->DestLeft, data->DestTop,
                     data->DestLeft + data->Width,
                     data->DestTop + data->Height),
            tRenderTexRectArray(textures));
    }

    void BlendMosaic(tTVPDivisibleData *data) {
        if(TVPGetRenderManager()->IsSoftware()) {
            BlendMosaicSoftware(data);
            return;
        }
        // Official extrans mosaic: the grid is anchored in transition-logical
        // coordinates and every block is filled from its clamped center pixel.
        // A single draw preserves that mapping for arbitrary divisible chunks.
        EnsureMosaicResources();
        iTVPTexture2D *dest = data->Dest->GetTextureForRender();
        iTVPTexture2D *s1 = const_cast<iTVPScanLineProvider *>(data->Src1)
                                ->GetTexture();
        iTVPTexture2D *s2 = const_cast<iTVPScanLineProvider *>(data->Src2)
                                ->GetTexture();
        SetFloat(MosaicMethod, "u_blendRatio", MosaicFrame.blendRatio);
        SetFloat(MosaicMethod, "u_blockSize", MosaicFrame.blockSize);
        SetFloat(MosaicMethod, "u_offsetX", MosaicFrame.offsetX);
        SetFloat(MosaicMethod, "u_offsetY", MosaicFrame.offsetY);
        SetFloat(MosaicMethod, "u_width", Width);
        SetFloat(MosaicMethod, "u_height", Height);
        SetFloat(MosaicMethod, "u_tex1Width", s1->GetInternalWidth());
        SetFloat(MosaicMethod, "u_tex1Height", s1->GetInternalHeight());
        SetFloat(MosaicMethod, "u_tex2Width", s2->GetInternalWidth());
        SetFloat(MosaicMethod, "u_tex2Height", s2->GetInternalHeight());
        SetFloat(MosaicMethod, "u_src1OffsetX", data->Src1Left - data->Left);
        SetFloat(MosaicMethod, "u_src1OffsetY", data->Src1Top - data->Top);
        SetFloat(MosaicMethod, "u_src2OffsetX", data->Src2Left - data->Left);
        SetFloat(MosaicMethod, "u_src2OffsetY", data->Src2Top - data->Top);
        SetFloat(MosaicMethod, "u_logicalOffsetX",
                 data->Left - data->Src1Left);
        SetFloat(MosaicMethod, "u_logicalOffsetY",
                 data->Top - data->Src1Top);
        tRenderTexRectArray::Element textures[] = {
            {s1, tTVPRect(data->Src1Left, data->Src1Top,
                          data->Src1Left + data->Width,
                          data->Src1Top + data->Height)},
            {s2, tTVPRect(data->Src2Left, data->Src2Top,
                          data->Src2Left + data->Width,
                          data->Src2Top + data->Height)},
        };
        TVPGetRenderManager()->OperateRect(
            MosaicMethod, dest, nullptr,
            tTVPRect(data->DestLeft, data->DestTop,
                     data->DestLeft + data->Width,
                     data->DestTop + data->Height),
            tRenderTexRectArray(textures));
    }

    void BlendMosaicSoftware(tTVPDivisibleData *data) {
        auto *destinationTexture = data->Dest->GetTextureForRender();
        auto *source1Texture = data->Src1->GetTexture();
        auto *source2Texture = data->Src2->GetTexture();
        if(!destinationTexture || !source1Texture || !source2Texture)
            TVPThrowExceptionMessage(TJS_W("mosaic image access failed"));
        auto *destination = static_cast<tjs_uint8 *>(
            destinationTexture->GetScanLineForWrite(data->DestTop));
        const auto *source1 = static_cast<const tjs_uint8 *>(
            source1Texture->GetScanLineForRead(0));
        const auto *source2 = static_cast<const tjs_uint8 *>(
            source2Texture->GetScanLineForRead(0));
        if(!destination || !source1 || !source2)
            TVPThrowExceptionMessage(TJS_W("mosaic image access failed"));
        const tjs_int destinationPitch = destinationTexture->GetPitch();
        const tjs_int source1Pitch = source1Texture->GetPitch();
        const tjs_int source2Pitch = source2Texture->GetPitch();
        for(tjs_int localY = 0; localY < data->Height; ++localY) {
            const tjs_int logicalY = data->Top + localY;
            const tjs_int sampleY = extrans::ComputeMosaicSampleCoordinate(
                logicalY, Height, MosaicFrame.blockSize, MosaicFrame.offsetY);
            auto *destinationRow = reinterpret_cast<tjs_uint32 *>(
                destination + static_cast<std::ptrdiff_t>(localY) *
                                  destinationPitch);
            const auto *source1Row = reinterpret_cast<const tjs_uint32 *>(
                source1 + static_cast<std::ptrdiff_t>(
                              sampleY + data->Src1Top - data->Top) *
                              source1Pitch);
            const auto *source2Row = reinterpret_cast<const tjs_uint32 *>(
                source2 + static_cast<std::ptrdiff_t>(
                              sampleY + data->Src2Top - data->Top) *
                              source2Pitch);
            for(tjs_int localX = 0; localX < data->Width; ++localX) {
                const tjs_int logicalX = data->Left + localX;
                const tjs_int sampleX = extrans::ComputeMosaicSampleCoordinate(
                    logicalX, Width, MosaicFrame.blockSize,
                    MosaicFrame.offsetX);
                destinationRow[data->DestLeft + localX] = BlendColor(
                    source1Row[sampleX + data->Src1Left - data->Left],
                    source2Row[sampleX + data->Src2Left - data->Left],
                    MosaicFrame.blendRatio);
            }
        }
    }

    void BlendTurn(tTVPDivisibleData *data) {
        if(TVPGetRenderManager()->IsSoftware()) {
            BlendTurnSoftware(data);
            return;
        }
        EnsureTurnResources();
        iTVPTexture2D *dest = data->Dest->GetTextureForRender();
        iTVPTexture2D *source1 =
            const_cast<iTVPScanLineProvider *>(data->Src1)->GetTexture();
        iTVPTexture2D *source2 =
            const_cast<iTVPScanLineProvider *>(data->Src2)->GetTexture();
        SetFloat(TurnMethod, "u_globalPhase", TurnGlobalPhase);
        SetFloat(TurnMethod, "u_srcWidth", Width);
        SetFloat(TurnMethod, "u_srcHeight", Height);
        SetFloat(TurnMethod, "u_tex1Width", source1->GetInternalWidth());
        SetFloat(TurnMethod, "u_tex1Height", source1->GetInternalHeight());
        SetFloat(TurnMethod, "u_tex2Width", source2->GetInternalWidth());
        SetFloat(TurnMethod, "u_tex2Height", source2->GetInternalHeight());
        SetFloat(TurnMethod, "u_src1OffsetX", data->Src1Left - data->Left);
        SetFloat(TurnMethod, "u_src1OffsetY", data->Src1Top - data->Top);
        SetFloat(TurnMethod, "u_src2OffsetX", data->Src2Left - data->Left);
        SetFloat(TurnMethod, "u_src2OffsetY", data->Src2Top - data->Top);
        SetFloat(TurnMethod, "u_logicalOffsetX",
                 data->Left - data->Src1Left);
        SetFloat(TurnMethod, "u_logicalOffsetY", data->Top - data->Src1Top);
        TurnMethod->SetParameterColor4B(
            TurnMethod->EnumParameterID("u_background"), TurnBGColor);
        tRenderTexRectArray::Element textures[] = {
            {source1, tTVPRect(data->Src1Left, data->Src1Top,
                               data->Src1Left + data->Width,
                               data->Src1Top + data->Height)},
            {source2, tTVPRect(data->Src2Left, data->Src2Top,
                               data->Src2Left + data->Width,
                               data->Src2Top + data->Height)},
            {TurnLUT[0]->GetTexture(), tTVPRect(0, 0, 64, 64)},
            {TurnLUT[1]->GetTexture(), tTVPRect(0, 0, 64, 64)},
            {TurnLUT[2]->GetTexture(), tTVPRect(0, 0, 64, 64)},
            {TurnLUT[3]->GetTexture(), tTVPRect(0, 0, 64, 64)},
            {TurnLUT[4]->GetTexture(), tTVPRect(0, 0, 64, 64)},
        };
        TVPGetRenderManager()->OperateRect(
            TurnMethod, dest, nullptr,
            tTVPRect(data->DestLeft, data->DestTop,
                     data->DestLeft + data->Width,
                     data->DestTop + data->Height),
            tRenderTexRectArray(textures));
    }

    void BlendRippleSoftware(tTVPDivisibleData *data) {
        auto *destinationTexture = data->Dest->GetTextureForRender();
        auto *source1Texture = data->Src1->GetTexture();
        auto *source2Texture = data->Src2->GetTexture();
        if(!destinationTexture || !source1Texture || !source2Texture)
            TVPThrowExceptionMessage(TJS_W("ripple image access failed"));
        auto *destination = static_cast<tjs_uint8 *>(
            destinationTexture->GetScanLineForWrite(data->DestTop));
        const auto *source1 = static_cast<const tjs_uint8 *>(
            source1Texture->GetScanLineForRead(0));
        const auto *source2 = static_cast<const tjs_uint8 *>(
            source2Texture->GetScanLineForRead(0));
        const tjs_int destinationPitch = destinationTexture->GetPitch();
        const tjs_int source1Pitch = source1Texture->GetPitch();
        const tjs_int source2Pitch = source2Texture->GetPitch();
        if(!destination || !source1 || !source2)
            TVPThrowExceptionMessage(TJS_W("ripple image access failed"));
        const std::size_t driftStride =
            static_cast<std::size_t>(RippleParameters.rippleWidth) *
            extrans::kRippleDirectionPrecision;
        for(tjs_int localY = 0; localY < data->Height; ++localY) {
            const tjs_int logicalY = data->Top + localY;
            auto *destinationRow = reinterpret_cast<tjs_uint32 *>(
                destination + static_cast<std::ptrdiff_t>(localY) *
                                  destinationPitch);
            for(tjs_int localX = 0; localX < data->Width; ++localX) {
                const tjs_int logicalX = data->Left + localX;
                const tjs_int mapX = logicalX < RippleParameters.centerX
                    ? RippleParameters.centerX - logicalX - 1
                    : logicalX - RippleParameters.centerX;
                const tjs_int mapY = logicalY < RippleParameters.centerY
                    ? RippleParameters.centerY - logicalY - 1
                    : logicalY - RippleParameters.centerY;
                const std::uint16_t displacement =
                    RippleTables.displacement[static_cast<std::size_t>(mapY) *
                                                  RippleTables.mapWidth +
                                              mapX];
                tjs_int xOffset = 0;
                tjs_int yOffset = 0;
                if(RippleParameters.maxDrift > 0) {
                    const tjs_int distance =
                        displacement / extrans::kRippleDirectionPrecision;
                    const tjs_int direction =
                        displacement % extrans::kRippleDirectionPrecision;
                    const tjs_int wave =
                        (distance + RippleFrame.phase) &
                        (RippleParameters.rippleWidth - 1);
                    const std::uint16_t encoded =
                        RippleTables.drift[static_cast<std::size_t>(
                                                RippleFrame.drift) *
                                                driftStride +
                                            wave *
                                                extrans::kRippleDirectionPrecision +
                                            direction];
                    xOffset = static_cast<std::int8_t>(encoded >> 8);
                    yOffset = static_cast<std::int8_t>(encoded & 0xff);
                }
                tjs_int sampleX = logicalX -
                    (logicalX < RippleParameters.centerX ? -xOffset : xOffset);
                tjs_int sampleY = logicalY -
                    (logicalY < RippleParameters.centerY ? -yOffset : yOffset);
                if(sampleX < 0) sampleX = -sampleX;
                if(sampleY < 0) sampleY = -sampleY;
                if(sampleX >= Width)
                    sampleX = Width - 1 - (sampleX - Width);
                if(sampleY >= Height)
                    sampleY = Height - 1 - (sampleY - Height);
                sampleX = std::clamp(sampleX, 0, Width - 1);
                sampleY = std::clamp(sampleY, 0, Height - 1);
                const auto *source1Row = reinterpret_cast<const tjs_uint32 *>(
                    source1 + static_cast<std::ptrdiff_t>(sampleY +
                                                          data->Src1Top -
                                                          data->Top) *
                                  source1Pitch);
                const auto *source2Row = reinterpret_cast<const tjs_uint32 *>(
                    source2 + static_cast<std::ptrdiff_t>(sampleY +
                                                          data->Src2Top -
                                                          data->Top) *
                                  source2Pitch);
                destinationRow[data->DestLeft + localX] = BlendColor(
                    source1Row[sampleX + data->Src1Left - data->Left],
                    source2Row[sampleX + data->Src2Left - data->Left],
                    RippleFrame.blendRatio);
            }
        }
    }

    void BlendTurnSoftware(tTVPDivisibleData *data) {
        auto *destinationTexture = data->Dest->GetTextureForRender();
        auto *source1Texture = data->Src1->GetTexture();
        auto *source2Texture = data->Src2->GetTexture();
        if(!destinationTexture || !source1Texture || !source2Texture)
            TVPThrowExceptionMessage(TJS_W("turn image access failed"));
        auto *destination = static_cast<tjs_uint8 *>(
            destinationTexture->GetScanLineForWrite(data->DestTop));
        const auto *source1 = static_cast<const tjs_uint8 *>(
            source1Texture->GetScanLineForRead(0));
        const auto *source2 = static_cast<const tjs_uint8 *>(
            source2Texture->GetScanLineForRead(0));
        const tjs_int destinationPitch = destinationTexture->GetPitch();
        const tjs_int source1Pitch = source1Texture->GetPitch();
        const tjs_int source2Pitch = source2Texture->GetPitch();
        if(!destination || !source1 || !source2)
            TVPThrowExceptionMessage(TJS_W("turn image access failed"));
        for(tjs_int localY = 0; localY < data->Height; ++localY) {
            const tjs_int logicalY = data->Top + localY;
            auto *destinationRow = reinterpret_cast<tjs_uint32 *>(
                destination + static_cast<std::ptrdiff_t>(localY) *
                                  destinationPitch);
            for(tjs_int localX = 0; localX < data->Width; ++localX) {
                const tjs_int logicalX = data->Left + localX;
                const auto mapping = extrans::MapTurnPixel(
                    TurnGlobalPhase, logicalX, logicalY);
                if(mapping.background) {
                    destinationRow[data->DestLeft + localX] = TurnBGColor;
                    continue;
                }
                const tjs_int sourceX = mapping.sourceX;
                const tjs_int sourceY = mapping.sourceY;
                if(sourceX < 0 || sourceY < 0 || sourceX >= Width ||
                   sourceY >= Height) {
                    destinationRow[data->DestLeft + localX] = TurnBGColor;
                    continue;
                }
                const auto *bytes = mapping.source2 ? source2 : source1;
                const tjs_int pitch =
                    mapping.source2 ? source2Pitch : source1Pitch;
                const tjs_int left =
                    mapping.source2 ? data->Src2Left : data->Src1Left;
                const tjs_int top =
                    mapping.source2 ? data->Src2Top : data->Src1Top;
                const auto *row = reinterpret_cast<const tjs_uint32 *>(
                    bytes + static_cast<std::ptrdiff_t>(sourceY + top -
                                                        data->Top) * pitch);
                tjs_uint32 color = row[sourceX + left - data->Left];
                if(mapping.gloss)
                    color = extrans::ApplyTurnGloss(color, mapping.gloss);
                destinationRow[data->DestLeft + localX] = color;
            }
        }
    }

    // Official tTVPRotateZoomTransHandler::CalcPosition (shared by
    // rotatezoom/rotatevanish). Fixed source covers the whole layer, the
    // other source is rotated+scaled around center with accel/twist curves.
    void CalcRotateZoomPos(float p) {
        tjs_int scx = Width / 2, scy = Height / 2;
        float zm = p, tm = p;
        if(Accel < 0) {
            zm = 1.0 - zm;
            zm = static_cast<float>(pow(static_cast<double>(zm), -Accel));
            zm = 1.0 - zm;
        } else if(Accel > 0) {
            zm = static_cast<float>(pow(static_cast<double>(zm), Accel));
        }
        tjs_int cx = extrans::TruncateRotateCoordinate(
            (static_cast<float>(scx) - static_cast<float>(CenterX)) * zm +
            static_cast<float>(CenterX));
        tjs_int cy = extrans::TruncateRotateCoordinate(
            (static_cast<float>(scy) - static_cast<float>(CenterY)) * zm +
            static_cast<float>(CenterY));
        if(TwistAccel < 0) {
            tm = 1.0 - tm;
            tm = static_cast<float>(pow(static_cast<double>(tm), -TwistAccel));
            tm = 1.0 - tm;
        } else if(TwistAccel > 0) {
            tm = static_cast<float>(pow(static_cast<double>(tm), TwistAccel));
        }
        float rad = (p >= 1.0f)
            ? 0.0f
            : static_cast<float>(2.0 * 3.14159265368979 * Twist * tm);
        zm = static_cast<float>((TargetFactor - Factor) * zm + Factor);
        float s = std::sin(rad) * zm, c = std::cos(rad) * zm;
        tTVPPointD pts[4];
        pts[0].x = extrans::TruncateRotateCoordinate(-cx * c + -cy * s + scx);
        pts[0].y = extrans::TruncateRotateCoordinate(-cx * -s + -cy * c + scy);
        pts[1].x = extrans::TruncateRotateCoordinate(
            (Width - 1 - cx) * c + -cy * s + scx);
        pts[1].y = extrans::TruncateRotateCoordinate(
            (Width - 1 - cx) * -s + -cy * c + scy);
        pts[2].x = extrans::TruncateRotateCoordinate(
            -cx * c + (Height - 1 - cy) * s + scx);
        pts[2].y = extrans::TruncateRotateCoordinate(
            -cx * -s + (Height - 1 - cy) * c + scy);
        // 4th corner = parallelogram (points[1] - points[0] + points[2])
        pts[3].x = pts[1].x - pts[0].x + pts[2].x;
        pts[3].y = pts[1].y - pts[0].y + pts[2].y;
        if(FixSrc1) {
            DrawQuad1 = false; // src1 fixed full-screen
            memcpy(Quad2, pts, sizeof(Quad2));
            DrawQuad2 = true;
        } else {
            DrawQuad2 = false; // src2 fixed full-screen
            memcpy(Quad1, pts, sizeof(Quad1));
            DrawQuad1 = true;
        }
    }

    // Official tTVPRotateSwapTransHandler::CalcPosition: both sources rotate,
    // which one ends up on top depends on the half of the transition.
    void CalcRotateSwapPos(float p) {
        tjs_int scx = Width / 2, scy = Height / 2;
        float zm = p;
        float twist = static_cast<float>(
            Twist * 2.0 * 3.14159265368979); // official ctor
        float tm, rad, s, c;
        tjs_int cx, cy;
        tTVPPointD q1[4], q2[4];

        // src1
        tm = zm * zm;
        cx = extrans::TruncateRotateCoordinate(
            (-scx) * tm + scx +
            std::sin(static_cast<float>(tm * 3.14159265368979)) * scx * 1.5f);
        cy = extrans::TruncateRotateCoordinate((-scy) * tm + scy);
        rad = tm * twist;
        tm = 1.0 - tm;
        s = std::sin(rad) * tm;
        c = std::cos(rad) * tm;
        q1[0].x = extrans::TruncateRotateCoordinate(-scx * c + -scy * s + cx);
        q1[0].y = extrans::TruncateRotateCoordinate(
            (-scx * -s + -scy * c) * tm + cy);
        q1[1].x = extrans::TruncateRotateCoordinate(
            (Width - 1 - scx) * c + -scy * s + cx);
        q1[1].y = extrans::TruncateRotateCoordinate(
            ((Width - 1 - scx) * -s + -scy * c) * tm + cy);
        q1[2].x = extrans::TruncateRotateCoordinate(
            -scx * c + (Height - 1 - scy) * s + cx);
        q1[2].y = extrans::TruncateRotateCoordinate(
            (-scx * -s + (Height - 1 - scy) * c) * tm + cy);
        q1[3].x = q1[1].x - q1[0].x + q1[2].x;
        q1[3].y = q1[1].y - q1[0].y + q1[2].y;

        // src2
        tm = 1.0 - (1.0 - zm) * (1.0 - zm);
        cx = extrans::TruncateRotateCoordinate(
            (scx - (Width - 1)) * tm + (Width - 1) -
            std::sin(static_cast<float>(tm * 3.14159265368979)) * scx * 1.5f);
        cy = extrans::TruncateRotateCoordinate(
            (scy - (Height - 1)) * tm + (Height - 1));
        rad = (-1.0 + tm) * twist;
        s = std::sin(rad) * tm;
        c = std::cos(rad) * tm;
        q2[0].x = extrans::TruncateRotateCoordinate(-scx * c + -scy * s + cx);
        q2[0].y = extrans::TruncateRotateCoordinate(
            (-scx * -s + -scy * c) * tm + cy);
        q2[1].x = extrans::TruncateRotateCoordinate(
            (Width - 1 - scx) * c + -scy * s + cx);
        q2[1].y = extrans::TruncateRotateCoordinate(
            ((Width - 1 - scx) * -s + -scy * c) * tm + cy);
        q2[2].x = extrans::TruncateRotateCoordinate(
            -scx * c + (Height - 1 - scy) * s + cx);
        q2[2].y = extrans::TruncateRotateCoordinate(
            (-scx * -s + (Height - 1 - scy) * c) * tm + cy);
        q2[3].x = q2[1].x - q2[0].x + q2[2].x;
        q2[3].y = q2[1].y - q2[0].y + q2[2].y;

        memcpy(Quad1, q1, sizeof(Quad1));
        memcpy(Quad2, q2, sizeof(Quad2));
        DrawQuad1 = DrawQuad2 = true;
    }

    void BlendRotate(tTVPDivisibleData *data) {
        // GPU port of official tTVPBaseRotateTransHandler::Process: fill the
        // background first, then draw sources in AddSource order so the later
        // one covers the earlier one. Fixed full-screen sources are plain
        // copies, the moving one uses a perspective quad.
        iTVPTexture2D *dest = data->Dest->GetTextureForRender();
        iTVPTexture2D *s1 = const_cast<iTVPScanLineProvider *>(data->Src1)
                                ->GetTexture();
        iTVPTexture2D *s2 = const_cast<iTVPScanLineProvider *>(data->Src2)
                                ->GetTexture();
        tTVPRect clip(data->DestLeft, data->DestTop,
                      data->DestLeft + data->Width,
                      data->DestTop + data->Height);
        const tjs_int destinationOffsetX = data->DestLeft - data->Left;
        const tjs_int destinationOffsetY = data->DestTop - data->Top;

        if(Effect == ExEffect::RotateSwap) {
            FillRectColor(dest, clip.left, clip.top, data->Width, data->Height,
                          RotateBGColor);
        }

        if(Effect == ExEffect::RotateZoom) {
            // src1 fixed, src2 rotates on top
            CopyRectTex(dest, data->DestLeft, data->DestTop, s1,
                        data->Src1Left, data->Src1Top, data->Width,
                        data->Height);
            PerspectiveQuad(dest, s2, Quad2,
                            data->Src2Left - data->Left,
                            data->Src2Top - data->Top, Width, Height,
                            destinationOffsetX, destinationOffsetY, clip);
        } else if(Effect == ExEffect::RotateVanish) {
            // src2 fixed, src1 rotates on top (vanish)
            CopyRectTex(dest, data->DestLeft, data->DestTop, s2,
                        data->Src2Left, data->Src2Top, data->Width,
                        data->Height);
            PerspectiveQuad(dest, s1, Quad1,
                            data->Src1Left - data->Left,
                            data->Src1Top - data->Top, Width, Height,
                            destinationOffsetX, destinationOffsetY, clip);
        } else {
            // RotateSwap: first half src1 covers src2, second half reversed
            const double p = Progress();
            if(p < 0.5) {
                PerspectiveQuad(dest, s2, Quad2,
                                data->Src2Left - data->Left,
                                data->Src2Top - data->Top, Width, Height,
                                destinationOffsetX, destinationOffsetY, clip);
                PerspectiveQuad(dest, s1, Quad1,
                                data->Src1Left - data->Left,
                                data->Src1Top - data->Top, Width, Height,
                                destinationOffsetX, destinationOffsetY, clip);
            } else {
                PerspectiveQuad(dest, s1, Quad1,
                                data->Src1Left - data->Left,
                                data->Src1Top - data->Top, Width, Height,
                                destinationOffsetX, destinationOffsetY, clip);
                PerspectiveQuad(dest, s2, Quad2,
                                data->Src2Left - data->Left,
                                data->Src2Top - data->Top, Width, Height,
                                destinationOffsetX, destinationOffsetY, clip);
            }
        }
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
    RegisterOne(TJS_W("ripple"), ExEffect::Ripple);
    RegisterOne(TJS_W("mosaic"), ExEffect::Mosaic);
    RegisterOne(TJS_W("turn"), ExEffect::Turn);
    RegisterOne(TJS_W("rotatezoom"), ExEffect::RotateZoom);
    RegisterOne(TJS_W("rotateswap"), ExEffect::RotateSwap);
    RegisterOne(TJS_W("rotatevanish"), ExEffect::RotateVanish);

    TVPAddImportantLog(
        TJS_W("extrans: native transition providers registered "
              "(wave/ripple/mosaic/turn/rotate*)"));
}

} // namespace

NCB_PRE_REGIST_CALLBACK(InitPlugin_Extrans);

extern "C" void TVPExtransPluginAnchor() {}
