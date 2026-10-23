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
#include "common/PluginSafety.h"

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
constexpr tjs_int kMaxMorphCoordinate = kMaxDimension * 4;

inline void LogError(const char *msg) {
	TVPAddImportantLog(ttstr(TJS_W("[extNagano] ")) + ttstr(msg));
}

inline void LogWarning(const ttstr &message) {
	TVPAddImportantLog(ttstr(TJS_W("[extNagano] ")) + message);
}

template <typename T>
inline T *AllocateArray(size_t count, pluginSafety::OperationBudget &budget) noexcept {
	const auto bytes = pluginSafety::checkedElementBytes(count, sizeof(T));
	if(!bytes)
		return nullptr;
	const auto allocation = pluginSafety::validateAllocationBudget(bytes.value, budget);
	if(!allocation)
		return nullptr;
	return new(std::nothrow) T[allocation.value.elementCount()];
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

inline bool ReadOptionNumber(iTVPSimpleOptionProvider *options,
                            const tjs_char *name, tjs_int64 minimum,
                            tjs_int64 maximum, tjs_int64 *out) noexcept {
	// Reads one numeric option. A missing value keeps the caller's default;
	// a real, an out-of-range value, or a non-number is converted and clamped
	// instead of rejecting the transition.
	if(!options || !name || !out)
		return false;
	if(minimum > maximum)
		std::swap(minimum, maximum);
	tTJSVariant value;
	if(TJS_FAILED(options->GetValue(name, &value)) || value.Type() == tvtVoid)
		return false;
	const auto type = value.Type();
	if(type != tvtInteger && type != tvtReal) {
		LogWarning(ttstr(TJS_W("option ")) + ttstr(name) +
		           TJS_W(" is not a number; the default is used"));
		return false;
	}
	const double raw = type == tvtInteger
	                       ? static_cast<double>(static_cast<tjs_int64>(value))
	                       : static_cast<double>(value);
	if(std::isnan(raw)) {
		LogWarning(ttstr(TJS_W("option ")) + ttstr(name) +
		           TJS_W(" is NaN; the default is used"));
		return false;
	}
	tjs_int64 converted = 0;
	if(type == tvtInteger)
		converted = static_cast<tjs_int64>(value);
	else {
		const double rounded = std::floor(raw + 0.5);
		if(rounded <= static_cast<double>(std::numeric_limits<tjs_int64>::min()))
			converted = std::numeric_limits<tjs_int64>::min();
		else if(rounded >= static_cast<double>(std::numeric_limits<tjs_int64>::max()))
			converted = std::numeric_limits<tjs_int64>::max();
		else
			converted = static_cast<tjs_int64>(rounded);
	}
	if(converted < minimum || converted > maximum) {
		const tjs_int64 clamped = converted < minimum ? minimum : maximum;
		LogWarning(ttstr(TJS_W("option ")) + ttstr(name) + TJS_W("=") +
		           ttstr(converted) + TJS_W(" is outside ") + ttstr(minimum) +
		           TJS_W("..") + ttstr(maximum) + TJS_W("; using ") +
		           ttstr(clamped));
		converted = clamped;
	}
	*out = converted;
	return true;
}

inline bool ReadOptionReal(iTVPSimpleOptionProvider *options,
                           const tjs_char *name, double minimum,
                           double maximum, double *out) noexcept {
	// Reads one real-valued option with the same leniency as
	// ReadOptionNumber.
	if(!options || !name || !out)
		return false;
	if(minimum > maximum)
		std::swap(minimum, maximum);
	tTJSVariant value;
	if(TJS_FAILED(options->GetValue(name, &value)) || value.Type() == tvtVoid)
		return false;
	const auto type = value.Type();
	if(type != tvtInteger && type != tvtReal) {
		LogWarning(ttstr(TJS_W("option ")) + ttstr(name) +
		           TJS_W(" is not a number; the default is used"));
		return false;
	}
	const double raw = type == tvtInteger
	                       ? static_cast<double>(static_cast<tjs_int64>(value))
	                       : static_cast<double>(value);
	if(!std::isfinite(raw)) {
		LogWarning(ttstr(TJS_W("option ")) + ttstr(name) +
		           TJS_W(" is not finite; the default is used"));
		return false;
	}
	double converted = raw < minimum ? minimum : (raw > maximum ? maximum : raw);
	if(converted != raw)
		LogWarning(ttstr(TJS_W("option ")) + ttstr(name) +
		           TJS_W(" is outside the supported range; clamped"));
	*out = converted;
	return true;
}

inline tjs_uint64 ReadRequiredTime(iTVPSimpleOptionProvider *options,
                                   bool *ok) {
	// time is mandatory. Its value is coerced and clamped: a real time value is
	// normal in games that scale the duration before starting a transition.
	if(ok)
		*ok = false;
	if(!options)
		return 2;
	tjs_int64 time = 0;
	if(!ReadOptionNumber(options, TJS_W("time"),
	                      static_cast<tjs_int64>(pluginSafety::kDefaultMinimumDurationMs),
	                      static_cast<tjs_int64>(pluginSafety::kDefaultMaximumDurationMs),
	                      &time))
		return 2;
	if(ok)
		*ok = true;
	return static_cast<tjs_uint64>(time);
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
	const auto layout = pluginSafety::validateLayerLayout(width, height, pitch);
	if(!layout) {
		LogError("rule image pitch is too small");
		return nullptr;
	}

	tTVPBaseTexture *bitmap = nullptr;
	try {
		bitmap = new tTVPBaseTexture(static_cast<tjs_uint>(width),
		                             static_cast<tjs_uint>(height));
		for(tjs_int y = 0; y < height; ++y) {
			const tjs_uint8 *src = buffer + static_cast<size_t>(y) *
			                                      static_cast<size_t>(pitch);
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
	const auto view = pluginSafety::LayerReadView::create(obj);
	if(!view) {
		LogError("rule object is not a Layer");
		return nullptr;
	}
	return SnapshotPixels(view.value.pixels(), view.value.width(),
	                      view.value.height(), view.value.pitchBytes());
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

// Option names a KAG game can pass to an extNagano transition.
inline const tjs_char *const kOptionNames[] = {
	TJS_W("time"),      TJS_W("type"),      TJS_W("rule"),
	TJS_W("dir"),       TJS_W("size"),      TJS_W("order"),
	TJS_W("twist"),     TJS_W("count"),     TJS_W("wavecount"),
	TJS_W("rwidth"),    TJS_W("maxdrift"),  TJS_W("roundness"),
	TJS_W("delaylast"), TJS_W("speed1"),    TJS_W("accel1"),
	TJS_W("bound1"),    TJS_W("speed2"),    TJS_W("accel2"),
	TJS_W("bound2"),    TJS_W("exponent"),  TJS_W("blur1"),
	TJS_W("blur1x"),    TJS_W("blur1y"),    TJS_W("blur2"),
	TJS_W("blur2x"),    TJS_W("blur2y"),    TJS_W("prerender"),
	TJS_W("zoom1"),     TJS_W("zoom2"),     TJS_W("delayR"),
	TJS_W("delayG"),    TJS_W("delayB"),    TJS_W("delayA"),
	TJS_W("type1"),     TJS_W("type2"),     TJS_W("back"),
	TJS_W("alpha"),     TJS_W("slip"),      TJS_W("before"),
	TJS_W("after"),     TJS_W("speed"),     TJS_W("accel"),
	TJS_W("twistaccel"), TJS_W("centerx"),   TJS_W("centery"),
};

inline ttstr DescribeOptions(iTVPSimpleOptionProvider *options) {
	if(!options)
		return TJS_W("(no options)");
	ttstr text;
	for(const tjs_char *name : kOptionNames) {
		tTJSVariant value;
		if(TJS_FAILED(options->GetValue(name, &value)) ||
		   value.Type() == tvtVoid)
			continue;
		if(!text.IsEmpty())
			text += TJS_W(" ");
		text += name;
		text += TJS_W("=");
		if(value.Type() == tvtObject || value.Type() == tvtOctet)
			text += TJS_W("<object>");
		else
			text += ttstr(value);
	}
	return text.IsEmpty() ? ttstr(TJS_W("(no known options)")) : text;
}

inline void TraceTransitionStart(const char *name,
                                iTVPSimpleOptionProvider *options) {
	// One line per distinct transition name keeps a long game session readable
	// while showing exactly what the script asked for.
	constexpr size_t kMaxSeen = 32;
	static const char *seen[kMaxSeen] = {};
	static size_t seenCount = 0;
	for(size_t i = 0; i < seenCount; ++i) {
		if(seen[i] && strcmp(seen[i], name) == 0)
			return;
	}
	if(seenCount < kMaxSeen)
		seen[seenCount++] = name;
	TVPAddLog(ttstr(TJS_W("[extNagano] transition ")) + ttstr(name) +
	          TJS_W(" options: ") + DescribeOptions(options));
}

inline void TraceTransitionReject(const char *name, const char *reason) {
	LogWarning(ttstr(TJS_W("transition ")) + ttstr(name) +
	           TJS_W(" was not started: ") + ttstr(reason));
}

} // namespace extNagano

#define ExtNaganoGetSrcScanLine extNagano::GetSrcScanLine
#define ExtNaganoGetDstScanLine extNagano::GetDstScanLine

#endif
