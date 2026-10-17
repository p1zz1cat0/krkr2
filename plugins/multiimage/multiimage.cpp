// multiimage.dll
//
// Ported from AetherKrkr plugins/multiimage.cpp (GPLv3, same as this fork).
// The original Windows plugin has no public source. This port keeps the TJS
// surface that games use and delegates the drawing to the core Layer methods
// affineCopy and operateAffine.

#define NCB_MODULE_NAME TJS_W("multiimage.dll")

extern "C" void TVPMultiImagePluginAnchor() {}

#include "ncbind.hpp"

#include <vector>

struct MultiImageEntry {
    tTJSVariant img;
    int x;
    int y;
    double zoomX;
    double zoomY;
    int time;
};

class MultiImage {
    std::vector<MultiImageEntry> queue;

public:
    MultiImage() = default;
    ~MultiImage() = default;

    void clear() { queue.clear(); }

    void addZoom(double zoomX, double zoomY, int time) {
        if(!queue.empty()) {
            queue.back().zoomX = zoomX;
            queue.back().zoomY = zoomY;
            queue.back().time = time;
        }
    }

    void push(tTJSVariant img, int x, int y) {
        MultiImageEntry entry;
        entry.img = img;
        entry.x = x;
        entry.y = y;
        entry.zoomX = 1.0;
        entry.zoomY = 1.0;
        entry.time = 0;
        queue.push_back(entry);
    }

    void divisionLayer(double div) { (void)div; }

    tTJSVariant calcMultiAffine(double a, double b, double c, double d,
                                double tx, double ty, int w, int h, int opa,
                                int mode) {
        (void)a;
        (void)b;
        (void)c;
        (void)d;
        (void)tx;
        (void)ty;
        (void)w;
        (void)h;
        (void)opa;
        (void)mode;
        return tTJSVariant();
    }

    tTJSVariant count() const {
        return tTJSVariant(static_cast<tjs_int>(queue.size()));
    }

    const std::vector<MultiImageEntry> &getQueue() const { return queue; }
};

NCB_REGISTER_CLASS(MultiImage) {
    NCB_CONSTRUCTOR(());
    NCB_METHOD(clear);
    NCB_METHOD(addZoom);
    NCB_METHOD(push);
    NCB_METHOD(divisionLayer);
    NCB_METHOD(calcMultiAffine);
    NCB_PROPERTY_RO(count, count);
}

namespace {

struct SourceRect {
    tTJSVariant src;
    tTJSVariant x;
    tTJSVariant y;
    tTJSVariant w;
    tTJSVariant h;
    tjs_real imageLeft = 0;
    tjs_real imageTop = 0;
};

SourceRect ExtractImageInfo(const tTJSVariant &img) {
    SourceRect info;
    info.src = img;
    info.x = tTJSVariant(0);
    info.y = tTJSVariant(0);
    info.w = tTJSVariant(0);
    info.h = tTJSVariant(0);

    if(img.Type() != tvtObject)
        return info;

    iTJSDispatch2 *dsp = img.AsObjectNoAddRef();
    if(!dsp)
        return info;

    tTJSVariant value;
    if(TJS_SUCCEEDED(dsp->PropGet(0, TJS_W("clipLeft"), nullptr, &value, dsp)))
        info.x = value;
    if(TJS_SUCCEEDED(dsp->PropGet(0, TJS_W("clipTop"), nullptr, &value, dsp)))
        info.y = value;
    if(TJS_SUCCEEDED(dsp->PropGet(0, TJS_W("clipWidth"), nullptr, &value, dsp)))
        info.w = value;
    if(TJS_SUCCEEDED(dsp->PropGet(0, TJS_W("clipHeight"), nullptr, &value, dsp)))
        info.h = value;
    if(TJS_SUCCEEDED(dsp->PropGet(0, TJS_W("imageLeft"), nullptr, &value, dsp)))
        info.imageLeft = static_cast<tjs_real>(value);
    if(TJS_SUCCEEDED(dsp->PropGet(0, TJS_W("imageTop"), nullptr, &value, dsp)))
        info.imageTop = static_cast<tjs_real>(value);
    return info;
}

MultiImage *GetMultiImage(const tTJSVariant &value) {
    if(value.Type() != tvtObject)
        return nullptr;
    iTJSDispatch2 *dsp = value.AsObjectNoAddRef();
    if(!dsp)
        return nullptr;
    return ncbInstanceAdaptor<MultiImage>::GetNativeInstance(dsp);
}

} // namespace

