#define NCB_MODULE_NAME TJS_W("drawdeviceD3D.dll")

#include "ncbind.hpp"
#include "DrawDevice.h"
#include "LayerManager.h"
#include "LayerBitmapIntf.h"
#include "MsgIntf.h"
#include "RenderManager.h"
#include "TVPWindow.h"
#include "WindowIntf.h"
#include "tjsArray.h"
#include "tvpgl.h"
#include "../motionplayer/D3DAdaptor.h"
#include "../motionplayer/EmotePlayerMembers.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <vector>

namespace {

// Draws a premultiplied-alpha texture onto a straight-alpha target. The
// geometry is the same as iTVPBaseBitmap::AffineBlt with a matrix: a source
// pixel (0,0) covers (-0.5,-0.5)-(0.5,0.5). The shared blend-method table has
// no bmAddAlphaOnAlpha, so this calls the GL method directly.
void affineBltPremultipliedOnAlpha(tTVPBaseTexture &target, iTVPTexture2D *source,
                                   tjs_int width, tjs_int height,
                                   const t2DAffineMatrix &m, tTVPBBStretchType type) {
    auto *mgr = TVPGetRenderManager();
    static iTVPRenderMethod *method = mgr->GetRenderMethod("AdditiveAlphaBlend_d");
    if(!method)
        return;
    const auto at = [&m](tjs_real x, tjs_real y) {
        return tTVPPointD{ m.a * x + m.c * y + m.tx, m.b * x + m.d * y + m.ty };
    };
    const tTVPPointD lt = at(-0.5, -0.5), rt = at(width - 0.5, -0.5),
                     lb = at(-0.5, height - 0.5), rb = at(width - 0.5, height - 0.5);
    const tTVPPointD dst[6] = { lt, rt, lb, rt, lb, rb };
    const tTVPPointD src[6] = {
        { 0.0, 0.0 }, { static_cast<double>(width), 0.0 },
        { 0.0, static_cast<double>(height) }, { static_cast<double>(width), 0.0 },
        { 0.0, static_cast<double>(height) },
        { static_cast<double>(width), static_cast<double>(height) },
    };
    static const int stretchTypeId = mgr->EnumParameterID("StretchType");
    mgr->SetParameterInt(stretchTypeId, static_cast<int>(type & stTypeMask));
    const tTVPRect rect(0, 0, target.GetWidth(), target.GetHeight());
    tRenderTexQuadArray::Element texture[] = { tRenderTexQuadArray::Element(source, src) };
    auto *targetTexture = target.GetTextureForRender(true, &rect);
    mgr->OperateTriangles(method, 2, targetTexture, targetTexture, rect, dst,
                          tRenderTexQuadArray(texture));
}

class D3DLayer;
class D3DPicture;
class DeviceEmotePlayer;

// The window owns this native device through its TJS drawDevice object.
// LayerManager owns its DrawBuffer. Keep input focus separate from drawing.
class DrawDeviceD3D : public tTVPDrawDevice {
public:
    DrawDeviceD3D(tjs_int width, tjs_int height) {
        setSize(width, height);
        screenRect_ = {0, 0, width, height};
    }
    ~DrawDeviceD3D() override;

    tjs_int64 getInterface() const {
        return static_cast<tjs_int64>(
            reinterpret_cast<std::intptr_t>(static_cast<const iTVPDrawDevice *>(this)));
    }

    tjs_int getWidth() const { return width_; }
    tjs_int getHeight() const { return height_; }
    void setWidth(tjs_int width) { setSize(width, height_); }
    void setHeight(tjs_int height) { setSize(width_, height); }
    void setSize(tjs_int width, tjs_int height) {
        if(width <= 0 || height <= 0)
            TVPThrowExceptionMessage(TJS_W("DrawDeviceD3D size must be positive"));
        if(transitionActive_)
            stopTransition();
        width_ = width;
        height_ = height;
        SetLockedSize(width, height);
        if(Window) {
            Window->NotifySrcResize();
            Window->RequestUpdate();
        }
    }

    tjs_int getLayerManagerIndex() const {
        return static_cast<tjs_int>(PrimaryLayerManagerIndex);
    }
    void setLayerManagerIndex(tjs_int index) {
        if(index < 0 || (!Managers.empty() &&
                         static_cast<size_t>(index) >= Managers.size()))
            TVPThrowExceptionMessage(TJS_W("DrawDeviceD3D layer manager index is out of range"));
        if(Managers.empty() && index != 0)
            TVPThrowExceptionMessage(TJS_W("DrawDeviceD3D has no layer manager"));
        PrimaryLayerManagerIndex = static_cast<size_t>(index);
    }

    tTJSVariant getPrimaryLayers() const {
        iTJSDispatch2 *array = TJSCreateArrayObject();
        tjs_int index = 0;
        for(auto *manager : Managers) {
            auto *primary = manager->GetPrimaryLayer();
            auto *owner = primary ? primary->GetOwnerNoAddRef() : nullptr;
            if(!owner)
                continue;
            tTJSVariant value(owner, owner);
            array->PropSetByNum(TJS_MEMBERENSURE, index++, &value, array);
        }
        tTJSVariant result(array, array);
        array->Release();
        return result;
    }

    // A new Array of this device's D3DLayer script objects (script subclasses
    // included) in creation order, as the original returns on every read.
    // KAGWindow.checkLayerTouch sorts it by .absolute and calls checkTouch.
    tTJSVariant getChildren() const;

    void update(tjs_real elapsed) {
        if(elapsed < 0)
            TVPThrowExceptionMessage(TJS_W("DrawDeviceD3D elapsed time must be nonnegative"));
        updateLayers(elapsed);
        Update();
        if(Window)
            Window->RequestUpdate();
    }

    // The original reports its own E-mote module; KAGWindow.initD3D then
    // probes maskMode/getModule behind typeof guards.
    bool checkEnable(tTJSString name) const { return name == TJS_W("emote"); }

