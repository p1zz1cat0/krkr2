#include "ncbind.hpp"
#include "MsgIntf.h"

#define NCB_MODULE_NAME TJS_W("getabout.dll")

extern "C" void TVPGetAboutPluginAnchor() {}
NCB_ATTACH_FUNCTION(getAboutString, System, TVPGetAboutString);