class LayerExMulti {
public:
    static tjs_error multiAffineCopy(tTJSVariant *result, tjs_int numparams,
                                     tTJSVariant **param,
                                     iTJSDispatch2 *objthis) {
        (void)result;
        if(numparams < 10 || !objthis)
            return TJS_E_BADPARAMCOUNT;

        MultiImage *multi = GetMultiImage(*param[0]);
        if(!multi)
            return TJS_S_OK;

        const double a = static_cast<double>(*param[1]);
        const double b = static_cast<double>(*param[2]);
        const double c = static_cast<double>(*param[3]);
        const double d = static_cast<double>(*param[4]);
        const double tx = static_cast<double>(*param[5]);
        const double ty = static_cast<double>(*param[6]);
        const int opa = static_cast<int>(*param[7]);
        const int mode = static_cast<int>(*param[8]);
        const int type = static_cast<int>(*param[9]);
        // Core affineCopy has no blend mode and no opacity. Keep both
        // parameters to match the script signature.
        (void)mode;
        (void)opa;

        for(const auto &entry : multi->getQueue()) {
            SourceRect info = ExtractImageInfo(entry.img);

            const double finalA = a * entry.zoomX;
            const double finalB = b * entry.zoomX;
            const double finalC = c * entry.zoomY;
            const double finalD = d * entry.zoomY;
            const double localX = entry.x + info.imageLeft;
            const double localY = entry.y + info.imageTop;
            const double finalTx = a * localX + c * localY + tx;
            const double finalTy = b * localX + d * localY + ty;

            // Core affineCopy takes 14 parameters: the last one is clear.
            // AetherKrkr passes a 15th opacity argument, but this fork has
            // no opacity parameter and would read it as clear.
            tTJSVariant args[14];
            args[0] = info.src;
            args[1] = info.x;
            args[2] = info.y;
            args[3] = info.w;
            args[4] = info.h;
            args[5] = tTJSVariant(static_cast<tjs_int>(1)); // affine mode
            args[6] = tTJSVariant(finalA);
            args[7] = tTJSVariant(finalB);
            args[8] = tTJSVariant(finalC);
            args[9] = tTJSVariant(finalD);
            args[10] = tTJSVariant(finalTx);
            args[11] = tTJSVariant(finalTy);
            args[12] = tTJSVariant(type);
            args[13] = tTJSVariant(static_cast<tjs_int>(0)); // no clear

            tTJSVariant *argsPtr[14] = {
                &args[0],  &args[1],  &args[2],  &args[3],  &args[4],
                &args[5],  &args[6],  &args[7],  &args[8],  &args[9],
                &args[10], &args[11], &args[12], &args[13],
            };
            objthis->FuncCall(0, TJS_W("affineCopy"), nullptr, nullptr, 14,
                              argsPtr, objthis);
        }

        return TJS_S_OK;
    }

    static tjs_error operateMultiAffine(tTJSVariant *result, tjs_int numparams,
                                        tTJSVariant **param,
                                        iTJSDispatch2 *objthis) {
        (void)result;
        if(numparams < 9 || !objthis)
            return TJS_E_BADPARAMCOUNT;

        MultiImage *multi = GetMultiImage(*param[0]);
        if(!multi)
            return TJS_S_OK;

        const double a = static_cast<double>(*param[1]);
        const double b = static_cast<double>(*param[2]);
        const double c = static_cast<double>(*param[3]);
        const double d = static_cast<double>(*param[4]);
        const double tx = static_cast<double>(*param[5]);
        const double ty = static_cast<double>(*param[6]);
        const int opa = static_cast<int>(*param[7]);
        const int mode = static_cast<int>(*param[8]);

        bool clear = false;
        if(numparams >= 10 && param[9]->Type() != tvtVoid)
            clear = static_cast<int>(*param[9]) != 0;

        if(clear) {
            tTJSVariant value;
            tjs_uint32 color = 0;
            if(TJS_SUCCEEDED(
                   objthis->PropGet(0, TJS_W("neutralColor"), nullptr, &value,
                                    objthis)))
                color = static_cast<tjs_uint32>(static_cast<tjs_int>(value));

            tTJSVariant clearArgs[5];
            clearArgs[0] = tTJSVariant(static_cast<tjs_int>(0));
            clearArgs[1] = tTJSVariant(static_cast<tjs_int>(0));
            clearArgs[2] = tTJSVariant(static_cast<tjs_int>(0));
            clearArgs[3] = tTJSVariant(static_cast<tjs_int>(0));
            clearArgs[4] = tTJSVariant(static_cast<tjs_int64>(color));

            tTJSVariant width;
            tTJSVariant height;
            if(TJS_SUCCEEDED(
                   objthis->PropGet(0, TJS_W("width"), nullptr, &width, objthis)))
                clearArgs[2] = width;
            if(TJS_SUCCEEDED(objthis->PropGet(0, TJS_W("height"), nullptr,
                                              &height, objthis)))
                clearArgs[3] = height;

            tTJSVariant *clearPtr[5] = {
                &clearArgs[0], &clearArgs[1], &clearArgs[2], &clearArgs[3],
                &clearArgs[4],
            };
            objthis->FuncCall(0, TJS_W("fillRect"), nullptr, nullptr, 5,
                              clearPtr, objthis);
        }

        for(const auto &entry : multi->getQueue()) {
            SourceRect info = ExtractImageInfo(entry.img);
            if(info.src.Type() != tvtObject)
                continue;

            const double localX = entry.x + info.imageLeft;
            const double localY = entry.y + info.imageTop;
            const double finalTx = a * localX + c * localY + tx;
            const double finalTy = b * localX + d * localY + ty;

            tTJSVariant isAffine(static_cast<tjs_int>(1));
            tTJSVariant varA(a);
            tTJSVariant varB(b);
            tTJSVariant varC(c);
            tTJSVariant varD(d);
            tTJSVariant varTx(finalTx);
            tTJSVariant varTy(finalTy);
            tTJSVariant paramMode(mode);
            tTJSVariant paramOpa(opa);
            tTJSVariant paramType(static_cast<tjs_int>(0)); // stNearest

            tTJSVariant *argsPtr[15] = {
                &info.src, &info.x,    &info.y,    &info.w,    &info.h,
                &isAffine, &varA,      &varB,      &varC,      &varD,
                &varTx,    &varTy,     &paramMode, &paramOpa,  &paramType,
            };
            objthis->FuncCall(0, TJS_W("operateAffine"), nullptr, nullptr, 15,
                              argsPtr, objthis);
        }

        return TJS_S_OK;
    }
};

NCB_ATTACH_CLASS(LayerExMulti, Layer) {
    RawCallback(TJS_W("multiAffineCopy"), &LayerExMulti::multiAffineCopy, 0);
    RawCallback(TJS_W("operateMultiAffine"),
                &LayerExMulti::operateMultiAffine, 0);
}