    // Defaults and access match the original DLL (oracle): primary size is
    // read-only, screen rect starts at the constructed size. clearColor,
    // stretchType, bicubicParam and forceRenderTexture are stored for scripts
    // only (After never changes them from the defaults).
    tjs_int getPrimaryWidth() const { return width_; }
    tjs_int getPrimaryHeight() const { return height_; }
    void ignorePrimarySizeWrite(tjs_int) {} // the original accepts and drops writes
    tjs_int getScreenLeft() const { return screenRect_[0]; }
    tjs_int getScreenTop() const { return screenRect_[1]; }
    tjs_int getScreenWidth() const { return screenRect_[2]; }
    tjs_int getScreenHeight() const { return screenRect_[3]; }
    tjs_int64 getClearColor() const { return clearColor_; }
    void setClearColor(tjs_int64 value) { clearColor_ = static_cast<tjs_uint32>(value); }
    tjs_int getStretchType() const { return stretchType_; }
    void setStretchType(tjs_int value) { stretchType_ = value; }
    tjs_real getBicubicParam() const { return bicubicParam_; }
    void setBicubicParam(tjs_real value) { bicubicParam_ = value; }
    tjs_int getForceRenderTexture() const { return forceRenderTexture_; }
    void setForceRenderTexture(tjs_int value) { forceRenderTexture_ = value; }

    void setScreenRect(tjs_int x, tjs_int y, tjs_int width, tjs_int height) {
        screenRect_ = {x, y, width, height};
    }

    void setPrimarySize(tjs_int width, tjs_int height) {
        setSize(width, height);
    }

    tjs_real getTransState() const { return transState_; }
    void setTransState(tjs_real value) {
        if(!std::isfinite(value))
            TVPThrowExceptionMessage(TJS_W("DrawDeviceD3D transState must be finite"));
        transState_ = value;
        if(Window)
            Window->RequestUpdate();
    }

    void startTransition(iTJSDispatch2 *parameters);
    void stopTransition() {
        transitionSource_.reset();
        transitionRule_.reset();
        transitionActive_ = false;
        transState_ = 1.0;
        if(Window)
            Window->RequestUpdate();
    }

    void capture(iTJSDispatch2 *layerObject, tjs_int index);
    void registerLayer(D3DLayer *layer) { layers_.push_back(layer); }
    void unregisterLayer(D3DLayer *layer) {
        layers_.erase(std::remove(layers_.begin(), layers_.end(), layer),
                      layers_.end());
    }

    void AddLayerManager(iTVPLayerManager *manager) override {
        if(!manager)
            TVPThrowExceptionMessage(TJS_W("DrawDeviceD3D layer manager is null"));
        tTVPDrawDevice::AddLayerManager(manager);
        manager->SetDesiredLayerType(ltAlpha);
        if(auto *concreteManager = dynamic_cast<tTVPLayerManager *>(manager)) {
            static const bool cacheEnabled = [] {
                const char *value = std::getenv("KRKR_D3D_KAG_CACHE");
                return !value || value[0] != '0' || value[1] != '\0';
            }();
            const bool cacheGPU = cacheEnabled && !TVPIsSoftwareRenderManager();
            concreteManager->SetDrawBufferCacheEnabled(cacheGPU);
            if(cacheGPU)
                concreteManager->SetHoldAlpha(false);
        }
    }

    void RemoveLayerManager(iTVPLayerManager *manager) override {
        if(auto *concreteManager = dynamic_cast<tTVPLayerManager *>(manager))
            concreteManager->SetDrawBufferCacheEnabled(false);
        tTVPDrawDevice::RemoveLayerManager(manager);
        if(PrimaryLayerManagerIndex >= Managers.size())
            PrimaryLayerManagerIndex = Managers.empty() ? 0 : Managers.size() - 1;
    }

    void NotifyLayerResize(iTVPLayerManager *manager) override {
        if(Window)
            tTVPDrawDevice::NotifyLayerResize(manager);
    }

    void NotifyLayerImageChange(iTVPLayerManager *) override {
        if(Window)
            Window->RequestUpdate();
    }

    void Show() override {
        if(!Window)
            return;
        auto *form = Window->getForm();
        if(!form)
            return;
        auto *frame = compose();
        if(frame)
            form->UpdateDrawBuffer(frame->GetTexture());
    }

    void StartBitmapCompletion(iTVPLayerManager *) override {}
    void NotifyBitmapCompleted(iTVPLayerManager *, tjs_int, tjs_int,
                               tTVPBaseTexture *, const tTVPRect &,
                               tTVPLayerType, tjs_int) override {}
    void EndBitmapCompletion(iTVPLayerManager *) override {}

private:
    void updateLayers(tjs_real elapsed);
    iTVPBaseBitmap *compose(tjs_int frontIndexLimit = 0);
    tjs_int width_ = 0;
    tjs_int height_ = 0;
    std::array<tjs_int, 4> screenRect_{};
    tjs_uint32 clearColor_ = 0;
    tjs_int stretchType_ = 2;
    tjs_real bicubicParam_ = -0.5;
    tjs_int forceRenderTexture_ = 0;
    tjs_real transState_ = 1.0;
    bool transitionActive_ = false;
    std::unique_ptr<tTVPBaseTexture> transitionSource_;
    // Set only for universal transitions: 8bpp rule, as the core loads it.
    struct TextureRelease {
        void operator()(iTVPTexture2D *texture) const { texture->Release(); }
    };
    std::unique_ptr<iTVPTexture2D, TextureRelease> transitionRule_;
    tjs_int transitionVague_ = 0;
    std::unique_ptr<tTVPBaseTexture> transitionFrame_;
    std::unique_ptr<tTVPBaseTexture> composite_;
    std::vector<D3DLayer *> layers_;
};

class D3D : public DrawDeviceD3D {
public:
    using DrawDeviceD3D::DrawDeviceD3D;
};

DrawDeviceD3D *resolveDevice(iTJSDispatch2 *object) {
    if(!object)
        return nullptr;
    if(auto *device = ncbInstanceAdaptor<DrawDeviceD3D>::GetNativeInstance(object))
        return device;
    return ncbInstanceAdaptor<D3D>::GetNativeInstance(object);
}

class D3DImage {
public:
    explicit D3DImage(iTJSDispatch2 *deviceObject)
        : deviceObject_(deviceObject, deviceObject) {
        if(!resolveDevice(deviceObject))
            TVPThrowExceptionMessage(TJS_W("D3DImage requires a DrawDeviceD3D"));
    }

