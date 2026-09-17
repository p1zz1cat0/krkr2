#include "ncbind.hpp"
#include "utils.hpp"
#include "common/PluginSafety.h"

#include <algorithm>
#include <vector>
#include <cmath>

#include <cstring>

#if !defined(_WIN32)
// generic(非Windows)ビルドでは windows.h が無いため DWORD が未定義。
// このファイルでは 32bit 符号なし整数の別名としてのみ使用しているので補う。
typedef tjs_uint32 DWORD;
#endif

//----------------------------------------------
// レイヤイメージ操作ユーティリティ

/**
 * レイヤのサイズとバッファを取得する
 *
 * [krkr2] 上游直接从实例 PropGet 读取尺寸/pitch/缓冲指针。这里经
 * PluginSafety 的 view 读取（经 Layer 类对象读原生属性，避免 KAGEX3 等
 * 子类覆写 imageWidth；校验 pitch 为正且覆盖整行、指针非空）。
 */
static bool
HasImage(iTJSDispatch2 *lay)
{
	if (!lay || TJS_FAILED(lay->IsInstanceOf(0, 0, 0, TJS_W("Layer"), lay))) return false;
	tTJSVariant val;
	return TJS_SUCCEEDED(lay->PropGet(0, TJS_W("hasImage"), 0, &val, lay)) && val.AsInteger() != 0;
}

bool
GetLayerSize(iTJSDispatch2 *lay, long &w, long &h, long *pitch)
{
	if (!HasImage(lay)) return false;
	const auto view = pluginSafety::LayerReadView::create(lay);
	if (!view) return false;
	w = view.value.width();
	h = view.value.height();
	if (pitch) *pitch = view.value.pitchBytes();
	return true;
}

// 読み込み用
bool
GetLayerBufferAndSize(iTJSDispatch2 *lay, long &w, long &h, BufRefT &ptr, long &pitch)
{
	if (!HasImage(lay)) return false;
	const auto view = pluginSafety::LayerReadView::create(lay);
	if (!view) return false;
	w = view.value.width();
	h = view.value.height();
	pitch = view.value.pitchBytes();
	ptr = view.value.pixels();
	return true;
}

// 書き込み用
bool
GetLayerBufferAndSize(iTJSDispatch2 *lay, long &w, long &h, WrtRefT &ptr, long &pitch)
{
	if (!HasImage(lay)) return false;
	const auto view = pluginSafety::LayerWriteView::create(lay);
	if (!view) return false;
	w = view.value.width();
	h = view.value.height();
	pitch = view.value.pitchBytes();
	ptr = view.value.pixels();
	return true;
}

// Province読み込み用
// [krkr2] province 为每像素 1 字节；pitch 必须为正且不小于宽度
bool
GetProvinceBufferAndSize(iTJSDispatch2 *lay, long &w, long &h, BufRefT &ptr, long &pitch)
{
	if (!GetLayerSize(lay, w, h)) return false;
	iTJSDispatch2 *layerClass = pluginSafety::detail::nativeLayerClass();
	const auto p = pluginSafety::detail::readIntegerProperty(layerClass, lay, TJS_W("provinceImageBufferPitch"));
	if (!p || p.value < w) return false;
	const auto buf = pluginSafety::detail::readPointerProperty(layerClass, lay, TJS_W("provinceImageBuffer"));
	if (!buf) return false;
	pitch = (long)p.value;
	ptr = reinterpret_cast<BufRefT>(buf.value);
	return true;
}

/**
 * 矩形領域の辞書を生成
 */
static void
MakeResult(tTJSVariant *result, long x, long y, long w, long h)
{
	ncbDictionaryAccessor dict;
	dict.SetValue(TJS_W("x"), x);
	dict.SetValue(TJS_W("y"), y);
	dict.SetValue(TJS_W("w"), w);
	dict.SetValue(TJS_W("h"), h);
	*result = dict;
}

/**
 * 不透明チェック関数
 */
static bool
CheckTransp(BufRefT p, long next, long count)
{
	for (; count > 0; count--, p+=next) if (p[3] != 0) return true;
	return false;
}

/**
 * レイヤイメージをクロップ（上下左右の余白透明部分を切り取る）したときのサイズを取得する
 *
 * Layer.getCropRect = function();
 * @return %[ x, y, w, h] 形式の辞書，またはvoid（全部透明のとき）
 */
