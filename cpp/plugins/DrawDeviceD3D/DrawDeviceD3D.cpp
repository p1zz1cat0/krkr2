#define NCB_MODULE_NAME TJS_W("drawdeviceD3D.dll")

#include "ncbind.hpp"
#include "DrawDevice.h"
#include "LayerManager.h"
#include "MsgIntf.h"
#include "TVPWindow.h"
#include "WindowIntf.h"
#include "tjsArray.h"

#include <cstdint>

namespace {

// The window owns this native device through its TJS drawDevice object.
// LayerManager owns its DrawBuffer; this device selects and presents it.
class DrawDeviceD3D : public tTVPDrawDevice {
public:
    DrawDeviceD3D(tjs_int width, tjs_int height) { setSize(width, height); }

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
        Update();
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
        auto *manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
        auto *form = Window->getForm();
        auto *buffer = manager ? manager->GetDrawBuffer() : nullptr;
        if(form && buffer)
            form->UpdateDrawBuffer(buffer->GetTexture());
    }

    void StartBitmapCompletion(iTVPLayerManager *) override {}
    void NotifyBitmapCompleted(iTVPLayerManager *, tjs_int, tjs_int,
                               tTVPBaseTexture *, const tTVPRect &,
                               tTVPLayerType, tjs_int) override {}
    void EndBitmapCompletion(iTVPLayerManager *) override {}

private:
    tjs_int width_ = 0;
    tjs_int height_ = 0;
};

class D3D : public DrawDeviceD3D {
public:
    using DrawDeviceD3D::DrawDeviceD3D;
};

} // namespace

NCB_REGISTER_CLASS(DrawDeviceD3D) {
    NCB_CONSTRUCTOR((tjs_int, tjs_int));
    NCB_PROPERTY_RO(interface, getInterface);
    NCB_PROPERTY(width, getWidth, setWidth);
    NCB_PROPERTY(height, getHeight, setHeight);
    NCB_METHOD(setSize);
    NCB_PROPERTY(layerManagerIndex, getLayerManagerIndex, setLayerManagerIndex);
    NCB_PROPERTY_RO(primaryLayers, getPrimaryLayers);
    NCB_METHOD(update);
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
}

extern "C" void TVPDrawDeviceD3DPluginAnchor() {}