    void load(iTJSDispatch2 *layerObject) {
        tTJSNI_BaseLayer *layer = nullptr;
        if(!layerObject || TJS_FAILED(layerObject->NativeInstanceSupport(
                TJS_NIS_GETINSTANCE, tTJSNC_Layer::ClassID,
                reinterpret_cast<iTJSNativeInstance **>(&layer))) || !layer)
            TVPThrowExceptionMessage(TJS_W("D3DImage.load requires a Layer"));
        auto *source = layer->GetMainImage();
        if(!source || !source->GetWidth() || !source->GetHeight())
            TVPThrowExceptionMessage(TJS_W("D3DImage.load source has no image"));
        auto *sourceTexture = source->GetTexture();
        if(!sourceTexture)
            TVPThrowExceptionMessage(TJS_W("D3DImage.load source has no texture"));
        auto next = std::make_unique<tTVPBaseTexture>(source->GetWidth(),
                                                       source->GetHeight());
        auto *snapshot = TVPGetRenderManager()->CreateTexture2D(
            source->GetWidth(), source->GetHeight(), sourceTexture);
        if(!snapshot)
            TVPThrowExceptionMessage(TJS_W("D3DImage.load could not copy the image"));
        try {
            next->AssignTexture(snapshot);
        } catch(...) {
            snapshot->Release();
            throw;
        }
        snapshot->Release();
        image_ = std::move(next);
    }

    tjs_int getWidth() const { return image_ ? image_->GetWidth() : 0; }
    tjs_int getHeight() const { return image_ ? image_->GetHeight() : 0; }
    const tTVPBaseTexture *bitmap() const { return image_.get(); }
    DrawDeviceD3D *device() const {
        return resolveDevice(deviceObject_.AsObjectNoAddRef());
    }

private:
    tTJSVariant deviceObject_;
    std::unique_ptr<tTVPBaseTexture> image_;
};

class D3DLayer {
public:
    enum : tjs_int { DrawPlaneFront = 1, DrawPlaneBack = 2, DrawPlaneBoth = 3 };

    D3DLayer(iTJSDispatch2 *deviceObject, iTJSDispatch2 *scriptObject)
        : deviceObject_(deviceObject, deviceObject), scriptObject_(scriptObject) {
        device_ = resolveDevice(deviceObject);
        if(!device_)
            TVPThrowExceptionMessage(TJS_W("D3DLayer requires a DrawDeviceD3D"));
        device_->registerLayer(this);
    }
    ~D3DLayer() {
        if(device_)
            device_->unregisterLayer(this);
    }
    void detachDevice() { device_ = nullptr; }
    DrawDeviceD3D *device() const { return device_; }
    iTJSDispatch2 *scriptObject() const { return scriptObject_; }
    void addPicture(D3DPicture *picture) { pictures_.push_back(picture); }
    void removePicture(D3DPicture *picture) {
        pictures_.erase(std::remove(pictures_.begin(), pictures_.end(), picture),
                        pictures_.end());
    }
    void addEmote(DeviceEmotePlayer *player) { emotes_.push_back(player); }
    void removeEmote(DeviceEmotePlayer *player) {
        emotes_.erase(std::remove(emotes_.begin(), emotes_.end(), player),
                      emotes_.end());
    }

    tjs_int getDrawPlane() const { return drawPlane_; }
    void setDrawPlane(tjs_int value) {
        if(value < 0 || value > DrawPlaneBoth)
            TVPThrowExceptionMessage(TJS_W("D3DLayer drawPlane is invalid"));
        drawPlane_ = value;
    }
    tjs_int getFrontIndex() const { return frontIndex_; }
    void setFrontIndex(tjs_int value) { frontIndex_ = value; }
    tjs_int getBackIndex() const { return backIndex_; }
    void setBackIndex(tjs_int value) { backIndex_ = value; }
    const std::array<tjs_real, 16> &matrix() const { return matrix_; }

    void setMatrix(tjs_real a, tjs_real c, tjs_real m2, tjs_real m3,
                   tjs_real b, tjs_real d, tjs_real m6, tjs_real m7,
                   tjs_real m8, tjs_real m9, tjs_real m10, tjs_real m11,
                   tjs_real tx, tjs_real ty, tjs_real m14, tjs_real m15) {
        const std::array<tjs_real, 16> next = {
            a, c, m2, m3, b, d, m6, m7,
            m8, m9, m10, m11, tx, ty, m14, m15
        };
        if(!std::all_of(next.begin(), next.end(),
                        [](tjs_real value) { return std::isfinite(value); }))
            TVPThrowExceptionMessage(TJS_W("D3DLayer matrix must be finite"));
        if(next[2] != 0 || next[3] != 0 || next[6] != 0 || next[7] != 0 ||
           next[8] != 0 || next[9] != 0 || next[10] != 1 || next[11] != 0 ||
           next[14] != 0 || next[15] != 1)
            TVPThrowExceptionMessage(TJS_W("D3DLayer matrix requires 2D affine form"));
        matrix_ = next;
    }

    void draw(tTVPBaseTexture &target) const;
    void update(tjs_real elapsed) {
        if(!scriptObject_)
            return;
        tTJSVariant method;
        if(TJS_FAILED(scriptObject_->PropGet(0, TJS_W("onUpdate"), nullptr,
                                             &method, scriptObject_)))
            return;
        tTJSVariant argument(elapsed);
        tTJSVariant *arguments[] = { &argument };
        if(TJS_FAILED(scriptObject_->FuncCall(0, TJS_W("onUpdate"), nullptr,
                                              nullptr, 1, arguments, scriptObject_)))
            TVPThrowExceptionMessage(TJS_W("D3DLayer.onUpdate failed"));
    }

private:
    tTJSVariant deviceObject_;
    iTJSDispatch2 *scriptObject_ = nullptr; // borrowed from the native instance owner
    DrawDeviceD3D *device_ = nullptr;
    tjs_int drawPlane_ = DrawPlaneBoth;
    tjs_int frontIndex_ = 0;
    tjs_int backIndex_ = 0;
    std::array<tjs_real, 16> matrix_ = {
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1
    };
    std::vector<D3DPicture *> pictures_;
    std::vector<DeviceEmotePlayer *> emotes_;
};

class D3DPicture {
public:
    D3DPicture(iTJSDispatch2 *layerObject, iTJSDispatch2 *imageObject)
        : layerObject_(layerObject, layerObject),
          imageObject_(imageObject, imageObject) {
        auto *layer = resolveLayer();
        auto *image = resolveImage();
        if(!layer || !image || !layer->device() ||
           layer->device() != image->device())
            TVPThrowExceptionMessage(TJS_W("D3DPicture requires D3DLayer and D3DImage"));
        layer->addPicture(this);
    }
    ~D3DPicture() {
        if(auto *layer = resolveLayer())
            layer->removePicture(this);
    }