static tjs_error TJS_INTF_METHOD
GetCropRect(tTJSVariant *result, tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *lay)
{
	// レイヤバッファの取得
	BufRefT p, r = 0;
	long w, h, nl, nc = 4;
	if (!GetLayerBufferAndSize(lay, w, h, r, nl))
		TVPThrowExceptionMessage(TJS_W("Invalid layer image."));

	// 結果領域
	long x1=0, y1=0, x2=w-1, y2=h-1;
	result->Clear();

	for (p=r;             x1 <  w; x1++,p+=nc) if (CheckTransp(p, nl,  h)) break; // 左から透明領域を調べる
	/*                                      */ if (x1 >= w) return TJS_S_OK;      // 全部透明なら void を返す
	for (p=r+x2*nc;       x2 >= 0; x2--,p-=nc) if (CheckTransp(p, nl,  h)) break; // 右から透明領域を調べる
	/*                                      */ long rw = x2 - x1 + 1;             // 左右に挟まれた残りの幅
	for (p=r+x1*nc;       y1 <  h; y1++,p+=nl) if (CheckTransp(p, nc, rw)) break; // 上から透明領域を調べる
	for (p=r+x1*nc+y2*nl; y2 >= 0; y2--,p-=nl) if (CheckTransp(p, nc, rw)) break; // 下から透明領域を調べる

	// 結果を辞書に返す
	MakeResult(result, x1, y1, rw, y2 - y1 + 1);

	return TJS_S_OK;
}

NCB_ATTACH_FUNCTION(getCropRect, Layer, GetCropRect);

/**
 * 不透明チェック関数
 */
static bool
CheckZerop(BufRefT p, long next, long count)
{
	for (; count > 0; count--, p+=next) if (p[3] != 0 || p[2] != 0 || p[1] != 0 || p[0] != 0) return true;
	return false;
}

/**
 * レイヤイメージをクロップ（上下左右の完全透明部分を切り取る）したときのサイズを取得する
 *
 * Layer.getCropRectZero = function();
 * @return %[ x, y, w, h] 形式の辞書，またはvoid（全部透明のとき）
 */
static tjs_error TJS_INTF_METHOD
GetCropRectZero(tTJSVariant *result, tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *lay)
{
	// レイヤバッファの取得
	BufRefT p, r = 0;
	long w, h, nl, nc = 4;
	if (!GetLayerBufferAndSize(lay, w, h, r, nl))
		TVPThrowExceptionMessage(TJS_W("Invalid layer image."));

	// 結果領域
	long x1=0, y1=0, x2=w-1, y2=h-1;
	result->Clear();

	for (p=r;             x1 <  w; x1++,p+=nc) if (CheckZerop(p, nl,  h)) break; // 左から透明領域を調べる
	/*                                      */ if (x1 >= w) return TJS_S_OK;      // 全部透明なら void を返す
	for (p=r+x2*nc;       x2 >= 0; x2--,p-=nc) if (CheckZerop(p, nl,  h)) break; // 右から透明領域を調べる
	/*                                      */ long rw = x2 - x1 + 1;             // 左右に挟まれた残りの幅
	for (p=r+x1*nc;       y1 <  h; y1++,p+=nl) if (CheckZerop(p, nc, rw)) break; // 上から透明領域を調べる
	for (p=r+x1*nc+y2*nl; y2 >= 0; y2--,p-=nl) if (CheckZerop(p, nc, rw)) break; // 下から透明領域を調べる

	// 結果を辞書に返す
	MakeResult(result, x1, y1, rw, y2 - y1 + 1);

	return TJS_S_OK;
}

NCB_ATTACH_FUNCTION(getCropRectZero, Layer, GetCropRectZero);

/**
 * 色比較関数
 */
#define IS_SAME_COLOR(A1,R1,G1,B1, A2,R2,G2,B2) \
	(((A1)==(A2)) && ((A1)==0 || ((R1)==(R2) && (G1)==(G2) && (B1)==(B2))))
//	!((p1[3] != p2[3]) || (p1[3] != 0 && (p1[0] != p2[0] || p1[1] != p2[1] || p1[2] != p2[2])))
//  ((A1 == A2) && (A1 == 0 || (R1==R2 && G1==G2 && B1==B2)))


static bool
CheckDiff(BufRefT p1, long p1n, BufRefT p2, long p2n, long count)
{
	for (; count > 0; count--, p1+=p1n, p2+=p2n)
		if (!IS_SAME_COLOR( p1[3],p1[2],p1[1],p1[0],  p2[3],p2[2],p2[1],p2[0] )) return true;
	return false;
}

