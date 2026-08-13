//---------------------------------------------------------------------------
// extNagano : 追加トランジションプラグイン (吉里吉里Z 向け復元版)
//
// 元は吉里吉里2 用の extNagano.dll (作者: ヤマモト / Shun / chiyoclone.net /
// シノハラ、tp_stub等は W.Dee)。ソースが失われていたため、残存 DLL の Ghidra
// による解析 + extrans の構造をベースに再構築したもの。
//
// Yoghourt では V2Link を使わず、NCB_MODULE_NAME で extNagano.dll の
// モジュール桶へ載せ、Plugins.link("extNagano.dll") 後に 12 種の
// トランジションプロバイダを登録する。
//---------------------------------------------------------------------------
#include "ncbind.hpp"

#include "3duniversal.h"
#include "blurfade.h"
#include "scanline.h"
#include "zoomfade.h"
#include "rgbfade.h"
#include "spin.h"
#include "flutter.h"
#include "book.h"
#include "imagewipe.h"
#include "honeyturn.h"
#include "morphing.h"
#include "multiripple.h"

#define NCB_MODULE_NAME TJS_W("extNagano.dll")

namespace {

bool s_registered = false;

void InitPlugin_ExtNagano() {
	if(s_registered)
		return;
	Register3duniversalTransHandlerProvider();
	RegisterBlurFadeTransHandlerProvider();
	RegisterScanLineTransHandlerProvider();
	RegisterZoomFadeTransHandlerProvider();
	RegisterRGBFadeTransHandlerProvider();
	RegisterSpinFadeTransHandlerProvider();
	RegisterFlutterTransHandlerProvider();
	RegisterBookTransHandlerProvider();
	RegisterImageWipeTransHandlerProvider();
	RegisterHoneyTurnTransHandlerProvider();
	RegisterMorphingTransHandlerProvider();
	RegisterMultiRippleTransHandlerProvider();
	s_registered = true;
}

void UninitPlugin_ExtNagano() {
	if(!s_registered)
		return;
	Unregister3duniversalTransHandlerProvider();
	UnregisterBlurFadeTransHandlerProvider();
	UnregisterScanLineTransHandlerProvider();
	UnregisterZoomFadeTransHandlerProvider();
	UnregisterRGBFadeTransHandlerProvider();
	UnregisterSpinFadeTransHandlerProvider();
	UnregisterFlutterTransHandlerProvider();
	UnregisterBookTransHandlerProvider();
	UnregisterImageWipeTransHandlerProvider();
	UnregisterHoneyTurnTransHandlerProvider();
	UnregisterMorphingTransHandlerProvider();
	UnregisterMultiRippleTransHandlerProvider();
	s_registered = false;
}

} // namespace

NCB_REGISTER_CALLBACK(PreRegist, &InitPlugin_ExtNagano, &UninitPlugin_ExtNagano,
                      ExtNagano);

extern "C" void TVPExtNaganoPluginAnchor() {}