    void assignImageRange(tjs_int sx, tjs_int sy, tjs_int sw, tjs_int sh,
                          tjs_int dx, tjs_int dy) {
        auto *image = resolveImage();
        if(!image || !image->bitmap() || sx < 0 || sy < 0 || sw <= 0 || sh <= 0 ||
           static_cast<tjs_int64>(sx) + sw > image->getWidth() ||
           static_cast<tjs_int64>(sy) + sh > image->getHeight())
            TVPThrowExceptionMessage(TJS_W("D3DPicture image range is invalid"));
        source_ = tTVPRect(sx, sy, sx + sw, sy + sh);
        destX_ = dx;
        destY_ = dy;
    }
    void setCoord(tjs_real x, tjs_real y) {
        if(!std::isfinite(x) || !std::isfinite(y))
            TVPThrowExceptionMessage(TJS_W("D3DPicture coordinates must be finite"));
        coordX_ = x;
        coordY_ = y;
    }
    tjs_int getBlendMode() const { return blendMode_; }
    void setBlendMode(tjs_int value) {
        if(value != ltOpaque && value != ltAlpha && value != ltAdditive &&
           value != ltMultiplicative)
            TVPThrowExceptionMessage(TJS_W("D3DPicture blendMode is unsupported"));
        blendMode_ = value;
    }
    tjs_int getOpacity() const { return opacity_; }
    void setOpacity(tjs_int value) {
        if(value < 0 || value > 255)
            TVPThrowExceptionMessage(TJS_W("D3DPicture opacity is out of range"));
        opacity_ = value;
    }
    // The device origin is the screen center, as for DeviceEmotePlayer.
    // Destination and setCoord offsets are local and go through the matrix:
    // screen = M * (local + dest + coord) + translation + screenCenter.
    void draw(tTVPBaseTexture &target, const std::array<tjs_real, 16> &matrix,
              tjs_int screenWidth, tjs_int screenHeight) const {
        auto *image = resolveImage();
        if(!image || !image->bitmap() || source_.get_width() <= 0 || opacity_ == 0)
            return;
        const tjs_real localX = destX_ + coordX_;
        const tjs_real localY = destY_ + coordY_;
        const t2DAffineMatrix transform = {
            matrix[0], matrix[1], matrix[4], matrix[5],
            matrix[0] * localX + matrix[4] * localY + matrix[12] + screenWidth / 2.0,
            matrix[1] * localX + matrix[5] * localY + matrix[13] + screenHeight / 2.0
        };
        if(!std::isfinite(transform.tx) || !std::isfinite(transform.ty))
            TVPThrowExceptionMessage(TJS_W("D3DPicture transformed coordinates are invalid"));
        const tjs_real width = source_.get_width();
        const tjs_real height = source_.get_height();
        for(const tjs_real x : { -0.5, width - 0.5 }) {
            for(const tjs_real y : { -0.5, height - 0.5 }) {
                const tjs_real tx = transform.a * x + transform.c * y + transform.tx;
                const tjs_real ty = transform.b * x + transform.d * y + transform.ty;
                constexpr tjs_real limit =
                    static_cast<tjs_real>(std::numeric_limits<tjs_int>::max() / 2);
                if(!std::isfinite(tx) || !std::isfinite(ty) ||
                   std::abs(tx) > limit || std::abs(ty) > limit)
                    TVPThrowExceptionMessage(TJS_W("D3DPicture transform is out of range"));
            }
        }
        const auto type = static_cast<tTVPLayerType>(blendMode_);
        if(transform.a == 1 && transform.b == 0 && transform.c == 0 &&
           transform.d == 1 && std::floor(transform.tx) == transform.tx &&
           std::floor(transform.ty) == transform.ty) {
            target.Blt(static_cast<tjs_int>(transform.tx),
                       static_cast<tjs_int>(transform.ty), image->bitmap(),
                       source_, type, opacity_, false);
        } else {
            target.AffineBlt(tTVPRect(0, 0, target.GetWidth(), target.GetHeight()),
                             image->bitmap(), source_, transform,
                             type == ltOpaque ? (opacity_ == 255 ? bmCopy : bmCopyOnAlpha) :
                             type == ltAdditive ? bmAdd :
                             type == ltMultiplicative ? bmMul : bmAlphaOnAlpha,
                             opacity_, nullptr, false);
        }
    }

private:
    D3DLayer *resolveLayer() const {
        return ncbInstanceAdaptor<D3DLayer>::GetNativeInstance(
            layerObject_.AsObjectNoAddRef());
    }
    D3DImage *resolveImage() const {
        return ncbInstanceAdaptor<D3DImage>::GetNativeInstance(
            imageObject_.AsObjectNoAddRef());
    }
    tTJSVariant layerObject_;
    tTJSVariant imageObject_;
    tTVPRect source_{0, 0, 0, 0};
    tjs_int destX_ = 0;
    tjs_int destY_ = 0;
    tjs_real coordX_ = 0;
    tjs_real coordY_ = 0;
    tjs_int blendMode_ = ltAlpha;
    tjs_int opacity_ = 255;
};

// The E-mote player drawdeviceD3D.dll registers as D3DEmotePlayer: built on
// a D3DLayer, loading by storage name and advancing by 60 fps frames. It
// reuses motionplayer's EmotePlayer and renders each frame into its own
// canvas with the model origin at the canvas center; the owning D3DLayer then
// places that canvas with its matrix around the screen center, the original's
// coordinate model (setCoord stays 0,0; stand positions are layer matrices).
class DeviceEmotePlayer : public motion::EmotePlayer {
public:
    static tjs_error factory(DeviceEmotePlayer **result, tjs_int numparams,
                             tTJSVariant **param, iTJSDispatch2 *) {
        if(numparams != 1 || !param[0])
            return TJS_E_BADPARAMCOUNT;
        *result = new DeviceEmotePlayer(*param[0]);
        return TJS_S_OK;
    }

