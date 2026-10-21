// gfxEffect.dll
//
// The gfxFire class of kaede-software gfxEffect.dll: a fire effect that is
// drawn into a Layer. The original plugin is closed source. The algorithm
// comes from a decompilation of the shipped x86 DLL; it is in gfxFireCore.hpp.
// This file only binds that core to TJS (properties, methods, Layer access).
//
// Calls a game can make (all of them exist in the original class):
//   new gfxFire()
//   init(layer [, numOfSeeds = 1])   set the target layer and the seed count
//   cycle()                          one animation step, then layer.update()
//   updateWarpMap(), updateCoolMap() redo the random warp mesh / cool map
//   setFireSeed(layer [, index = 0]) copy the alpha of a layer as heat source
//   setFireSeedPos(x, y [, index = 0]), clearFireSeed([index = 0])
//   applyRed(), applyBlue(), applyWhite(), applyCustom()
//   setCustomColorTable(layer)       row 0, first 256 pixels, shared by all
//   finalize()
//   properties: targetLayer (write), seedLayer (write), seedX, seedY,
//   numOfSeeds (read), isRed (read), randomSeed, forceH, forceV,
//   boundRangeH, boundRangeV, scalingCoeff, numOfBlurForCoolMap,
//   textureFilterType, coolRange, coolStrength,
//   coolParticleDensityDenominator, coolParticleDensityNumerator,
//   edgeSmoothing.
//
// Port differences:
//   - Layer pixels are RGBA in this fork. The built-in colour tables are
//     swizzled from Windows BGRA words when first used.
//   - The target layer width and height must be a multiple of 16 and at
//     least 16. Other sizes are logged and ignored.
//   - The SSE2 renderer of the DLL skips the last 8 rows. This port draws
//     all rows. See gfxFireCore.hpp for the other documented quirks.
//   - Setting randomSeed reseeds one Mersenne Twister that all instances
//     share, like the DLL.

#define NCB_MODULE_NAME TJS_W("gfxEffect.dll")

extern "C" void TVPGfxEffectPluginAnchor() {}

#include "ncbind.hpp"
#include "common/LayerPixel.h"
#include "common/PluginSafety.h"
#include "gfxFireCore.hpp"

#include <spdlog/spdlog.h>

namespace {

void LogPluginError(const char *msg) {
    if(auto logger = spdlog::get("plugin"))
        logger->error("[gfxEffect] {}", msg);
}

bool IsLayerObject(const tTJSVariant &v) {
    return v.Type() == tvtObject && v.AsObjectNoAddRef();
}

} // namespace

class gfxFire {
public:
    gfxFire() = default;
    ~gfxFire() { target_.Clear(); }

    void finalize() { target_.Clear(); }

