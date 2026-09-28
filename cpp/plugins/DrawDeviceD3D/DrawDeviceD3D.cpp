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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace {

class D3DLayer;
class D3DPicture;

// The window owns this native device through its TJS drawDevice object.
// LayerManager owns its DrawBuffer; this device selects and presents it.
class DrawDeviceD3D : public tTVPDrawDevice {
public:
    DrawDeviceD3D(tjs_int width, tjs_int height) { setSize(width, height); }
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

    void update(tjs_real elapsed) {
        if(elapsed < 0)
            TVPThrowExceptionMessage(TJS_W("DrawDeviceD3D elapsed time must be nonnegative"));
        updateLayers(elapsed);
        Update();
        if(Window)
            Window->RequestUpdate();
    }

    bool checkEnable(tTJSString name) const {
        // Report only modules owned by this device. The E-mote bridge is not
        // connected yet, even when the separate emoteplayer plugin is linked.
        (void)name;
        return false;
    }

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
        if(transitionActive_)
            showBackPlane_ = !showBackPlane_;
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
        manager->SetDesiredLayerType(ltOpaque);
    }

    void RemoveLayerManager(iTVPLayerManager *manager) override {
        tTVPDrawDevice::RemoveLayerManager(manager);
        if(PrimaryLayerManagerIndex >= Managers.size())
            PrimaryLayerManagerIndex = Managers.empty() ? 0 : Managers.size() - 1;
    }

    void NotifyLayerResize(iTVPLayerManager *manager) override {
        if(Window)
            tTVPDrawDevice::NotifyLayerResize(manager);
    }

    void NotifyLayerImageChange(iTVPLayerManager *manager) override {
        if(Window)
            tTVPDrawDevice::NotifyLayerImageChange(manager);
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
    iTVPBaseBitmap *compose();
    tjs_int width_ = 0;
    tjs_int height_ = 0;
    std::array<tjs_int, 4> screenRect_{};
    tjs_real transState_ = 1.0;
    bool transitionActive_ = false;
    bool showBackPlane_ = false;
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
    enum : tjs_int { DrawPlaneBoth = 0, DrawPlaneFront = 1, DrawPlaneBack = 2 };

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
    void addPicture(D3DPicture *picture) { pictures_.push_back(picture); }
    void removePicture(D3DPicture *picture) {
        pictures_.erase(std::remove(pictures_.begin(), pictures_.end(), picture),
                        pictures_.end());
    }

    tjs_int getDrawPlane() const { return drawPlane_; }
    void setDrawPlane(tjs_int value) {
        if(value < DrawPlaneBoth || value > DrawPlaneBack)
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
    void draw(tTVPBaseTexture &target, const std::array<tjs_real, 16> &matrix) const {
        auto *image = resolveImage();
        if(!image || !image->bitmap() || source_.get_width() <= 0 || opacity_ == 0)
            return;
        const t2DAffineMatrix transform = {
            matrix[0], matrix[1], matrix[4], matrix[5],
            matrix[12] + destX_ + coordX_, matrix[13] + destY_ + coordY_
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

void D3DLayer::draw(tTVPBaseTexture &target) const {
    for(auto *picture : pictures_)
        picture->draw(target, matrix_);
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

iTVPBaseBitmap *DrawDeviceD3D::compose() {
    auto *manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
    if(layers_.empty() && !transitionActive_)
        return manager ? manager->GetDrawBuffer() : nullptr;

    if(!composite_ || composite_->GetWidth() != static_cast<tjs_uint>(width_) ||
       composite_->GetHeight() != static_cast<tjs_uint>(height_))
        composite_ = std::make_unique<tTVPBaseTexture>(width_, height_);
    composite_->Fill(tTVPRect(0, 0, width_, height_), 0);

    std::vector<D3DLayer *> ordered = layers_;
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const D3DLayer *left, const D3DLayer *right) {
                         return left->getFrontIndex() < right->getFrontIndex();
                     });
    const bool drawBackPlane = transitionActive_ ? !showBackPlane_ : showBackPlane_;
    for(auto *layer : ordered) {
        if(layer->getDrawPlane() != D3DLayer::DrawPlaneBack)
            layer->draw(*composite_);
    }
    if(drawBackPlane)
        for(auto *layer : ordered)
            if(layer->getDrawPlane() == D3DLayer::DrawPlaneBack)
                layer->draw(*composite_);

    auto *concreteManager = dynamic_cast<tTVPLayerManager *>(manager);
    if(concreteManager) {
        concreteManager->SetHoldAlpha(false);
        auto *drawBuffer = concreteManager->GetOrCreateDrawBuffer();
        if(drawBuffer) {
            const tTVPRect full(0, 0, drawBuffer->GetWidth(),
                                drawBuffer->GetHeight());
            drawBuffer->Fill(full, 0);
            manager->RequestInvalidation(full);
            manager->UpdateToDrawDevice();
        }
    }
    auto *buffer = manager ? manager->GetDrawBuffer() : nullptr;
    if(buffer)
        composite_->Blt(0, 0, buffer,
                        tTVPRect(0, 0, buffer->GetWidth(), buffer->GetHeight()),
                        ltAlpha, 255, false);
    if(!transitionActive_ || !transitionSource_)
        return composite_.get();
    if(transState_ <= 0) {
        stopTransition();
        return composite_.get();
    }
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
    if(index != 0)
        TVPThrowExceptionMessage(TJS_W("DrawDeviceD3D capture index is unsupported"));
    tTJSNI_BaseLayer *layer = nullptr;
    if(!layerObject || TJS_FAILED(layerObject->NativeInstanceSupport(
            TJS_NIS_GETINSTANCE, tTJSNC_Layer::ClassID,
            reinterpret_cast<iTJSNativeInstance **>(&layer))) || !layer)
        TVPThrowExceptionMessage(TJS_W("DrawDeviceD3D.capture requires a Layer"));
    Update();
    auto *frame = compose();
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

NCB_REGISTER_CLASS(DrawDeviceD3D) {
    NCB_CONSTRUCTOR((tjs_int, tjs_int));
    NCB_PROPERTY_RO(interface, getInterface);
    NCB_PROPERTY(width, getWidth, setWidth);
    NCB_PROPERTY(height, getHeight, setHeight);
    NCB_METHOD(setSize);
    NCB_PROPERTY(layerManagerIndex, getLayerManagerIndex, setLayerManagerIndex);
    NCB_PROPERTY_RO(primaryLayers, getPrimaryLayers);
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

NCB_REGISTER_CLASS(D3DPicture) {
    NCB_CONSTRUCTOR((iTJSDispatch2 *, iTJSDispatch2 *));
    NCB_METHOD(assignImageRange);
    NCB_METHOD(setCoord);
    NCB_PROPERTY(blendMode, getBlendMode, setBlendMode);
    NCB_PROPERTY(opacity, getOpacity, setOpacity);
}

extern "C" void TVPDrawDeviceD3DPluginAnchor() {}