/**
 * レイヤの差分領域を取得する
 * 
 * Layer.getDiffRegion = function(base);
 * @param base 差分元となるベース用の画像（インスタンス自身と同じ画像サイズであること）
 * @return %[ x, y, w, h ] 形式の辞書，またはvoid（完全に同じ画像のとき）
 */

static tjs_error TJS_INTF_METHOD
GetDiffRect(tTJSVariant *result, tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *lay)
{
	// 引数の数チェック
	if (numparams < 1) return TJS_E_BADPARAMCOUNT;

	iTJSDispatch2 *base = param[0]->AsObjectNoAddRef();

	// レイヤバッファの取得
	BufRefT fp, tp, fr = 0, tr = 0;
	long w, h, tnl, fw, fh, fnl, nc = 4;
	if (!GetLayerBufferAndSize(lay,   w,  h, tr, tnl) || 
		!GetLayerBufferAndSize(base, fw, fh, fr, fnl))
		TVPThrowExceptionMessage(TJS_W("Invalid layer image."));

	// レイヤのサイズは同じか
	if (w != fw || h != fh)
		TVPThrowExceptionMessage(TJS_W("Different layer size."));

	// 結果領域
	long x1=0, y1=0, x2=w-1, y2=h-1;
	result->Clear();

	for (fp=fr,             tp=tr;              x1 <  w; x1++,fp+=nc, tp+=nc ) if (CheckDiff(fp, fnl, tp, tnl,  h)) break; // 左から透明領域を調べる
	/*                                                                      */ if (x1 >= w) return TJS_S_OK;               // 全部透明なら void を返す
	for (fp=fr+x2*nc,       tp=tr+x2*nc;        x2 >= 0; x2--,fp-=nc, tp-=nc ) if (CheckDiff(fp, fnl, tp, tnl,  h)) break; // 右から透明領域を調べる
	/*                                                                      */ long rw = x2 - x1 + 1;                      // 左右に挟まれた残りの幅
	for (fp=fr+x1*nc,       tp=tr+x1*nc;        y1 <  h; y1++,fp+=fnl,tp+=tnl) if (CheckDiff(fp, nc,  tp, nc,  rw)) break; // 上から透明領域を調べる
	for (fp=fr+x1*nc+y2*fnl,tp=tr+x1*nc+y2*tnl; y2 >= 0; y2--,fp-=fnl,tp-=tnl) if (CheckDiff(fp, nc,  tp, nc,  rw)) break; // 下から透明領域を調べる

	// 結果を辞書に返す
	MakeResult(result, x1, y1, rw, y2 - y1 + 1);

	return TJS_S_OK;
}

NCB_ATTACH_FUNCTION(getDiffRect, Layer, GetDiffRect);

/**
 * レイヤのピクセル比較を行う
 * 
 * Layer.getDiffPixel = function(base, samecol, diffcol);
 * @param base 差分元となるベース用の画像（インスタンス自身と同じ画像サイズであること）
 * @param samecol 同じ場合に塗りつぶす色(0xAARRGGBB)（void・省略なら塗りつぶさない）
 * @param diffcol 違う場合に塗りつぶす色(0xAARRGGBB)（void・省略なら塗りつぶさない）
 * @return 違うピクセルのカウント
 */

static tjs_error TJS_INTF_METHOD
GetDiffPixel(tTJSVariant *result, tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *lay)
{
	DWORD scol = 0, dcol = 0;
	bool sfill = false, dfill = false;
	tTVInteger count = 0;

	// 引数の数チェック
	if (numparams < 1) return TJS_E_BADPARAMCOUNT;

	if (numparams >= 2 && param[1]->Type() != tvtVoid) {
		scol = RevRGB((DWORD)(param[1]->AsInteger())); // [krkr2] RGBA 内存序
		sfill = true;
	}

	if (numparams >= 3 && param[2]->Type() != tvtVoid) {
		dcol = RevRGB((DWORD)(param[2]->AsInteger())); // [krkr2] RGBA 内存序
		dfill = true;
	}

	iTJSDispatch2 *base = param[0]->AsObjectNoAddRef();

	// レイヤバッファの取得
	BufRefT fp, fr = 0;
	WrtRefT tp, tr = 0;
	long w, h, tnl, fw, fh, fnl, nc = 4;
	if (!GetLayerBufferAndSize(lay,   w,  h, tr, tnl) || 
		!GetLayerBufferAndSize(base, fw, fh, fr, fnl))
		TVPThrowExceptionMessage(TJS_W("Invalid layer image."));

	// レイヤのサイズは同じか
	if (w != fw || h != fh)
		TVPThrowExceptionMessage(TJS_W("Different layer size."));

	// 塗りつぶし
	for (long y = 0; (fp=fr, tp=tr, y < h); y++, fr+=fnl, tr+=tnl) {
		for (long x = 0; x < w; x++, fp+=nc, tp+=nc) {
			bool same = IS_SAME_COLOR(fp[3],fp[2],fp[1],fp[0], tp[3],tp[2],tp[1],tp[0]);
			if (      same &&     sfill) *(DWORD*)tp = scol;
			else if (!same) { if (dfill) *(DWORD*)tp = dcol; count++; }
		}
	}
	if (result) *result = count;

	return TJS_S_OK;
}