    // ---- properties --------------------------------------------------
    tjs_int getForceH() const { return core_.p.forceH; }
    void setForceH(tjs_int v) {
        if(v >= -7 && v <= 7)
            core_.p.forceH = v;
    }
    tjs_int getForceV() const { return core_.p.forceV; }
    void setForceV(tjs_int v) {
        if(v >= -7 && v <= 7)
            core_.p.forceV = v;
    }
    tjs_int getBoundRangeH() const { return core_.p.boundRangeH; }
    void setBoundRangeH(tjs_int v) {
        if(v >= 1 && v <= 16)
            core_.p.boundRangeH = v;
    }
    tjs_int getBoundRangeV() const { return core_.p.boundRangeV; }
    void setBoundRangeV(tjs_int v) {
        if(v >= 1 && v <= 16)
            core_.p.boundRangeV = v;
    }
    tjs_int getNumOfBlurForCoolMap() const { return core_.p.numOfBlurForCoolMap; }
    void setNumOfBlurForCoolMap(tjs_int v) {
        if(v > 0)
            core_.p.numOfBlurForCoolMap = v;
    }
    tjs_int64 getRandomSeed() const { return core_.p.randomSeed; }
    void setRandomSeed(tjs_int64 v) {
        core_.p.randomSeed = static_cast<tjs_uint32>(v);
        gfxfire::reseed(core_.p.randomSeed);
    }
    tjs_int getTextureFilterType() const { return core_.p.textureFilterType; }
    void setTextureFilterType(tjs_int v) { core_.p.textureFilterType = v != 0 ? 1 : 0; }
    tjs_int getCoolRange() const { return core_.p.coolRange; }
    void setCoolRange(tjs_int v) {
        if(v >= 1 && v <= 255)
            core_.p.coolRange = v;
    }
    tjs_int getCoolStrength() const { return core_.p.coolStrength; }
    void setCoolStrength(tjs_int v) {
        if(v >= 0 && v < 0xff)
            core_.p.coolStrength = v;
    }
    tjs_int64 getCoolDenominator() const { return core_.p.denominator; }
    void setCoolDenominator(tjs_int64 v) {
        if(v > 0 && v <= 0xffffffffLL)
            core_.p.denominator = static_cast<tjs_uint32>(v);
    }
    tjs_int64 getCoolNumerator() const { return core_.p.numerator; }
    void setCoolNumerator(tjs_int64 v) {
        if(v > 0 && v <= 0xffffffffLL)
            core_.p.numerator = static_cast<tjs_uint32>(v);
    }
    bool getEdgeSmoothing() const { return core_.p.edgeSmoothing; }
    void setEdgeSmoothing(bool v) { core_.p.edgeSmoothing = v; }
    double getScalingCoeff() const { return core_.p.scalingCoeff; }
    void setScalingCoeff(double v) {
        if(v > 0.0 && v < 0.01)
            core_.p.scalingCoeff = static_cast<float>(v);
    }
    bool getIsRed() const { return core_.p.palette == gfxfire::kPaletteRed; }
    tjs_int getNumOfSeeds() const { return static_cast<tjs_int>(core_.seeds.size()); }
    tjs_int getSeedX() const { return core_.seeds.empty() ? 0 : core_.seeds[0].x; }
    void setSeedX(tjs_int v) {
        if(!core_.seeds.empty())
            core_.seeds[0].x = v;
    }
    tjs_int getSeedY() const { return core_.seeds.empty() ? 0 : core_.seeds[0].y; }
    void setSeedY(tjs_int v) {
        if(!core_.seeds.empty())
            core_.seeds[0].y = v;
    }
    void setTargetLayer(tTJSVariant layer) { setTarget(layer); }
    void setSeedLayer(tTJSVariant layer) { setFireSeed(layer, 0); }

    // ---- methods -----------------------------------------------------
    tjs_error init(tjs_int numparams, tTJSVariant **param) {
        if(numparams < 1)
            return TJS_E_BADPARAMCOUNT;
        const tjs_int seeds = numparams >= 2 ? static_cast<tjs_int>(*param[1]) : 1;
        // init() drops the maps and the seeds, then sets the target again.
        core_.F.free();
        core_.W.free();
        core_.K.free();
        core_.seeds.assign(static_cast<std::size_t>(seeds < 0 ? 0 : seeds), gfxfire::Seed());
        setTarget(*param[0]);
        return TJS_S_OK;
    }

    void cycle() {
        if(!IsLayerObject(target_))
            return;
        const auto view = pluginSafety::LayerWriteView::create(target_.AsObjectNoAddRef());
        if(!view)
            return;
        if(!core_.ready() || view.value.width() != core_.w || view.value.height() != core_.h ||
           view.value.pixels() != lastPixels_ || view.value.pitchBytes() != lastPitch_) {
            if(!acceptSize(view.value.width(), view.value.height()))
                return;
            core_.setTarget(view.value.width(), view.value.height());
            lastPixels_ = view.value.pixels();
            lastPitch_ = view.value.pitchBytes();
        }
        core_.cycle(view.value.pixels(), view.value.pitchBytes());
        notifyUpdate(target_.AsObjectNoAddRef());
    }

