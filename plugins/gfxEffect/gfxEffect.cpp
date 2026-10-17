// gfxEffect.dll
//
// Compatibility stub ported from AetherKrkr plugins/dummy_plugin_stubs.cpp
// (GPLv3, same as this fork). The original Windows plugin is closed source;
// only the class constructor and finalize method are available. Drawing
// methods do not exist yet, so a game that calls them sees a TJS member error.

#define NCB_MODULE_NAME TJS_W("gfxEffect.dll")

extern "C" void TVPGfxEffectPluginAnchor() {}

#include "ncbind.hpp"
#include "DebugIntf.h"

class gfxFire {
public:
    gfxFire() { TVPAddLog(TJS_W("gfxFire construct")); }
    ~gfxFire() = default;
    void finalize() { TVPAddLog(TJS_W("gfxFire finalize")); }
};

NCB_REGISTER_CLASS(gfxFire) {
    Constructor();
    NCB_METHOD(finalize);
}