NCB_ATTACH_FUNCTION(getDiffPixel, Layer, GetDiffPixel);


// 色を加算
static inline void AddColor(DWORD &r, DWORD &g, DWORD &b, BufRefT p) {
	r += p[PIX_R], g += p[PIX_G], b += p[PIX_B]; // [krkr2]
}

/**
 * レイヤの淵の色を透明部分まで引き伸ばす（縮小時に偽色が出るのを防ぐ）
 * 
 * Layer.oozeColor = function(level, threshold=1);
 * @param level 処理を行う回数。大きいほど引き伸ばし領域が増える
 * @param threshold アルファの閾値(1～255)これより低いピクセルへ引き伸ばす
 * @param fillColor 処理領域以外の塗りつぶし色
 */
static tjs_error TJS_INTF_METHOD
OozeColor(tTJSVariant *result, tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *lay)
{
	// 引数の数チェック
	if (numparams < 1) return TJS_E_BADPARAMCOUNT;
	int level = (int)(param[0]->AsInteger());
	if (level <= 0) 
		TVPThrowExceptionMessage(TJS_W("Invalid level count."));
	unsigned char threshold = (unsigned char)(numparams > 1 ? param[1]->AsInteger() : 1);
	unsigned long fillColor = (unsigned long)(numparams > 2 ? param[2]->AsInteger() : 0);
	unsigned char fillR = (unsigned char)((fillColor >> 16) & 0xff);
	unsigned char fillG = (unsigned char)((fillColor >> 8) & 0xff);
	unsigned char fillB = (unsigned char)((fillColor) & 0xff);
	if (threshold < 1)
		threshold = 1;
	else if (threshold > 255)
		threshold = 255;

	// レイヤバッファの取得
	WrtRefT p, r = 0;
	long x, y, w, h, nl, nc = 4, ow, oh;
	if (!GetLayerBufferAndSize(lay, w, h, r, nl))
		TVPThrowExceptionMessage(TJS_W("Invalid layer image."));

	ow = w+2, oh = h+2; // oozed map のサイズ
	char *o, *otop, *oozed = new char[ow*oh];
	otop = oozed + ow + 1; // oozed map 左上
	memset(oozed, 0, ow*oh); // クリア
	try {
		// アルファマップを調べる
		for (y = 0; y < h; y++) {
			o = otop + y*ow;
			p = r    + y*nl;
			for (x = 0; x < w; x++, o++, p+=nc) {
				if (p[3] >= threshold) *o = -1;
				else {
					p[PIX_R] = fillR; // [krkr2]
					p[PIX_G] = fillG;
					p[PIX_B] = fillB; // 閾値以下の不透明部分の色を指定色でクリア
				}
			}
		}

		// 引き伸ばし処理
		for (int i = 0; i < level; i++) {
			bool L, R, U, D;
			for (y = 0; y < h; y++) {
				o = otop + y*ow;
				p = r    + y*nl;
				for (x = 0; x < w; x++, p+=nc, o++) {
					// 未処理領域をチェック
					if (!*o) {
						DWORD cr = 0, cg = 0, cb = 0;
						// 上下左右の領域をチェック
						U=o[-ow]<0, D=o[ow]<0, L=o[-1]<0, R=o[1]<0;
						if (U || D || L || R) {
							int cnt = 0;
							if (U) AddColor(cr, cg, cb, p-nl), cnt++;
							if (D) AddColor(cr, cg, cb, p+nl), cnt++;
							if (L) AddColor(cr, cg, cb, p-nc), cnt++;
							if (R) AddColor(cr, cg, cb, p+nc), cnt++;
							p[PIX_R] = (unsigned char)(cr / cnt); // [krkr2]
							p[PIX_G] = (unsigned char)(cg / cnt);
							p[PIX_B] = (unsigned char)(cb / cnt);
							*o = 1;
						}
					}
				}
			}
			// 処理済マップの値を再設定
			for (y = 0; y < h; y++) {
				o = otop + y*ow;
				for (x = 0; x < w; x++, o++) if (*o>0) *o=-1;
			}
		}
	} catch (...) {
		delete[] oozed;
		throw;
	}
	delete[] oozed;

	return TJS_S_OK;
}

