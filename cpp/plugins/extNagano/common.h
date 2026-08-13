#ifndef extNagano_commonH
#define extNagano_commonH

// Yoghourt adaptation of wamsoft/extNagano common helpers.
// Original Clip/Blend/Swap stay byte-compatible. Scanline access is routed
// through iTVPTexture2D because iTVPScanLineProvider::GetScanLine is compiled
// out of this engine.

#include "tjsCommHead.h"
#include "tjsArray.h"
#include "TransIntf.h"
#include "transhandler.h"
#include "RenderManager.h"
#include "LayerBitmapIntf.h"
#include "MsgIntf.h"
#include "DebugIntf.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>

#ifndef TJS_INTF_METHOD
#define TJS_INTF_METHOD
#endif

//---------------------------------------------------------------------------
// extNagano 共通ヘルパ (extrans/common.h 由来 + 追加分)
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
static inline bool Clip(tjs_int &l, tjs_int &r, tjs_int cl, tjs_int cr)
{
	// 線分 l <-> r (l<r) を cl <-> cr (cl<cr) でクリッピングし結果を l r に返す。
	// 残れば真、消滅すれば偽。
	if(l < cl) l = cl;
	if(r > cr) r = cr;
	if(l >= r) return false;
	return true;
}
//---------------------------------------------------------------------------
static inline tjs_uint32 Blend(tjs_uint32 a, tjs_uint32 b, tjs_int opa)
{
	// a と b を混合比 opa で混合 ( opa = 0..255, 0 = a, 255 = b )
	tjs_uint32 ret;
	tjs_uint32 tmp;
	tmp = a & 0x000000ff;  ret   = 0x000000ff & (tmp + (( (b & 0x000000ff) - tmp ) * opa >> 8));
	tmp = a & 0x0000ff00;  ret  |= 0x0000ff00 & (tmp + (( (b & 0x0000ff00) - tmp ) * opa >> 8));
	tmp = a & 0x00ff0000;  ret  |= 0x00ff0000 & (tmp + (( (b & 0x00ff0000) - tmp ) * opa >> 8));
	tmp = a >> 24;
	ret  |= (0x000000ff & (tmp + (( (b >> 24) - tmp ) * opa >> 8))) << 24;
	return ret;
}
//---------------------------------------------------------------------------
static inline void Swap_tjs_int(tjs_int &a, tjs_int &b)
{
	tjs_int tmp = a; a = b; b = tmp;
}
//---------------------------------------------------------------------------