    void updateWarpMap() { core_.updateWarp(); }
    void updateCoolMap() { core_.updateCool(); }

    tjs_error clearFireSeed(tjs_int numparams, tTJSVariant **param) {
        core_.clearFireSeed(numparams >= 1 ? static_cast<tjs_int>(*param[0]) : 0);
        return TJS_S_OK;
    }
    tjs_error setFireSeedMethod(tjs_int numparams, tTJSVariant **param) {
        if(numparams < 1)
            return TJS_E_BADPARAMCOUNT;
        setFireSeed(*param[0], numparams >= 2 ? static_cast<tjs_int>(*param[1]) : 0);
        return TJS_S_OK;
    }
    tjs_error setFireSeedPos(tjs_int numparams, tTJSVariant **param) {
        if(numparams < 2)
            return TJS_E_BADPARAMCOUNT;
        const tjs_int idx = numparams >= 3 ? static_cast<tjs_int>(*param[2]) : 0;
        if(idx >= 0 && idx < static_cast<tjs_int>(core_.seeds.size())) {
            core_.seeds[idx].x = static_cast<tjs_int>(*param[0]);
            core_.seeds[idx].y = static_cast<tjs_int>(*param[1]);
        }
        return TJS_S_OK;
    }
    tjs_error setCustomColorTable(tjs_int numparams, tTJSVariant **param) {
        if(numparams < 1)
            return TJS_E_BADPARAMCOUNT;
        if(IsLayerObject(*param[0])) {
            const auto view = pluginSafety::LayerReadView::create(param[0]->AsObjectNoAddRef());
            // The DLL copies the first 256 pixels of row 0 as raw words.
            if(view && view.value.width() >= 256)
                std::memcpy(gfxfire::detail::customTable().data(), view.value.pixels(), 256 * 4);
        }
        core_.p.palette = gfxfire::kPaletteCustom;
        return TJS_S_OK;
    }
    void applyBlue() { core_.p.palette = gfxfire::kPaletteBlue; }
    void applyRed() { core_.p.palette = gfxfire::kPaletteRed; }
    void applyWhite() { core_.p.palette = gfxfire::kPaletteWhite; }
    void applyCustom() { core_.p.palette = gfxfire::kPaletteCustom; }

private:
    static bool acceptSize(tjs_int w, tjs_int h) {
        if(w >= 16 && h >= 16 && w % 16 == 0 && h % 16 == 0)
            return true;
        LogPluginError("target layer width and height must be multiples of 16");
        return false;
    }

    void setTarget(const tTJSVariant &layer) {
        if(!IsLayerObject(layer)) {
            LogPluginError("targetLayer needs a Layer object");
            return;
        }
        const auto view = pluginSafety::LayerWriteView::create(layer.AsObjectNoAddRef());
        if(!view || !acceptSize(view.value.width(), view.value.height()))
            return;
        target_ = layer;
        core_.setTarget(view.value.width(), view.value.height());
        lastPixels_ = view.value.pixels();
        lastPitch_ = view.value.pitchBytes();
    }

    void setFireSeed(const tTJSVariant &layer, tjs_int index) {
        if(!IsLayerObject(layer)) {
            LogPluginError("setFireSeed needs a Layer object");
            return;
        }
        const auto view = pluginSafety::LayerReadView::create(layer.AsObjectNoAddRef());
        if(!view)
            return;
        // Only the alpha byte of each pixel is the heat of the seed.
        core_.setFireSeed(index, view.value.pixels(), view.value.width(),
                          view.value.height(), view.value.pitchBytes());
    }