NCB_ATTACH_FUNCTION(oozeColor, Layer, OozeColor);

/**
 * Layer.copyAlpha = function(src);
 * src の B値を α領域にコピーする
 */
static tjs_error TJS_INTF_METHOD
CopyBlueToAlpha(tTJSVariant *result, tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *lay)
{
	if (numparams < 1) { // [krkr2] 上游为 < 0，无参数时解引用 param[0]
		return TJS_E_BADPARAMCOUNT;
	}

	// 読み込みもと
	BufRefT sbuf = 0;
	long sw, sh, spitch;
	if (!GetLayerBufferAndSize(param[0]->AsObjectNoAddRef(), sw, sh, sbuf, spitch)) {
		TVPThrowExceptionMessage(TJS_W("src must be Layer."));
	}
	// 書き込み先
	WrtRefT dbuf = 0;
	long dw, dh, dpitch;
	if (!GetLayerBufferAndSize(lay, dw, dh, dbuf, dpitch)) {
		TVPThrowExceptionMessage(TJS_W("dest must be Layer."));
	}

	// 小さい領域分
	int w = (sw < dw ? sw : dw);
	int h = sh < dh ? sh : dh;
	// コピー
	for (int i=0;i<h;i++) {
		BufRefT p = sbuf + PIX_B; // B領域 [krkr2]
		WrtRefT q = dbuf+3;   // A領域
		for (int j=0;j<w;j++) {
			*q = *p;
			p += 4;
			q += 4;
		}
		sbuf += spitch;
		dbuf += dpitch;
	}
	return TJS_S_OK;
}

NCB_ATTACH_FUNCTION(copyBlueToAlpha, Layer, CopyBlueToAlpha);

/**
 * [krkr2] x,y,w,h 引数をソース範囲でクリッピングする。
 * 上游以 tjs_int 计算 left+width，极端参数会溢出；这里在 int64 中裁剪，
 * 结果落在 [0, sw]x[0, sh] 内（宽高可能 <= 0，由调用方判定）。
 */
static void
ClipRect(tTJSVariant **param, long sw, long sh, tjs_int &left, tjs_int &top, tjs_int &width, tjs_int &height)
{
	tjs_int64 l = param[0]->AsInteger(), t = param[1]->AsInteger();
	tjs_int64 r = l + std::max<tjs_int64>(param[2]->AsInteger(), 0);
	tjs_int64 b = t + std::max<tjs_int64>(param[3]->AsInteger(), 0);
	l = std::clamp<tjs_int64>(l, 0, sw); r = std::clamp<tjs_int64>(r, 0, sw);
	t = std::clamp<tjs_int64>(t, 0, sh); b = std::clamp<tjs_int64>(b, 0, sh);
	left = (tjs_int)l; top = (tjs_int)t;
	width = (tjs_int)(r - l); height = (tjs_int)(b - t);
}

/**
 * Layer.isBlank = function(x,y,w,h);
 * 指定領域がブランクデータかどうか確認する
 */
static tjs_error TJS_INTF_METHOD
isBlank(tTJSVariant *result, tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *objthis)
{
	if (numparams < 4) {
		return TJS_E_BADPARAMCOUNT;
	}

	// 読み込みもと
	BufRefT sbuf = 0;
	long sw, sh, spitch;
	if (!GetLayerBufferAndSize(objthis, sw, sh, sbuf, spitch)) {
		TVPThrowExceptionMessage(TJS_W("src must be Layer."));
	}

	tjs_int left, top, width, height;
	ClipRect(param, sw, sh, left, top, width, height);

	// 範囲チェック
	if (width >= 0 && height >= 0) {
		// 判定処理
		for (tjs_int y = top; y < top + height; y++) {
			BufRefT buffer = sbuf + left * 4 + spitch * y;
			for (tjs_int x = left; x < left + width; x++, buffer += 4) {
				// [krkr2] 上游只看首字节（Windows 上为 B 通道），保持该语义
				if (buffer[PIX_B]) {
					if (result) {
						*result = 0;
					}
					return TJS_S_OK;
				}
			}
		}
	}
	
	if (result) {
		*result = 1;
	}
	return TJS_S_OK;
}