namespace extNagano {

constexpr tjs_int kMaxDimension = 4096;
constexpr tjs_int64 kMaxPixels = 4096LL * 4096LL;
constexpr tjs_int kMaxMorphElements = 256 * 6; // official triangle cap is 0x100

inline void LogError(const char *msg) {
	TVPAddImportantLog(ttstr(TJS_W("[extNagano] ")) + ttstr(msg));
}

inline bool CheckImageSize(tjs_uint w, tjs_uint h) {
	if(w == 0 || h == 0)
		return false;
	if(w > static_cast<tjs_uint>(kMaxDimension) ||
	   h > static_cast<tjs_uint>(kMaxDimension))
		return false;
	if(static_cast<tjs_int64>(w) * static_cast<tjs_int64>(h) > kMaxPixels)
		return false;
	return true;
}

inline bool CheckImageSize(tjs_int w, tjs_int h) {
	if(w <= 0 || h <= 0)
		return false;
	return CheckImageSize(static_cast<tjs_uint>(w), static_cast<tjs_uint>(h));
}

inline tjs_uint64 ReadRequiredTime(iTVPSimpleOptionProvider *options,
                                   bool *ok) {
	if(ok)
		*ok = false;
	if(!options)
		return 2;
	tTJSVariant tmp;
	if(TJS_FAILED(options->GetValue(TJS_W("time"), &tmp)))
		return 2;
	if(tmp.Type() == tvtVoid)
		return 2;
	const tjs_int64 raw = static_cast<tjs_int64>(tmp);
	if(ok)
		*ok = true;
	if(raw < 2)
		return 2;
	return static_cast<tjs_uint64>(raw);
}

inline tjs_error GetSrcScanLine(iTVPScanLineProvider *provider, tjs_int line,
                                const void **scanline) {
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

inline tjs_error GetDstScanLine(iTVPScanLineProvider *provider, tjs_int line,
                                void **scanline) {
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

inline iTVPScanLineProvider *
SnapshotPixels(const tjs_uint8 *buffer, tjs_int width, tjs_int height,
               tjs_int pitch) {
	if(!buffer || !CheckImageSize(width, height))
		return nullptr;
	const tjs_int64 minAbsPitch = static_cast<tjs_int64>(width) * 4;
	if(pitch == 0 ||
	   (pitch > 0 ? static_cast<tjs_int64>(pitch)
	              : -static_cast<tjs_int64>(pitch)) < minAbsPitch) {
		LogError("rule image pitch is too small");
		return nullptr;
	}

	tTVPBaseTexture *bitmap = nullptr;
	try {
		bitmap = new tTVPBaseTexture(static_cast<tjs_uint>(width),
		                             static_cast<tjs_uint>(height));
		for(tjs_int y = 0; y < height; ++y) {
			const tjs_uint8 *src =
			    buffer + static_cast<tjs_int64>(y) * pitch;
			void *dst = bitmap->GetScanLineForWrite(static_cast<tjs_uint>(y));
			if(!dst) {
				delete bitmap;
				return nullptr;
			}
			memcpy(dst, src, static_cast<size_t>(width) * 4);
		}
		return new tTVPScanLineProviderForBaseBitmap(bitmap, true);
	} catch(...) {
		delete bitmap;
		LogError("failed to snapshot rule image");
		return nullptr;
	}
}

inline iTVPScanLineProvider *
SnapshotLayerObject(iTJSDispatch2 *obj) {
	if(!obj)
		return nullptr;
	if(TJS_FAILED(obj->IsInstanceOf(0, nullptr, nullptr, TJS_W("Layer"), obj))) {
		LogError("rule object is not a Layer");
		return nullptr;
	}

	tTJSVariant val;
	tjs_int width = 0, height = 0, pitch = 0;
	if(TJS_FAILED(obj->PropGet(0, TJS_W("imageWidth"), nullptr, &val, obj)))
		return nullptr;
	width = static_cast<tjs_int>(static_cast<tjs_int64>(val));
	if(TJS_FAILED(obj->PropGet(0, TJS_W("imageHeight"), nullptr, &val, obj)))
		return nullptr;
	height = static_cast<tjs_int>(static_cast<tjs_int64>(val));
	if(TJS_FAILED(obj->PropGet(0, TJS_W("mainImageBufferPitch"), nullptr, &val,
	                           obj)))
		return nullptr;
	pitch = static_cast<tjs_int>(static_cast<tjs_int64>(val));
	if(TJS_FAILED(obj->PropGet(0, TJS_W("mainImageBuffer"), nullptr, &val, obj)))
		return nullptr;
	const tjs_uint8 *buffer = reinterpret_cast<const tjs_uint8 *>(
	    static_cast<tjs_intptr_t>(static_cast<tjs_int64>(val)));
	if(!buffer) {
		LogError("rule layer has a null mainImageBuffer");
		return nullptr;
	}
	return SnapshotPixels(buffer, width, height, pitch);
}

inline iTVPScanLineProvider *
CreateRuleProvider(iTVPSimpleOptionProvider *options,
                   iTVPSimpleImageProvider *imagepro, tjs_uint fallbackH) {
	if(!options)
		return nullptr;

	tTJSVariant rule;
	if(TJS_FAILED(options->GetValue(TJS_W("rule"), &rule)))
		return nullptr;
	if(rule.Type() == tvtVoid)
		return nullptr;

	if(rule.Type() == tvtObject) {
		iTJSDispatch2 *obj = rule.AsObjectNoAddRef();
		return SnapshotLayerObject(obj);
	}

	const tjs_char *name = nullptr;
	if(TJS_FAILED(options->GetAsString(TJS_W("rule"), &name)) || !name)
		return nullptr;
	if(!imagepro)
		return nullptr;

	iTVPScanLineProvider *loaded = nullptr;
	if(TJS_FAILED(imagepro->LoadImage(name, 32, 0x02ffffff, 0, fallbackH,
	                                  &loaded)) ||
	   !loaded) {
		LogError("failed to load rule image");
		return nullptr;
	}
	tjs_int width = 0, height = 0;
	loaded->GetWidth(&width);
	loaded->GetHeight(&height);
	if(!CheckImageSize(width, height)) {
		loaded->Release();
		LogError("rule image exceeds the resource budget");
		return nullptr;
	}
	return loaded;
}

} // namespace extNagano

#define ExtNaganoGetSrcScanLine extNagano::GetSrcScanLine
#define ExtNaganoGetDstScanLine extNagano::GetDstScanLine

#endif