    explicit DeviceEmotePlayer(const tTJSVariant &layerObject)
        : motion::EmotePlayer(motion::ResourceManager{}),
          layerObject_(layerObject) {
        auto *layer = resolveLayer();
        if(!layer || !layer->device())
            TVPThrowExceptionMessage(TJS_W("D3DEmotePlayer requires a D3DLayer"));
        setUseD3D(true);
        layer->addEmote(this);
    }
    ~DeviceEmotePlayer() {
        if(auto *layer = resolveLayer())
            layer->removeEmote(this);
    }

    // Unlike Motion.EmotePlayer, the original takes a storage name, throws
    // when it cannot be loaded, and selects the base pose itself: After's D3D
    // scripts never set chara/play/initPhysics, which its non-D3D
    // AffineSourceMotion.createPlayer does after Motion.EmotePlayer.load.
    void load(ttstr storage) {
        const tTJSVariant module = resources_.load(storage);
        if(module.Type() != tvtObject)
            TVPThrowExceptionMessage(TJS_W("D3DEmotePlayer cannot load %1"), storage);
        motion::EmotePlayer::load(module);
        tTJSVariant metadata, base, value;
        if(member(module, TJS_W("metadata"), metadata) &&
           member(metadata, TJS_W("base"), base)) {
            if(member(base, TJS_W("chara"), value))
                setChara(value);
            if(member(base, TJS_W("motion"), value))
                play(value, motion::PlayFlagForce);
        }
        initPhysics(metadata);
    }

    // clone(D3DLayer) returns a player drawn by that layer, sharing the model.
    tTJSVariant clone(tTJSVariant layerObject) {
        auto copy = std::make_unique<DeviceEmotePlayer>(layerObject);
        copyStateTo(*copy);
        iTJSDispatch2 *adaptor =
            ncbInstanceAdaptor<DeviceEmotePlayer>::CreateAdaptor(copy.get());
        if(!adaptor)
            TVPThrowExceptionMessage(TJS_W("D3DEmotePlayer.clone failed"));
        copy.release();
        tTJSVariant result(adaptor, adaptor);
        adaptor->Release();
        return result;
    }

    // Scripts pass elapsed 60 fps frames (ms * 0.06); EmotePlayer takes ms.
    void progressFrames(tjs_real frames) {
        motion::EmotePlayer::progress(frames * 1000.0 / 60.0);
    }

    void composite(tTVPBaseTexture &target, const std::array<tjs_real, 16> &matrix,
                   tjs_int screenWidth, tjs_int screenHeight) {
        if(!getVisible())
            return;
        // ponytail: fixed canvas of screen width x twice its height keeps a
        // model raised by the layer matrix intact; size it from the model's
        // bounds if close-ups need more resolution or wider poses clip.
        const tjs_int canvasWidth = screenWidth;
        const tjs_int canvasHeight = screenHeight * 2;
        if(canvas_.getWidth() != canvasWidth || canvas_.getHeight() != canvasHeight) {
            canvas_.setSize(canvasWidth, canvasHeight);
            tTJSVariant values[6] = { 1.0, 0.0, 0.0, 1.0, canvasWidth / 2.0,
                                      canvasHeight / 2.0 };
            tTJSVariant *params[6] = { &values[0], &values[1], &values[2],
                                       &values[3], &values[4], &values[5] };
            setDrawAffineTranslateMatrixCompat(nullptr, 6, params, this);
        }
        canvas_.setPremultipliedOutputAllowed(true);
        if(!getPlayer().renderToD3DAdaptor(&canvas_))
            return;
        // screen = M * (canvas - canvasCenter) + translation + screenCenter
        t2DAffineMatrix transform = { matrix[0], matrix[1], matrix[4], matrix[5], 0, 0 };
        const tjs_real cx = canvasWidth / 2.0, cy = canvasHeight / 2.0;
        transform.tx = matrix[12] + screenWidth / 2.0 - (transform.a * cx + transform.c * cy);
        transform.ty = matrix[13] + screenHeight / 2.0 - (transform.b * cx + transform.d * cy);
        const auto stretch = getSmoothing() ? stLinear : stNearest;
        if(auto *premultiplied = canvas_.premultipliedOutput()) {
            affineBltPremultipliedOnAlpha(target, premultiplied, canvasWidth,
                                          canvasHeight, transform, stretch);
            return;
        }
        auto *texture = canvas_.targetTexture();
        if(!texture)
            return;
        if(!canvasView_ || viewedTexture_ != texture) {
            auto view = std::make_unique<tTVPBaseTexture>(canvasWidth, canvasHeight);
            view->AssignTexture(texture);
            canvasView_ = std::move(view);
            viewedTexture_ = texture;
        }
        target.AffineBlt(tTVPRect(0, 0, target.GetWidth(), target.GetHeight()),
                         canvasView_.get(), tTVPRect(0, 0, canvasWidth, canvasHeight),
                         transform, bmAlphaOnAlpha, 255, nullptr, false, stretch);
    }

private:
    static bool member(const tTJSVariant &object, const tjs_char *name,
                       tTJSVariant &value) {
        value.Clear();
        auto *dispatch = object.Type() == tvtObject ? object.AsObjectNoAddRef() : nullptr;
        return dispatch &&
            TJS_SUCCEEDED(dispatch->PropGet(0, name, nullptr, &value, dispatch)) &&
            value.Type() != tvtVoid;
    }

    D3DLayer *resolveLayer() const {
        return layerObject_.Type() == tvtObject
            ? ncbInstanceAdaptor<D3DLayer>::GetNativeInstance(
                  layerObject_.AsObjectNoAddRef())
            : nullptr;
    }

