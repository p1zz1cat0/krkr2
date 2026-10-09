#ifndef extNagano_flutterH
#define extNagano_flutterH

#include "tjsTypes.h"
#include "common/LayerPixel.h"

void RegisterFlutterTransHandlerProvider();
void UnregisterFlutterTransHandlerProvider();

namespace extNagano {
// 裏面色: TJS 引数 back (0xAARRGGBB) の RGB を不透明で使う (元 back|0xff000000)。
// Layer 画素へ混ぜるので Layer 画素配置で返す。
inline tjs_uint32 FlutterBackColor(tjs_uint32 back)
{
	return layerPixel::fromArgb(back) | 0xff000000;
}
} // namespace extNagano
#endif