    // The DLL calls layer.update(imageLeft, imageTop, imageWidth, imageHeight).
    static void notifyUpdate(iTJSDispatch2 *layer) {
        tTJSVariant args[4];
        const tjs_char *names[4] = {TJS_W("imageLeft"), TJS_W("imageTop"), TJS_W("imageWidth"),
                                    TJS_W("imageHeight")};
        iTJSDispatch2 *layerClass = pluginSafety::detail::nativeLayerClass();
        for(int i = 0; i < 4; ++i)
            if(!layerClass || TJS_FAILED(layerClass->PropGet(0, names[i], nullptr, &args[i], layer)))
                return;
        tTJSVariant *params[4] = {&args[0], &args[1], &args[2], &args[3]};
        layer->FuncCall(0, TJS_W("update"), nullptr, nullptr, 4, params, layer);
    }

    gfxfire::FireCore core_;
    tTJSVariant target_;
    const tjs_uint8 *lastPixels_ = nullptr;
    tjs_int lastPitch_ = 0;
};

namespace {
template <tjs_error (gfxFire::*Method)(tjs_int, tTJSVariant **)>
tjs_error Raw(tTJSVariant *, tjs_int numparams, tTJSVariant **param, gfxFire *self) {
    return (self->*Method)(numparams, param);
}
} // namespace

NCB_REGISTER_CLASS(gfxFire) {
    Constructor();
    NCB_METHOD(finalize);
    NCB_METHOD_RAW_CALLBACK(init, (Raw<&gfxFire::init>), 0);
    NCB_METHOD(cycle);
    NCB_METHOD(updateWarpMap);
    NCB_METHOD(updateCoolMap);
    NCB_METHOD_RAW_CALLBACK(clearFireSeed, (Raw<&gfxFire::clearFireSeed>), 0);
    NCB_METHOD_RAW_CALLBACK(setFireSeed, (Raw<&gfxFire::setFireSeedMethod>), 0);
    NCB_METHOD_RAW_CALLBACK(setFireSeedPos, (Raw<&gfxFire::setFireSeedPos>), 0);
    NCB_METHOD_RAW_CALLBACK(setCustomColorTable, (Raw<&gfxFire::setCustomColorTable>), 0);
    NCB_METHOD(applyBlue);
    NCB_METHOD(applyRed);
    NCB_METHOD(applyWhite);
    NCB_METHOD(applyCustom);
    NCB_PROPERTY_RO(isRed, getIsRed);
    NCB_PROPERTY_WO(targetLayer, setTargetLayer);
    NCB_PROPERTY_WO(seedLayer, setSeedLayer);
    NCB_PROPERTY(seedX, getSeedX, setSeedX);
    NCB_PROPERTY(seedY, getSeedY, setSeedY);
    NCB_PROPERTY_RO(numOfSeeds, getNumOfSeeds);
    NCB_PROPERTY(randomSeed, getRandomSeed, setRandomSeed);
    NCB_PROPERTY(forceH, getForceH, setForceH);
    NCB_PROPERTY(forceV, getForceV, setForceV);
    NCB_PROPERTY(boundRangeH, getBoundRangeH, setBoundRangeH);
    NCB_PROPERTY(boundRangeV, getBoundRangeV, setBoundRangeV);
    NCB_PROPERTY(scalingCoeff, getScalingCoeff, setScalingCoeff);
    NCB_PROPERTY(numOfBlurForCoolMap, getNumOfBlurForCoolMap, setNumOfBlurForCoolMap);
    NCB_PROPERTY(textureFilterType, getTextureFilterType, setTextureFilterType);
    NCB_PROPERTY(coolRange, getCoolRange, setCoolRange);
    NCB_PROPERTY(coolStrength, getCoolStrength, setCoolStrength);
    NCB_PROPERTY(coolParticleDensityDenominator, getCoolDenominator, setCoolDenominator);
    NCB_PROPERTY(coolParticleDensityNumerator, getCoolNumerator, setCoolNumerator);
    NCB_PROPERTY(edgeSmoothing, getEdgeSmoothing, setEdgeSmoothing);
}
