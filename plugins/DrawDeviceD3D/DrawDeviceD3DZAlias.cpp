#define NCB_MODULE_NAME TJS_W("drawdeviceD3DZ.dll")

#include "ncbind.hpp"

static void RegisterDrawDeviceD3DZAlias() {
    ncbAutoRegister::LoadModule(TJS_W("drawdeviceD3D.dll"));
}

NCB_PRE_REGIST_CALLBACK(RegisterDrawDeviceD3DZAlias);

extern "C" void TVPDrawDeviceD3DZAliasPluginAnchor() {}