NCB_ATTACH_FUNCTION(isBlank, Layer, isBlank);

/**
 * Layer.clearAlpha = function (threthold, fillColor=0)
 * αが指定より小さい部分を完全透明化する
 */
static tjs_error TJS_INTF_METHOD
clearAlpha(tTJSVariant *result, tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *lay)
{
	int threthold = numparams <= 0 ? 0 : (int)(tTVInteger)(*param[0]);
	// [krkr2] 上游 unsigned long 在 LP64 上为 8 字节，写入会越过像素；改为 32 位并换成 RGBA 内存序
	DWORD fillColor = RevRGB((DWORD)((numparams > 1 ? param[1]->AsInteger() : 0) & 0xffffff));
	
	// 書き込み先
	WrtRefT dbuf = 0;
	long w, h, pitch;
	if (!GetLayerBufferAndSize(lay, w, h, dbuf, pitch)) {
		TVPThrowExceptionMessage(TJS_W("dest must be Layer."));
	}

	// コピー
	for (int i=0;i<h;i++) {
		WrtRefT q = dbuf;   // A領域
		for (int j=0;j<w;j++) {
			if (q[3] <= threthold) {
				memcpy(q, &fillColor, sizeof(fillColor));
			}
			q += 4;
		}
		dbuf += pitch;
	}
	return TJS_S_OK;
}

NCB_ATTACH_FUNCTION(clearAlpha, Layer, clearAlpha);

/**
 * Layer.getAverageColor = function(x,y,w,h);
 * 指定領域の平均色を返す
 */
static tjs_error TJS_INTF_METHOD
getAverageColor(tTJSVariant *result, tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *objthis)
{
	if (numparams < 4) {
		return TJS_E_BADPARAMCOUNT;
	}

	// 読み込みもと
	BufRefT sbuf = 0;
	long sw, sh, spitch;
	if (!GetLayerBufferAndSize(objthis, sw, sh, sbuf, spitch)) {
		TVPThrowExceptionMessage(TJS_W("src must be Layer."));
	}

	tjs_int left, top, width, height;
	ClipRect(param, sw, sh, left, top, width, height);

	// 範囲チェック
	if (width <= 0 || height <= 0) 
		TVPThrowExceptionMessage(TJS_W("invalid layer range"));

	double a = 0;
	double r = 0;
	double g = 0;
	double b = 0;
	double size = width * height;
	
	// 値取得
	for (tjs_int y = top; y < top + height; y++) {
		BufRefT buffer = sbuf + left * 4 + spitch * y;
		for (tjs_int x = left; x < left + width; x++, buffer += 4) {
			// [krkr2] 上游按字节 0..3 累加到 a/r/g/b（Windows 上实为 B/G/R/A），
			// 按 BGRA 字节顺序读取以保持与上游一致的返回值
			a += buffer[PIX_B];
			r += buffer[PIX_G];
			g += buffer[PIX_R];
			b += buffer[PIX_A];
		}
	}
	a /= size;
	r /= size;
	g /= size;
	b /= size;
	DWORD color = (((DWORD)a & 0xff) << 24 |
								 ((DWORD)r & 0xff) << 16 |
								 ((DWORD)g & 0xff) << 8 |
								 ((DWORD)b & 0xff) << 0);
  if (result) {
	  *result = (tjs_int)color;
	}
	return TJS_S_OK;
}

NCB_ATTACH_FUNCTION(getAverageColor, Layer, getAverageColor);

/**
 * Layer.getFingerPrintValue = function(ignore_transparent=true);
 * 比較用のハッシュ値を返す（同値の場合においてもgetDiffPixelなどを使って厳密に比較すること）
 */
