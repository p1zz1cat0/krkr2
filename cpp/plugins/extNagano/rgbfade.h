#ifndef extNagano_rgbfadeH
#define extNagano_rgbfadeH

#include "tjsTypes.h"
#include "common/LayerPixel.h"

void RegisterRGBFadeTransHandlerProvider();
void UnregisterRGBFadeTransHandlerProvider();

namespace extNagano {
// FUN_10017110 の 1 画素分: src1 -> src2 を成分ごとの opa (0..255) で補間する。
// 入出力は Layer 画素 (common/LayerPixel.h)。元 DLL の計算は 0xAARRGGBB 前提
// なので境界で変換する (opaR が R に効くこと)。
inline tjs_uint32 RGBFadePixel(tjs_uint32 pixel1, tjs_uint32 pixel2,
                               tjs_int opaR, tjs_int opaG, tjs_int opaB,
                               tjs_int opaA)
{
	const tjs_uint32 s1 = layerPixel::toArgb(pixel1);
	const tjs_uint32 s2 = layerPixel::toArgb(pixel2);
	tjs_int s1b =  s1        & 0xff;
	tjs_int s1g = (s1 >> 8)  & 0xff;
	tjs_int s1r = (s1 >> 16) & 0xff;
	tjs_int s1a = (s1 >> 24) & 0xff;
	tjs_int s2b =  s2        & 0xff;
	tjs_int s2g = (s2 >> 8)  & 0xff;
	tjs_int s2r = (s2 >> 16) & 0xff;
	tjs_int s2a = (s2 >> 24) & 0xff;

	tjs_int db, dg, dr, da;

	if(s2a == 0)
	{
		// src2 が完全透明の画素: src1 のアルファをフェードアウトさせる。
		// ※RGB は元DLL通り「成分 * opa」(下位8bit)。プリマルチプライ透明時の
		//   縮退処理と思われ通常の不透明トランジションでは通らない。要確認。
		db = (s1b * opaB) & 0xff;
		dg = (s1g * opaG) & 0xff;
		dr = (s1r * opaR) & 0xff;
		da = s1a - ((s1a * opaA) >> 8);
	}
	else if(s1a == 0)
	{
		// src1 が完全透明の画素: src2 のアルファをフェードインさせる。
		// ※RGB は上と同様、元DLL通りの「成分 * opa」。要確認。
		db = (s1b * opaB) & 0xff;
		dg = (s1g * opaG) & 0xff;
		dr = (s1r * opaR) & 0xff;
		da = (s2a * opaA) >> 8;
	}
	else
	{
		// 通常経路: 成分ごとに別 opa で src1->src2 を線形補間
		db = s1b + (((s2b - s1b) * opaB) >> 8);
		dg = s1g + (((s2g - s1g) * opaG) >> 8);
		dr = s1r + (((s2r - s1r) * opaR) >> 8);
		da = s1a + (((s2a - s1a) * opaA) >> 8);
	}

	return layerPixel::fromArgb(((tjs_uint32)(da & 0xff) << 24) |
	        ((tjs_uint32)(dr & 0xff) << 16) |
	        ((tjs_uint32)(dg & 0xff) << 8)  |
	         (tjs_uint32)(db & 0xff));
}
} // namespace extNagano
#endif
