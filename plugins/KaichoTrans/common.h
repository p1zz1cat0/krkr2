// KaichoTrans common helpers.
//
// The original plugin used the Windows SDK and tp_stub.h. This port uses
// the engine headers directly. iTVPScanLineProvider::GetScanLine and
// GetScanLineForWrite are compiled out in this fork, so scanlines come from
// the provider's texture.

#ifndef KAICHO_TRANS_COMMON_H
#define KAICHO_TRANS_COMMON_H

#include "tjsCommHead.h"
#include "typedefine.h"
#include "TransIntf.h"
#include "transhandler.h"
#include "LayerBitmapIntf.h"
#include "RenderManager.h"
#include "MsgIntf.h"
#include "DebugIntf.h"
#include "tvpgl.h"
#include "tjsUtils.h"
#include "common/PluginSafety.h"

#ifndef TJS_INTF_METHOD
#define TJS_INTF_METHOD
#endif

inline tjs_error KaichoGetSrcScanLine(iTVPScanLineProvider *provider,
                                      tjs_int line, const void **scanline) {
    if(!provider || !scanline)
        return TJS_E_FAIL;
    tjs_int height = 0;
    if(TJS_FAILED(provider->GetHeight(&height)))
        return TJS_E_FAIL;
    if(line < 0 || line >= height)
        return TJS_E_FAIL;
    iTVPTexture2D *texture = provider->GetTexture();
    if(!texture)
        return TJS_E_FAIL;
    const void *ptr = texture->GetScanLineForRead(static_cast<tjs_uint>(line));
    if(!ptr)
        return TJS_E_FAIL;
    *scanline = ptr;
    return TJS_S_OK;
}

inline tjs_error KaichoGetDstScanLine(iTVPScanLineProvider *provider,
                                      tjs_int line, void **scanline) {
    if(!provider || !scanline)
        return TJS_E_FAIL;
    tjs_int height = 0;
    if(TJS_FAILED(provider->GetHeight(&height)))
        return TJS_E_FAIL;
    if(line < 0 || line >= height)
        return TJS_E_FAIL;
    iTVPTexture2D *texture = provider->GetTextureForRender();
    if(!texture)
        return TJS_E_FAIL;
    void *ptr = texture->GetScanLineForWrite(static_cast<tjs_uint>(line));
    if(!ptr)
        return TJS_E_FAIL;
    *scanline = ptr;
    return TJS_S_OK;
}

inline tjs_int KaichoGetSrcPitch(iTVPScanLineProvider *provider) {
    iTVPTexture2D *texture = provider ? provider->GetTexture() : nullptr;
    return texture ? texture->GetPitch() : 0;
}

inline tjs_int KaichoGetDstPitch(iTVPScanLineProvider *provider) {
    iTVPTexture2D *texture =
        provider ? provider->GetTextureForRender() : nullptr;
    return texture ? texture->GetPitch() : 0;
}

#endif