    tTJSVariant layerObject_;
    motion::ResourceManager resources_;
    motion::D3DAdaptor canvas_;
    std::unique_ptr<tTVPBaseTexture> canvasView_;
    iTVPTexture2D *viewedTexture_ = nullptr; // owned by canvas_
};

tTJSVariant DrawDeviceD3D::getChildren() const {
    iTJSDispatch2 *array = TJSCreateArrayObject();
    tjs_int index = 0;
    for(auto *layer : layers_) {
        auto *object = layer->scriptObject();
        if(!object)
            continue;
        tTJSVariant value(object, object);
        array->PropSetByNum(TJS_MEMBERENSURE, index++, &value, array);
    }
    tTJSVariant result(array, array);
    array->Release();
    return result;
}

void D3DLayer::draw(tTVPBaseTexture &target) const {
    if(!device_)
        return;
    for(auto *picture : pictures_)
        picture->draw(target, matrix_, device_->getWidth(), device_->getHeight());
    for(auto *emote : emotes_)
        emote->composite(target, matrix_, device_->getWidth(), device_->getHeight());
}

DrawDeviceD3D::~DrawDeviceD3D() {
    for(auto *layer : layers_)
        layer->detachDevice();
}

void DrawDeviceD3D::updateLayers(tjs_real elapsed) {
    const auto snapshot = layers_;
    for(auto *layer : snapshot) {
        if(std::find(layers_.begin(), layers_.end(), layer) != layers_.end())
            layer->update(elapsed);
    }
}

iTVPBaseBitmap *DrawDeviceD3D::compose(tjs_int frontIndexLimit) {
    if(!composite_ || composite_->GetWidth() != static_cast<tjs_uint>(width_) ||
       composite_->GetHeight() != static_cast<tjs_uint>(height_))
        composite_ = std::make_unique<tTVPBaseTexture>(width_, height_);
    composite_->Fill(tTVPRect(0, 0, width_, height_), 0);

    struct DrawEntry {
        D3DLayer *layer = nullptr;
        std::shared_ptr<iTVPLayerManager> manager;
        tjs_int plane = D3DLayer::DrawPlaneBoth;
        tjs_int frontIndex = 0;
        tjs_int backIndex = 0;
    };
    std::vector<DrawEntry> entries;
    for(auto *layer : layers_)
        entries.push_back({layer, {}, layer->getDrawPlane(),
                           layer->getFrontIndex(), layer->getBackIndex()});

    // Retain managers while script getters and completion callbacks run.
    const auto managers = [&] {
        std::vector<std::shared_ptr<iTVPLayerManager>> result;
        for(auto *manager : Managers) {
            manager->AddRef();
            result.emplace_back(manager, [](iTVPLayerManager *value) {
                value->Release();
            });
        }
        return result;
    }();
    for(const auto &manager : managers) {
        auto *primary = manager->GetPrimaryLayer();
        auto *owner = primary ? primary->GetOwnerNoAddRef() : nullptr;
        if(!owner)
            continue;
        const tTJSVariant retainedOwner(owner, owner);
        const auto property = [owner](const tjs_char *name, tjs_int fallback) {
            tTJSVariant value;
            if(TJS_FAILED(owner->PropGet(0, name, nullptr, &value, owner)) ||
               value.Type() == tvtVoid)
                return fallback;
            return static_cast<tjs_int>(value.AsInteger());
        };
        if(!property(TJS_W("drawvisible"), 1))
            continue;
        entries.push_back({nullptr, manager,
                           property(TJS_W("drawPlane"), D3DLayer::DrawPlaneBoth),
                           property(TJS_W("frontIndex"), 0),
                           property(TJS_W("backIndex"), 0)});
    }

    const auto draw = [&](const DrawEntry &entry) {
        if(entry.layer) {
            if(std::find(layers_.begin(), layers_.end(), entry.layer) != layers_.end())
                entry.layer->draw(*composite_);
            return;
        }
        auto *manager = entry.manager.get();
        if(std::find(Managers.begin(), Managers.end(), manager) == Managers.end() ||
           !manager->GetPrimaryLayer())
            return;
        auto *concreteManager = dynamic_cast<tTVPLayerManager *>(manager);
        if(concreteManager) {
            concreteManager->SetHoldAlpha(false);
            auto *buffer = concreteManager->GetOrCreateDrawBuffer();
            if(buffer && (!concreteManager->GetDrawBufferCacheEnabled() ||
                          TVPIsSoftwareRenderManager())) {
                const tTVPRect full(0, 0, buffer->GetWidth(), buffer->GetHeight());
                buffer->Fill(full, 0);
                manager->RequestInvalidation(full);
            }
        }
        manager->UpdateToDrawDevice();
        auto *buffer = manager->GetDrawBuffer();
        if(buffer)
            composite_->Blt(0, 0, buffer,
                            tTVPRect(0, 0, buffer->GetWidth(), buffer->GetHeight()),
                            ltAlpha, 255, false);
    };

    // Draw the selected plane. Both kinds of layer use the same index order.
    // The script exchanges page flags after stopTransition. Use Front at rest.
    const bool drawBackPlane = frontIndexLimit == 0 && transitionActive_;
    const tjs_int plane = drawBackPlane ? D3DLayer::DrawPlaneBack
                                       : D3DLayer::DrawPlaneFront;
    std::stable_sort(entries.begin(), entries.end(),
                     [drawBackPlane](const DrawEntry &left, const DrawEntry &right) {
                         return drawBackPlane ? left.backIndex < right.backIndex
                                              : left.frontIndex < right.frontIndex;
                     });
    for(const auto &entry : entries)
        if((entry.plane & plane) &&
           (frontIndexLimit == 0 || entry.frontIndex < frontIndexLimit))
            draw(entry);
    if(frontIndexLimit != 0 || !transitionActive_ || !transitionSource_)
        return composite_.get();
    if(transState_ <= 0)
        return composite_.get();
    const tjs_real remaining = std::clamp(transState_, 0.0, 1.0);
    if(!transitionRule_) {
        const auto opacity = static_cast<tjs_int>(std::lround(remaining * 255.0));
        if(opacity > 0)
            composite_->Blt(0, 0, transitionSource_.get(),
                            tTVPRect(0, 0, transitionSource_->GetWidth(),
                                     transitionSource_->GetHeight()), bmCopy,
                            opacity, false);
        return composite_.get();
    }

    // Same method and phase range (0 = old frame, 255 + vague = new frame)
    // as the core universal transition handler.
    static iTVPRenderMethod *method =
        TVPGetRenderManager()->GetRenderMethod("UnivTransBlend");
    static const int phaseId = method->EnumParameterID("phase");
    static const int vagueId = method->EnumParameterID("vague");
    if(!transitionFrame_ ||
       transitionFrame_->GetWidth() != static_cast<tjs_uint>(width_) ||
       transitionFrame_->GetHeight() != static_cast<tjs_uint>(height_))
        transitionFrame_ = std::make_unique<tTVPBaseTexture>(width_, height_);
    method->SetParameterInt(vagueId, transitionVague_);
    method->SetParameterInt(
        phaseId, static_cast<int>(std::lround((1.0 - remaining) *
                                              (255 + transitionVague_))));
    const tTVPRect full(0, 0, width_, height_);
    tRenderTexRectArray::Element sources[] = {
        tRenderTexRectArray::Element(transitionSource_->GetTexture(), full),
        tRenderTexRectArray::Element(composite_->GetTexture(), full),
        tRenderTexRectArray::Element(transitionRule_.get(), full)};
    TVPGetRenderManager()->OperateRect(
        method, transitionFrame_->GetTextureForRender(false, &full), nullptr,
        full, tRenderTexRectArray(sources));
    return transitionFrame_.get();
}

void DrawDeviceD3D::startTransition(iTJSDispatch2 *parameters) {
    if(!parameters)
        TVPThrowExceptionMessage(TJS_W("DrawDeviceD3D.startTransition requires parameters"));
    tTJSVariant method;
    if(TJS_FAILED(parameters->PropGet(0, TJS_W("method"), nullptr,
                                      &method, parameters)) ||
       method.Type() != tvtString)
        TVPThrowExceptionMessage(TJS_W("DrawDeviceD3D.startTransition requires a method"));
    const ttstr methodName = method.AsStringNoAddRef();
    std::unique_ptr<iTVPTexture2D, TextureRelease> rule;
    tjs_int vague = 0;
    if(methodName == TJS_W("universal")) {
        // KAGWindow.beginTransition passes the rule image already loaded
        // into a Layer, plus vague (it substitutes 64 when omitted).
        tTJSVariant ruleValue, vagueValue;
        tTJSNI_BaseLayer *ruleLayer = nullptr;
        if(TJS_FAILED(parameters->PropGet(0, TJS_W("rule"), nullptr,
                                          &ruleValue, parameters)) ||
           ruleValue.Type() != tvtObject || !ruleValue.AsObjectNoAddRef() ||
           TJS_FAILED(ruleValue.AsObjectNoAddRef()->NativeInstanceSupport(
               TJS_NIS_GETINSTANCE, tTJSNC_Layer::ClassID,
               reinterpret_cast<iTJSNativeInstance **>(&ruleLayer))) ||
           !ruleLayer)
            TVPThrowExceptionMessage(TJS_W("DrawDeviceD3D universal transition requires a rule Layer"));
        // KAG tag attributes arrive as strings ("128"); convert like the
        // core handler's GetAsNumber does.
        if(TJS_FAILED(parameters->PropGet(0, TJS_W("vague"), nullptr,
                                          &vagueValue, parameters)) ||
           vagueValue.Type() == tvtVoid || vagueValue.AsInteger() < 0 ||
           vagueValue.AsInteger() > 0xffff)
            TVPThrowExceptionMessage(TJS_W("DrawDeviceD3D universal transition requires vague in 0..65535"));
        vague = static_cast<tjs_int>(vagueValue.AsInteger());
        auto *image = ruleLayer->GetMainImage();
        if(!image || !image->Is32BPP() ||
           image->GetWidth() != static_cast<tjs_uint>(width_) ||
           image->GetHeight() != static_cast<tjs_uint>(height_))
            TVPThrowExceptionMessage(TJS_W("DrawDeviceD3D universal rule image must match the device size"));
        // A refcounted 8bpp bitmap, as the core rule loader produces: the
        // pixel-pointer CreateTexture2D overload only references its input.
        auto *gray = new tTVPBitmap(width_, height_, 8);
        try {
            for(tjs_int y = 0; y < height_; ++y)
                TVPBLConvert32BitTo8Bit(
                    static_cast<tjs_uint8 *>(gray->GetScanLine(y)),
                    static_cast<const tjs_uint32 *>(image->GetScanLine(y)),
                    width_);
            rule.reset(TVPGetRenderManager()->CreateTexture2D(gray));
        } catch(...) {
            gray->Release();
            throw;
        }
        gray->Release();
        if(!rule)
            TVPThrowExceptionMessage(TJS_W("DrawDeviceD3D.startTransition could not create the rule"));
    } else if(methodName != TJS_W("crossfade")) {
        TVPThrowExceptionMessage(TJS_W("DrawDeviceD3D supports only crossfade and universal transitions"));
    }
    auto *frame = compose();
    if(!frame || !frame->GetTexture())
        TVPThrowExceptionMessage(TJS_W("DrawDeviceD3D.startTransition has no frame"));
    auto next = std::make_unique<tTVPBaseTexture>(frame->GetWidth(),
                                                   frame->GetHeight());
    auto *snapshot = TVPGetRenderManager()->CreateTexture2D(
        frame->GetWidth(), frame->GetHeight(), frame->GetTexture());
    if(!snapshot)
        TVPThrowExceptionMessage(TJS_W("DrawDeviceD3D.startTransition could not copy frame"));
    try {
        next->AssignTexture(snapshot);
    } catch(...) {
        snapshot->Release();
        throw;
    }
    snapshot->Release();
    transitionSource_ = std::move(next);
    transitionRule_ = std::move(rule);
    transitionVague_ = vague;
    transitionActive_ = true;
    transState_ = 1.0;
}

void DrawDeviceD3D::capture(iTJSDispatch2 *layerObject, tjs_int index) {
    tTJSNI_BaseLayer *layer = nullptr;
    if(!layerObject || TJS_FAILED(layerObject->NativeInstanceSupport(
            TJS_NIS_GETINSTANCE, tTJSNC_Layer::ClassID,
            reinterpret_cast<iTJSNativeInstance **>(&layer))) || !layer)
        TVPThrowExceptionMessage(TJS_W("DrawDeviceD3D.capture requires a Layer"));
    Update();
    auto *frame = compose(index);
    if(!frame)
        TVPThrowExceptionMessage(TJS_W("DrawDeviceD3D.capture has no frame"));
    auto *frameTexture = frame->GetTexture();
    if(!frameTexture)
        TVPThrowExceptionMessage(TJS_W("DrawDeviceD3D.capture has no texture"));
    auto *texture = TVPGetRenderManager()->CreateTexture2D(
        frame->GetWidth(), frame->GetHeight(), frameTexture);
    if(!texture)
        TVPThrowExceptionMessage(TJS_W("DrawDeviceD3D.capture failed"));
    try {
        layer->AssignTexture(texture);
    } catch(...) {
        texture->Release();
        throw;
    }
    texture->Release();
}

} // namespace

