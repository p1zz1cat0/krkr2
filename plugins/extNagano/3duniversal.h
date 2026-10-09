#ifndef extNagano_3duniversalH
#define extNagano_3duniversalH

#include "tjsTypes.h"
#include "common/LayerPixel.h"

void Register3duniversalTransHandlerProvider();
void Unregister3duniversalTransHandlerProvider();

namespace extNagano {
//---------------------------------------------------------------------------
// RGB -> HSB(ルール画素) 変換 (原 FUN_10001000)
//   返す ARGB のバイト配置を、RGB 時と同じ [0]=方向 / [1]=スピード /
//   [2]=開始時間 になるよう詰める。
//   原バイナリの詰め方は:
//     byte0 = Hue(0-255)         -> 方向インデクス
//     byte1 = Saturation*2(clamp) -> スピード
//     byte2 = 255 - V(明度)       -> 開始時間
//   ※ ドキュメントは「H=開始時間 / B(明度)=方向」と記すが、実バイナリは上記の
//     とおり Hue が方向・(255-明度)が開始時間に割り当たっている(役割が逆)。
//     ここではバイナリ(=実挙動)に合わせる。
//---------------------------------------------------------------------------
inline tjs_uint32 LoadRulePixelHSB(tjs_uint32 argb)
{
	tjs_int R = (argb >> 16) & 0xff;
	tjs_int G = (argb >> 8) & 0xff;
	tjs_int B = argb & 0xff;

	tjs_int mx = R; if(G > mx) mx = G; if(B > mx) mx = B;
	tjs_int mn = R; if(G < mn) mn = G; if(B < mn) mn = B;
	tjs_int chroma = mx - mn;

	// Hue (0-360 相当を整数演算で) → 0-255 へ正規化
	tjs_int hue = 0;
	if(chroma != 0)
	{
		tjs_int h6; // 0-6 セクタ
		if(G == mx)      h6 = (B - R) * 60 / chroma + 120;
		else if(B == mx) h6 = (R - G) * 60 / chroma + 240;
		else if(G < B)   h6 = (G - B) * 60 / chroma + 360;
		else             h6 = (G - B) * 60 / chroma;       // R==mx, G>=B
		// h6 は 0-360 度。0-255 へ
		if(h6 < 0) h6 += 360;
		hue = h6 * 255 / 360;
		if(hue > 255) hue = 255;
	}
	// Saturation
	tjs_int sat = (mx == 0) ? 0 : (chroma * 255 / mx);
	sat = sat * 2; if(sat > 255) sat = 255;
	// Brightness (原コードは 255 - V を開始時間に使う)
	tjs_int inv_v = 255 - mx;

	// byte0=方向(Hue) / byte1=スピード(Sat) / byte2=開始時間(255-V)
	return (tjs_uint32)hue | ((tjs_uint32)sat << 8) | ((tjs_uint32)inv_v << 16);
}
//---------------------------------------------------------------------------
// Layer 画素のルール -> [0]=方向 / [1]=スピード / [2]=開始時間 に詰めた値。
// 元 DLL は画素を 0xAARRGGBB として扱う (RGB ルール: B=方向 / G=スピード /
// R=開始時間) ので、先に ARGB へ戻してから詰める。
//---------------------------------------------------------------------------
inline tjs_uint32 DecodeRulePixel(tjs_uint32 pixel, bool hsb)
{
	const tjs_uint32 argb = layerPixel::toArgb(pixel);
	return hsb ? LoadRulePixelHSB(argb) : argb;
}
} // namespace extNagano
#endif