#define FNV_1A_BASIS64 (0xCBF29CE484222325uLL)
#define FNV_1A_PRIME64 (0x00000100000001B3uLL)
static inline tjs_uint64 fnv_1a_hash(tjs_uint8 n, tjs_uint64 hash) { return (hash ^ n) * FNV_1A_PRIME64; }
static tjs_error TJS_INTF_METHOD
GetFingerPrintValue(tTJSVariant *result, tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *lay)
{
	BufRefT src = 0;
	long w, h, nl, nc = 4;
	if (!GetLayerBufferAndSize(lay, w, h, src, nl))
		TVPThrowExceptionMessage(TJS_W("Invalid layer image."));

	// 完全透明カラーの比較有無
	const bool lazy = (numparams > 0 && param[0]->Type() != tvtVoid) ? param[0]->operator bool() : true;

	tjs_uint64 hash = FNV_1A_BASIS64;

	for (long y = 0; y < h; ++y, src += nl) {
		BufRefT p = src;
		if (lazy) {
			for (long x = 0; x < w; ++x,p+=nc) {
				if (p[3]) {
					hash = fnv_1a_hash(p[PIX_B], hash); // [krkr2] 按上游 BGRA 顺序
					hash = fnv_1a_hash(p[PIX_G], hash);
					hash = fnv_1a_hash(p[PIX_R], hash);
					hash = fnv_1a_hash(p[PIX_A], hash);
				} else {
					hash = fnv_1a_hash(0, hash);
					hash = fnv_1a_hash(0, hash);
					hash = fnv_1a_hash(0, hash);
					hash = fnv_1a_hash(0, hash);
				}
			}
		} else {
			for (long x = 0; x < w; ++x,p+=nc) {
				hash = fnv_1a_hash(p[PIX_B], hash); // [krkr2] 按上游 BGRA 顺序
				hash = fnv_1a_hash(p[PIX_G], hash);
				hash = fnv_1a_hash(p[PIX_R], hash);
				hash = fnv_1a_hash(p[PIX_A], hash);
			}
		}
	}
	// 最後に大きさを混ぜる
	for (int i = 24; i >= 0; i-=8) {
		hash = fnv_1a_hash((w >> i) & 255, hash);
		hash = fnv_1a_hash((h >> i) & 255, hash);
	}
	if (result) *result = (tTVInteger)hash;
	return TJS_S_OK;
}

NCB_ATTACH_FUNCTION(getFingerPrintValue, Layer, GetFingerPrintValue);


/**
 * Layer.getShrinkVectorOctet = function(w, h);
 * 縮小ベクトルを返す
 */
static inline DWORD calcBlockSum(BufRefT src, long const w, long const h, long const nc, long const nl) {
	DWORD s[4] = {0,0,0,0}, total = 0;
	for (long y = 0; y < h; ++y, src += nl) {
		BufRefT p = src;
		for (long x = 0; x < w; ++x,p+=nc,++total) {
			// [krkr2] 按上游 BGRA 顺序累加，输出 octet 字节序与上游一致
			if (p[PIX_A]) s[0]+=p[PIX_B], s[1]+=p[PIX_G], s[2]+=p[PIX_R], s[3]+=p[PIX_A];
		}
	}
	DWORD const bias = total>>1;
	return (((((s[0] + bias) / total) & 0xFF)      ) |
			((((s[1] + bias) / total) & 0xFF) <<  8) |
			((((s[2] + bias) / total) & 0xFF) << 16) |
			((((s[3] + bias) / total) & 0xFF) << 24));
}
static tjs_error TJS_INTF_METHOD
GetShrinkVectorOctet(tTJSVariant *result, tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *lay)
{
	const tjs_int svw = (numparams > 0 && param[0]->Type() != tvtVoid) ? (tjs_int)*param[0] : 16;
	const tjs_int svh = (numparams > 1 && param[1]->Type() != tvtVoid) ? (tjs_int)*param[1] : 16;
	if (svw <= 0 || svh <= 0) return TJS_E_INVALIDPARAM; // [krkr2] 上游 svh==0 时除零

	BufRefT src = 0;
	long w, h, nl, nc = 4;
	if (!GetLayerBufferAndSize(lay, w, h, src, nl))
		TVPThrowExceptionMessage(TJS_W("Invalid layer image."));

	const long step_w = (w + svw - 1)/ svw;
	const long step_h = (h + svh - 1)/ svh;
	const long next_block = nc * step_w;
	const long next_line  = nl * step_h;

	std::vector<DWORD> vector;
	for (long y = 0; y < h; y+=step_h, src += next_line) {
		BufRefT p = src;
		const long bh = (h - y) > step_h ? step_h : (h - y);
		for (long x = 0; x < w; x+=step_w,p+=next_block) {
			const long bw = (w - x) > step_w ? step_w : (w - x);
			vector.push_back(calcBlockSum(p, bw, bh, nc, nl));
		}
	}
	if (result) *result = tTJSVariant(reinterpret_cast<tjs_uint8*>(&vector.front()), (tjs_uint)(vector.size() * sizeof(DWORD)));
	return TJS_S_OK;
}
NCB_ATTACH_FUNCTION(getShrinkVectorOctet, Layer, GetShrinkVectorOctet);