static tjs_error D3DLayerFactory(D3DLayer **result, tjs_int numparams,
                                 tTJSVariant **params, iTJSDispatch2 *objthis) {
    if(numparams != 1 || !params[0] || params[0]->Type() != tvtObject)
        return TJS_E_BADPARAMCOUNT;
    *result = new D3DLayer(params[0]->AsObjectNoAddRef(), objthis);
    return TJS_S_OK;
}

static void RegisterDrawDeviceD3DDependencies() {
    // Nested E-mote motions create ncbind Player adaptors; their class must
    // exist even when the script only links the draw-device module.
    if(!ncbAutoRegister::LoadModule(TJS_W("motionplayer.dll")))
        TVPThrowExceptionMessage(TJS_W("DrawDeviceD3D requires motionplayer.dll"));
}

NCB_PRE_REGIST_CALLBACK(RegisterDrawDeviceD3DDependencies);

NCB_REGISTER_CLASS(DrawDeviceD3D) {
    NCB_CONSTRUCTOR((tjs_int, tjs_int));
    NCB_PROPERTY_RO(interface, getInterface);
    NCB_PROPERTY(width, getWidth, setWidth);
    NCB_PROPERTY(height, getHeight, setHeight);
    NCB_METHOD(setSize);
    NCB_PROPERTY(layerManagerIndex, getLayerManagerIndex, setLayerManagerIndex);
    NCB_PROPERTY_RO(primaryLayers, getPrimaryLayers);
    NCB_PROPERTY_RO(children, getChildren);
    NCB_PROPERTY(primaryWidth, getPrimaryWidth, ignorePrimarySizeWrite);
    NCB_PROPERTY(primaryHeight, getPrimaryHeight, ignorePrimarySizeWrite);
    NCB_PROPERTY_RO(screenLeft, getScreenLeft);
    NCB_PROPERTY_RO(screenTop, getScreenTop);
    NCB_PROPERTY_RO(screenWidth, getScreenWidth);
    NCB_PROPERTY_RO(screenHeight, getScreenHeight);
    NCB_PROPERTY(clearColor, getClearColor, setClearColor);
    NCB_PROPERTY(stretchType, getStretchType, setStretchType);
    NCB_PROPERTY(bicubicParam, getBicubicParam, setBicubicParam);
    NCB_PROPERTY(forceRenderTexture, getForceRenderTexture, setForceRenderTexture);
    NCB_METHOD(update);
    NCB_METHOD(checkEnable);
    NCB_METHOD(setScreenRect);
    NCB_METHOD(setPrimarySize);
    NCB_PROPERTY(transState, getTransState, setTransState);
    NCB_METHOD(startTransition);
    NCB_METHOD(stopTransition);
    NCB_METHOD(capture);
}

NCB_REGISTER_CLASS(D3D) {
    NCB_CONSTRUCTOR((tjs_int, tjs_int));
    NCB_PROPERTY_RO(interface, getInterface);
    NCB_PROPERTY(width, getWidth, setWidth);
    NCB_PROPERTY(height, getHeight, setHeight);
    NCB_METHOD(setSize);
    NCB_PROPERTY(layerManagerIndex, getLayerManagerIndex, setLayerManagerIndex);
    NCB_PROPERTY_RO(primaryLayers, getPrimaryLayers);
    NCB_PROPERTY_RO(children, getChildren);
    NCB_PROPERTY(primaryWidth, getPrimaryWidth, ignorePrimarySizeWrite);
    NCB_PROPERTY(primaryHeight, getPrimaryHeight, ignorePrimarySizeWrite);
    NCB_PROPERTY_RO(screenLeft, getScreenLeft);
    NCB_PROPERTY_RO(screenTop, getScreenTop);
    NCB_PROPERTY_RO(screenWidth, getScreenWidth);
    NCB_PROPERTY_RO(screenHeight, getScreenHeight);
    NCB_PROPERTY(clearColor, getClearColor, setClearColor);
    NCB_PROPERTY(stretchType, getStretchType, setStretchType);
    NCB_PROPERTY(bicubicParam, getBicubicParam, setBicubicParam);
    NCB_PROPERTY(forceRenderTexture, getForceRenderTexture, setForceRenderTexture);
    NCB_METHOD(update);
    NCB_METHOD(checkEnable);
    NCB_METHOD(setScreenRect);
    NCB_METHOD(setPrimarySize);
    NCB_PROPERTY(transState, getTransState, setTransState);
    NCB_METHOD(startTransition);
    NCB_METHOD(stopTransition);
    NCB_METHOD(capture);
}

NCB_REGISTER_CLASS(D3DLayer) {
    RawCallback(D3DLayerFactory);
    NCB_METHOD(setMatrix);
    NCB_PROPERTY(drawPlane, getDrawPlane, setDrawPlane);
    NCB_PROPERTY(frontIndex, getFrontIndex, setFrontIndex);
    NCB_PROPERTY(backIndex, getBackIndex, setBackIndex);
    Variant(TJS_W("DrawPlaneBoth"), static_cast<tjs_int>(D3DLayer::DrawPlaneBoth));
    Variant(TJS_W("DrawPlaneFront"), static_cast<tjs_int>(D3DLayer::DrawPlaneFront));
    Variant(TJS_W("DrawPlaneBack"), static_cast<tjs_int>(D3DLayer::DrawPlaneBack));
}

NCB_REGISTER_CLASS(D3DImage) {
    NCB_CONSTRUCTOR((iTJSDispatch2 *));
    NCB_METHOD(load);
    NCB_PROPERTY_RO(width, getWidth);
    NCB_PROPERTY_RO(height, getHeight);
}

NCB_REGISTER_CLASS_DIFFER(D3DEmotePlayer, DeviceEmotePlayer) {
    Factory(&Class::factory);
    EMOTE_PLAYER_SHARED_MEMBERS
    NCB_METHOD(load);
    NCB_METHOD(clone);
    Method(TJS_W("progress"), &Class::progressFrames);
}

NCB_REGISTER_CLASS(D3DPicture) {
    NCB_CONSTRUCTOR((iTJSDispatch2 *, iTJSDispatch2 *));
    NCB_METHOD(assignImageRange);
    NCB_METHOD(setCoord);
    NCB_PROPERTY(blendMode, getBlendMode, setBlendMode);
    NCB_PROPERTY(opacity, getOpacity, setOpacity);
}

extern "C" void TVPDrawDeviceD3DPluginAnchor() {}