/**
 * Math.octetDot = function(oct_a,oct_b);
 * octetの各要素を0-255のベクトルとみなして内積を取る
 */
template <typename T>
struct VectorSumWork {
	T csum, dsum, nsum1, nsum2, psum1, psum2;
	size_t diff;
	VectorSumWork() : csum((T)0), dsum((T)0), nsum1((T)0), nsum2((T)0), psum1((T)0), psum2((T)0), diff(0) {}
};
template <typename T>
static inline bool octetVectorSum(tTJSVariant const *v1, tTJSVariant const *v2, VectorSumWork<T> &work)
{
	if (!v1 || v1->Type() != tvtOctet) return false;

	tTJSVariantOctet *oct1 = v1->AsOctetNoAddRef();
	tTJSVariantOctet *oct2 = (v2 && v2->Type() == tvtOctet) ? v2->AsOctetNoAddRef() : 0;
	if (!oct1) return false;
	if (!oct2) {
		tjs_uint const sz = oct1->GetLength();
		const tjs_uint8 *a = oct1->GetData();
		if (!a) return false;
		for (tjs_uint n = 0; n < sz; ++n) {
			T const va = (T)a[n];
			work.dsum  += va * va;
			work.nsum1 += va;
			if (a[n] != 0) ++work.diff;
		}
		work.psum1 = work.dsum;
	} else {
		tjs_uint const sz1 = oct1->GetLength();
		tjs_uint const sz2 = oct2->GetLength();
		if (sz1 != sz2) return false;

		const tjs_uint8 *a = oct1->GetData();
		const tjs_uint8 *b = oct2->GetData();
		if (!a || !b) return false;
		for (tjs_uint n = 0; n < sz1; ++n) {
			T const va = (T)a[n];
			T const vb = (T)b[n];
			T const vd = (va > vb) ? (va - vb) : (vb - va);
			work.csum  += va * vb;
			work.dsum  += vd * vd;
			work.nsum1 += va;
			work.nsum2 += vb;
			work.psum1 += va * va;
			work.psum2 += vb * vb;
			if (a[n] != b[n]) ++work.diff;
		}
	}
	return true;
}
static tjs_error TJS_INTF_METHOD
MathOctetVectorSum(tTJSVariant *result, tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *obj)
{
	if (numparams < 2) return TJS_E_BADPARAMCOUNT;

	VectorSumWork<tTVInteger> work;
	if (!octetVectorSum(param[0], param[1], work)) return TJS_E_INVALIDPARAM;

	if (result) {
		tTJSVariant v;
		iTJSDispatch2 *arr = TJSCreateArrayObject();
		if (!arr) return TJS_E_FAIL;
		v = (tTVInteger)work.diff;
		/**/            arr->PropSetByNum(TJS_MEMBERENSURE, 6, &v, arr);
		v = work.csum;  arr->PropSetByNum(TJS_MEMBERENSURE, 0, &v, arr);
		v = work.dsum;  arr->PropSetByNum(TJS_MEMBERENSURE, 1, &v, arr);
		v = work.psum1; arr->PropSetByNum(TJS_MEMBERENSURE, 2, &v, arr);
		v = work.psum2; arr->PropSetByNum(TJS_MEMBERENSURE, 3, &v, arr);
		v = work.nsum1; arr->PropSetByNum(TJS_MEMBERENSURE, 4, &v, arr);
		v = work.nsum2; arr->PropSetByNum(TJS_MEMBERENSURE, 5, &v, arr);
		*result = tTJSVariant(arr, arr);
		arr->Release();
	}
	return TJS_S_OK;
}
NCB_ATTACH_FUNCTION(octetVectorSum, Math, MathOctetVectorSum);

// [krkr2] 静态链接保活锚点，由 Main.cpp 的 TVPLayerExSavePluginAnchor 引用
void LayerExSaveUtilsAnchor() {}
