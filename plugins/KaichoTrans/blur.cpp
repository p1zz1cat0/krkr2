//---------------------------------------------------------------------------
#include <math.h>
#include <new>
#include "common.h"
#include "blur.h"

/**
 * ログ出力用
 */
#if 0
#include <stdio.h>
static void log(const tjs_char *format, ...)
{
        va_list args;
        va_start(args, format);
        tjs_char msg[1024];
        _vsnwprintf_s(msg, 1024, format, args);
        TVPAddLog(msg);
        va_end(args);
}
#endif


//---------------------------------------------------------------------------
/*
	boxBlur トランジション
	2012/01/29	0.2.5.0	動的な画面でもblurできるオプションdynamicを追加
	2011/12/09	0.2.1.0	"bad alloc exception thrown"を回避…したつもり
*/
//---------------------------------------------------------------------------

// The original used MSVC x86 inline assembly. This port uses the scalar
// path, which is complete.
// #define USE_SSE2


class tTVPBlurTransHandler : public iTVPDivisibleTransHandler
{
	//	boxBlur トランジションハンドラクラスの実装

	tjs_int RefCount; // 参照カウンタ
	/*
	 * iTVPDivisibleTransHandler は 参照カウンタによる管理を行う
	 */

protected:
	bool          First;		// 一番最初の呼び出しかどうか
	bool          Dynamic;		// 動的な画像に対応するかどうか
	tjs_int64     StartTick;	// トランジションを開始した tick count
	tjs_int64     HalfTime;		// トランジションに要する時間 / 2
	tjs_int64     Time;			// トランジションに要する時間
	tTVPLayerType LayerType;	// レイヤタイプ
	tjs_int       Width;		// 処理する画像の幅
	tjs_int       Height;		// 処理する画像の高さ

	tjs_int64     CurTime;		// 現在の tick count
	tjs_int       BlendRatio;	// ブレンド比

	double        Accel;		// 進行加速度(def=1.0)
	double        CurRatio;		// 現在の進行比率(0〜1.0)

	typedef tjs_uint32 IPIXELTYPE;	// インテグラルイメージのドットタイプ
	#define IPIXELCOLORNUM 4	// インテグラルイメージのカラー数(ARGB)
	#define IPIXELSIZE (sizeof(IPIXELTYPE)*IPIXELCOLORNUM)

	BYTE*         Iimg1;		// src1用インテグラルイメージ
	tjs_int       MaxXblur1, MaxYblur1; // src1の最大ブラー幅
	tjs_int       Iimgwidth1;	// インテグラルイメージの横幅
	tjs_int       Iimgheight1;	// インテグラルイメージの縦幅
	tjs_int       Iimgpitch1;	// インテグラルイメージの縦1ドット間の差異バイト数
	tjs_int       CurXblur1, CurYblur1; // src1の現在のブラー幅

	BYTE*         Iimg2;		// src2用インテグラルイメージ
	tjs_int       MaxXblur2, MaxYblur2;// src2の最大ブラー幅
	tjs_int       Iimgwidth2;	// インテグラルイメージの横幅
	tjs_int       Iimgheight2;	// インテグラルイメージの縦幅
	tjs_int       Iimgpitch2;	// インテグラルイメージの縦1ドット間の差異バイト数
	tjs_int       CurXblur2, CurYblur2; // src1の現在のブラー幅

	tjs_uint32    *bdst1, *bdst2; // src1/src2から作った1ライン分のテンポラリ画像
BYTE* p;
	// 現在のRatio(0〜1.0)を求める
	inline double setCurrentRatio() //(tjs_int64 starttick=StartTick, tjs_int64 time=Time, tjs_int64 curtick=CurTime, double accel=Accel)
	{
		double ratio = (double)CurTime/Time;
		// ratio = 0〜1.0
		ratio = (ratio < 0.0) ? 0.0 : (ratio > 1.0) ? 1.0 : ratio;
		if(Accel > 1.0)				// 下弦(最初遅く徐々に早く)
			ratio = pow(ratio, Accel);
		else if(Accel < -1.0) {	// 上弦(最初早く徐々に遅く)
			ratio = 1.0 - ratio;
			ratio = pow(ratio, -Accel);
			ratio = 1.0 - ratio;
		}
		CurRatio = ratio;
		return ratio;
	}

	inline BYTE *getIimg1Addr(tjs_int x, tjs_int y) {
		return Iimg1 + ((MaxYblur1+1+1 + y)*Iimgpitch1 + (MaxXblur1+1 + x)*IPIXELSIZE);
	}
	inline BYTE *getIimg2Addr(tjs_int x, tjs_int y) {
		return Iimg2 + ((MaxYblur2+1+1 + y)*Iimgpitch2 + (MaxXblur2+1 + x)*IPIXELSIZE);
	}
	void addALineToIntegralImage32(
			IPIXELTYPE *dst, IPIXELTYPE *dstprev,
			tjs_uint32 *src, tjs_int srcwidth,
			tjs_int xblur);
	void drawALineFromIntegralImageToImage32(
			tjs_uint32 *dst, tjs_int dstwidth,
			IPIXELTYPE *src1, IPIXELTYPE *src2,
			tjs_int xblur, tjs_int sq);
	void buildIntegralImage32(
			IPIXELTYPE *dst, tjs_int dstpitch,
			iTVPScanLineProvider *srcdat, tjs_int width, tjs_int height,
			tjs_int xblur, tjs_int yblur);

	// メモリ確保のラッパ関数。TJSAlignedAlloc()がすぐbad_allocになるので
	void *local_alloc(size_t size, size_t align=16)
	{
		(void)align;
		void * p = TJSAlignedAlloc(static_cast<tjs_uint>(size), 4);
		if (p == NULL)
			throw(std::bad_alloc());
		return p;
	}
	// 同上のfree。
	void local_free(void* p)
	{
		TJSAlignedDealloc(p);
	}

public:
	// コンストラクタ
	tTVPBlurTransHandler(tjs_uint64 time, tTVPLayerType layertype,
		tjs_int width, tjs_int height,
		tjs_int xblur1, tjs_int yblur1, tjs_int xblur2, tjs_int yblur2,
		double accel, bool dynamic)
	{
		RefCount    = 1;

		First       = true;
		LayerType   = layertype;
		Width       = width;
		Height      = height;
		Time        = time;
		HalfTime    = time / 2;
		Accel       = accel;
		Dynamic     = dynamic;

		MaxXblur1   = xblur1;
		MaxYblur1   = yblur1;
		Iimgwidth1  = width+xblur1*2+1;
		Iimgpitch1  = Iimgwidth1*IPIXELSIZE;
		Iimgheight1 = height+yblur1*2+1+1;
		Iimg1       = (BYTE*)local_alloc(Iimgheight1*Iimgpitch1, 16);

		MaxXblur2   = xblur2;
		MaxYblur2   = yblur2;
		Iimgwidth2  = width+xblur2*2+1;
		Iimgpitch2  = Iimgwidth2*IPIXELSIZE;
		Iimgheight2 = height+yblur2*2+1+1;
		Iimg2       = (BYTE*)local_alloc(Iimgheight2*Iimgpitch2, 16);

		bdst1       = (tjs_uint32*)local_alloc(Width*sizeof(*bdst1), 16);
		bdst2       = (tjs_uint32*)local_alloc(Width*sizeof(*bdst2), 16);
	}

	// デストラクタ
	virtual ~tTVPBlurTransHandler()
	{
		local_free(Iimg1);
		local_free(Iimg2);
		local_free(bdst1);
		local_free(bdst2);
	}

	tjs_error TJS_INTF_METHOD AddRef()
	{
		// iTVPBaseTransHandler の AddRef
		// 参照カウンタをインクリメント
		RefCount ++;
		return TJS_S_OK;
	}

	tjs_error TJS_INTF_METHOD Release()
	{
		// iTVPBaseTransHandler の Release
		// 参照カウンタをデクリメントし、0 になるならば delete this
		if(RefCount == 1)
			delete this;
		else
			RefCount--;
		return TJS_S_OK;
	}


	tjs_error TJS_INTF_METHOD SetOption(
			/*in*/iTVPSimpleOptionProvider *options // option provider
		)
	{
		// iTVPBaseTransHandler の SetOption
		// とくにやることなし
		return TJS_S_OK;
	}

	tjs_error TJS_INTF_METHOD StartProcess(tjs_uint64 tick);

	tjs_error TJS_INTF_METHOD EndProcess();

	tjs_error TJS_INTF_METHOD Process(
			tTVPDivisibleData *data);

	tjs_error TJS_INTF_METHOD MakeFinalImage(
			iTVPScanLineProvider ** dest,
			iTVPScanLineProvider * src1,
			iTVPScanLineProvider * src2)
	{
		*dest = src2; // 常に最終画像は src2
		return TJS_S_OK;
	}
};


//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD tTVPBlurTransHandler::StartProcess(tjs_uint64 tick)
{
	// トランジションの画面更新一回ごとに呼ばれる

	// トランジションの画面更新一回につき、まず最初に StartProcess が呼ばれる
	// そのあと Process が複数回呼ばれる ( 領域を分割処理している場合 )
	// 最後に EndProcess が呼ばれる

	if (First)	// 最初の実行
		StartTick = tick;
	if (Dynamic)		// 動的画面更新が必要なら次もFirstにして
		First = true;	// Integral Imageを再作成する。超無駄。

	// 画像演算に必要な各パラメータを計算
	CurTime = ((tjs_int64)tick - StartTick);
	if (CurTime > Time) CurTime = Time;

	// BlendRatio
	BlendRatio = tjs_int(CurTime * 255 / Time);
	if(BlendRatio > 255) BlendRatio = 255;

	//CurXblur/CurYblur
	CurXblur1 = tjs_int(MaxXblur1*CurTime/Time);
	CurYblur1 = tjs_int(MaxYblur1*CurTime/Time);
	CurXblur2 = tjs_int(MaxXblur2*(Time-CurTime)/Time);
	CurYblur2 = tjs_int(MaxYblur2*(Time-CurTime)/Time);

	return TJS_S_TRUE;
}

//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD tTVPBlurTransHandler::EndProcess()
{
	// トランジションの画面更新一回分が終わるごとに呼ばれる

	if(BlendRatio == 255) return TJS_S_FALSE; // トランジション終了

	return TJS_S_TRUE;
}


/*
 * インテグラルイメージ(=dst)に src から計算して一ライン分追加する
 * dstにはメモリは既に確保されているものとする
 */
void tTVPBlurTransHandler::addALineToIntegralImage32(
		IPIXELTYPE *dst, IPIXELTYPE *dstprev, tjs_uint32 *src, tjs_int srcwidth, tjs_int xblur)
{
#ifndef USE_SSE2
	tjs_uint32 r_sum = 0, g_sum = 0, b_sum = 0;
	tjs_uint32 col = *src;
	for (int x = -xblur-1; x < 0; x++) {
		b_sum += (col>> 0)&0xff;
		g_sum += (col>> 8)&0xff;
		r_sum += (col>>16)&0xff;
		*dst++ = b_sum + *dstprev++;
		*dst++ = g_sum + *dstprev++;
		*dst++ = r_sum + *dstprev++;
		dst++, dstprev++;	// 空き分をスキップ
	}
	for (int x = 0; x < srcwidth-1; x++) {
		col = *src++;
		b_sum += (col>> 0)&0xff;
		g_sum += (col>> 8)&0xff;
		r_sum += (col>>16)&0xff;
		*dst++ = b_sum + *dstprev++;
		*dst++ = g_sum + *dstprev++;
		*dst++ = r_sum + *dstprev++;
		dst++, dstprev++;	// 空き分をスキップ
	}
	col = *src++;
	for (int x = srcwidth-1; x < srcwidth+xblur; x++) {
		b_sum += (col>> 0)&0xff;
		g_sum += (col>> 8)&0xff;
		r_sum += (col>>16)&0xff;
		*dst++ = b_sum + *dstprev++;
		*dst++ = g_sum + *dstprev++;
		*dst++ = r_sum + *dstprev++;
		dst++, dstprev++;	// 空き分をスキップ
	}
#else
	// インラインアセンブラはクラスメンバ変数にアクセスできない(すると何も言わずに変な値になる)
	// ので、ローカル変数にのみアクセスしていることに注意
	__asm {
		mov			esi,  src
		mov			edi,  dst
		pxor		xmm1, xmm1	// unpack用
		pxor		xmm2, xmm2	// sum用
		mov			ebx,  dstprev
		mov			ecx,  xblur
		inc			ecx
		movd		xmm0, [esi]
		punpcklbw	xmm0, xmm1	// byte -> word
		punpcklwd	xmm0, xmm1	// word -> tjs_uint32
	BUILDINTIMAGE_LOOP1:
		paddd		xmm2, xmm0
		movdqa		xmm3, [ebx]
		paddd		xmm3, xmm2
		movdqa		[edi],xmm3
		add			edi,  16
		add			ebx,  16
		loop		BUILDINTIMAGE_LOOP1
		mov			ecx,  srcwidth
		dec			ecx
	BUILDINTIMAGE_LOOP2:
		movd		xmm0, [esi]
		punpcklbw	xmm0, xmm1	// byte -> word
		punpcklwd	xmm0, xmm1	// word -> tjs_uint32
		paddd		xmm2, xmm0
		movdqa		xmm3, [ebx]
		paddd		xmm3, xmm2
		movdqa		[edi],xmm3
		add			esi,  4
		add			edi,  16
		add			ebx,  16
		loop		BUILDINTIMAGE_LOOP2
		mov			ecx,  xblur
		inc			ecx
		movd		xmm0, [esi]
		punpcklbw	xmm0, xmm1	// byte -> word
		punpcklwd	xmm0, xmm1	// word -> tjs_uint32
	BUILDINTIMAGE_LOOP3:
		paddd		xmm2, xmm0
		movdqa		xmm3, [ebx]
		paddd		xmm3, xmm2
		movdqa		[edi],xmm3
		add			edi,  16
		add			ebx,  16
		loop		BUILDINTIMAGE_LOOP3
	}
#endif
}

/*
 * インテグラルイメージ(=src)を元にdstに一ライン描画する
 */
void tTVPBlurTransHandler::drawALineFromIntegralImageToImage32(
		tjs_uint32 *dst, tjs_int dstwidth, IPIXELTYPE *src1, IPIXELTYPE *src2, tjs_int xblur, tjs_int sq)
{
#ifndef USE_SSE2
	int xblurwidth = (xblur*2+1)*IPIXELCOLORNUM;
	for (int x = 0; x < dstwidth; x++) {
		tjs_uint32 b = (*src1 - *(src1+xblurwidth) - *src2 + *(src2+xblurwidth))/sq;
		src1++, src2++;
		tjs_uint32 g = (*src1 - *(src1+xblurwidth) - *src2 + *(src2+xblurwidth))/sq;
		src1++, src2++;
		tjs_uint32 r = (*src1 - *(src1+xblurwidth) - *src2 + *(src2+xblurwidth))/sq;
		src1+=2, src2+=2;
		*dst++ = ((r&0xff)<<16) | ((g&0xff)<<8) | (b&0xff);
	}
#else
	int xblurwidthsize = (xblur*2+1)*IPIXELSIZE;
	// ここ↓で rsqf が 1/sq でないのは、計算時に(単精度小数点で)誤差が大き過ぎて
	// 画像が変になることがあるから。色を加算したものに桁が近い方が誤差が少ないため。
	float rsqf = (float)(1<<16)/sq;
	__asm {
		mov			esi,  src1
		mov			edx,  src2
		mov			edi,  dst
		mov			ebx,  xblurwidthsize
		mov			eax,  rsqf
		movd		xmm1, eax
		pshufd		xmm1, xmm1, 0x0
		mov			ecx,  dstwidth
		sar			ecx,  2
		// 最初に 4dot ごとに処理する。これでスピード二割増し。
	BOXBLUR_LOOP1:
		movdqa		xmm0, [esi]
		paddd		xmm0, [edx+ebx]
		psubd		xmm0, [esi+ebx]
		psubd		xmm0, [edx]			// xmm0 = |sum_a|sum_r|sum_g|sum_b}
		cvtdq2ps	xmm0, xmm0
		mulps		xmm0, xmm1			// xmm0 *= 65536/sq
		cvtps2dq	xmm0, xmm0
		psrld		xmm0, 16			// xmm0 = |a|r|g|b|(tjs_uint32)
		packssdw	xmm0, xmm0
		packuswb	xmm0, xmm0			// xmm0 = |a|r|g|b|(byte)
		movd		[edi],xmm0
		add			esi, 16
		add			edx, 16
		add			edi, 4

		movdqa		xmm0, [esi]
		paddd		xmm0, [edx+ebx]
		psubd		xmm0, [esi+ebx]
		psubd		xmm0, [edx]			// xmm0 = |sum_a|sum_r|sum_g|sum_b}
		cvtdq2ps	xmm0, xmm0
		mulps		xmm0, xmm1			// xmm0 *= 65536/sq
		cvtps2dq	xmm0, xmm0
		psrld		xmm0, 16			// xmm0 = |a|r|g|b|(tjs_uint32)
		packssdw	xmm0, xmm0
		packuswb	xmm0, xmm0			// xmm0 = |a|r|g|b|(byte)
		movd		[edi],xmm0
		add			esi, 16
		add			edx, 16
		add			edi, 4

		movdqa		xmm0, [esi]
		paddd		xmm0, [edx+ebx]
		psubd		xmm0, [esi+ebx]
		psubd		xmm0, [edx]			// xmm0 = |sum_a|sum_r|sum_g|sum_b}
		cvtdq2ps	xmm0, xmm0
		mulps		xmm0, xmm1			// xmm0 *= 65536/sq
		cvtps2dq	xmm0, xmm0
		psrld		xmm0, 16			// xmm0 = |a|r|g|b|(tjs_uint32)
		packssdw	xmm0, xmm0
		packuswb	xmm0, xmm0			// xmm0 = |a|r|g|b|(byte)
		movd		[edi],xmm0
		add			esi, 16
		add			edx, 16
		add			edi, 4

		movdqa		xmm0, [esi]
		paddd		xmm0, [edx+ebx]
		psubd		xmm0, [esi+ebx]
		psubd		xmm0, [edx]			// xmm0 = |sum_a|sum_r|sum_g|sum_b}
		cvtdq2ps	xmm0, xmm0
		mulps		xmm0, xmm1			// xmm0 *= 65536/sq
		cvtps2dq	xmm0, xmm0
		psrld		xmm0, 16			// xmm0 = |a|r|g|b|(tjs_uint32)
		packssdw	xmm0, xmm0
		packuswb	xmm0, xmm0			// xmm0 = |a|r|g|b|(byte)
		movd		[edi],xmm0
		add			esi, 16
		add			edx, 16
		add			edi, 4
		dec			ecx
		jnz			BOXBLUR_LOOP1

		// 残りドットを処理
		mov			ecx,  dstwidth
		and			ecx,  3
		jz			BOXBLUR_LOOP_END
	BOXBLUR_LOOP2:
		movdqa		xmm0, [esi]
		paddd		xmm0, [edx+ebx]
		psubd		xmm0, [esi+ebx]
		psubd		xmm0, [edx]			// xmm0 = |sum_a|sum_r|sum_g|sum_b}
		cvtdq2ps	xmm0, xmm0
		mulps		xmm0, xmm1			// xmm0 *= 65536/sq
		cvtps2dq	xmm0, xmm0
		psrld		xmm0, 16			// xmm0 = |a|r|g|b|(tjs_uint32)
		packssdw	xmm0, xmm0
		packuswb	xmm0, xmm0			// xmm0 = |a|r|g|b|(byte)
		movd		[edi],xmm0
		add			esi, 16
		add			edx, 16
		add			edi, 4
		loop		BOXBLUR_LOOP2
	BOXBLUR_LOOP_END:
	}
#endif
}

/*
 * buildIntegralImage: 画像からインテグラルイメージを作成する
 */
void tTVPBlurTransHandler::buildIntegralImage32(
		IPIXELTYPE *dst, tjs_int dstpitch,
		iTVPScanLineProvider *srcdat, tjs_int width, tjs_int height,
		tjs_int xblur, tjs_int yblur)
{
	// iimgp は 画面に縦横ブラー+1と縦一ドットを追加した大きめ画面。
	// 最初の列は0で埋める
	for	(int x = -xblur-1; x < width+xblur; x++)
		*dst++ = 0, *dst++ = 0, *dst++ = 0, dst++;
	// 次からの列は「ここまでの和」＋「一つ上の値」
	tjs_uint32 *src;
	if (TJS_FAILED(KaichoGetSrcScanLine(srcdat, 0, (const void**)&src))) return;
//log(L"dst=0x%08x, dstpitch=%d, src=0x%08x, width=%d, height=%d, xblur=%d, yblur=%d", 
//	dst, dstpitch, src, width, height, xblur, yblur);
	for (int y = -yblur-1; y < 0; y++) {
		addALineToIntegralImage32(dst, (tjs_uint32*)((BYTE*)dst-dstpitch), src, width, xblur);
		dst = (IPIXELTYPE*)((BYTE*)dst + dstpitch);
	}
	for (int y = 0; y < height-1; y++) {
		if (TJS_FAILED(KaichoGetSrcScanLine(srcdat, y, (const void**)&src))) return;
		addALineToIntegralImage32(dst, (tjs_uint32*)((BYTE*)dst-dstpitch), src, width, xblur);
		dst = (IPIXELTYPE*)((BYTE*)dst + dstpitch);
	}
	if (TJS_FAILED(KaichoGetSrcScanLine(srcdat, height-1, (const void**)&src))) return;
	for (int y = height; y < height+yblur; y++) {
		addALineToIntegralImage32(dst, (tjs_uint32*)((BYTE*)dst-dstpitch), src, width, xblur);
		dst = (IPIXELTYPE*)((BYTE*)dst + dstpitch);
	}
}


//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD tTVPBlurTransHandler::Process(
			tTVPDivisibleData *data)
{
	// トランジションの各領域ごとに呼ばれる
	// 吉里吉里は画面を更新するときにいくつかの領域に分割しながら処理を行うので
	// このメソッドは通常、画面更新一回につき複数回呼ばれる

	// data には領域や画像に関する情報が入っている

	if (First) {	// 最初の実行
		// 初回は、インテグラルイメージを作る。
		buildIntegralImage32((IPIXELTYPE*)Iimg1, Iimgpitch1, data->Src1, Width, Height, MaxXblur1, MaxYblur1);
		buildIntegralImage32((IPIXELTYPE*)Iimg2, Iimgpitch2, data->Src2, Width, Height, MaxXblur2, MaxYblur2);
		First = false;
//log(L"Iimg1=0x%08x, Iimgwidth1=%d, Iimgheight1=%d, Iimgpitch1=%d, MaxXblur1=%d, MaxYblur1=%d",
//		Iimg1, Iimgwidth1, Iimgheight1, Iimgpitch1, MaxXblur1, MaxYblur1);
//log(L"Iimg2=0x%08x, Iimgwidth2=%d, Iimgheight2=%d, Iimgpitch2=%d, MaxXblur2=%d, MaxYblur2=%d",
//		Iimg2, Iimgwidth2, Iimgheight2, Iimgpitch2, MaxXblur2, MaxYblur2);
	}

	// ここから boxBlur
	int sq1 = (CurXblur1*2+1)*(CurYblur1*2+1), sq2 = (CurXblur2*2+1)*(CurYblur2*2+1);
	// ラインごとに処理
	for (int n = 0; n < data->Height; n++) {
		int	dstx = data->DestLeft;
		int	dsty = data->DestTop + n;
		int srcx = data->Left;
		int srcy = data->Top + n;
		// bdst1 に src1 からの一ライン分のBlur後配列を求める
		IPIXELTYPE *src_u, *src_d;
		src_u = (IPIXELTYPE*)getIimg1Addr(srcx-(CurXblur1+1), srcy-(CurYblur1+1));	// 上基準
		src_d = (IPIXELTYPE*)getIimg1Addr(srcx-(CurXblur1+1), srcy+(CurYblur1+0));	// 下基準
		drawALineFromIntegralImageToImage32(bdst1, data->Width, src_u, src_d, CurXblur1, sq1);
		// bdst2 に src2 からの一ライン分のBlur後配列を求める
		src_u = (IPIXELTYPE*)getIimg2Addr(srcx-(CurXblur2+1), srcy-(CurYblur2+1));	// 上基準
		src_d = (IPIXELTYPE*)getIimg2Addr(srcx-(CurXblur2+1), srcy+(CurYblur2+0));	// 下基準
		drawALineFromIntegralImageToImage32(bdst2, data->Width, src_u, src_d, CurXblur2, sq2);

		tjs_uint32 *dst;
		if (TJS_FAILED(KaichoGetDstScanLine(data->Dest, data->DestTop+n, (void**)&dst)))
			return TJS_E_FAIL;
		// で、ここでbdst1とbdst2を合成しつつdstに転送する
		if (LayerType == ltAlpha) {
			TVPConstAlphaBlend_SD_d(dst+data->DestLeft-data->Left,
									bdst1, bdst2, data->Width, BlendRatio);
		} else if (LayerType == ltAddAlpha) {
			TVPConstAlphaBlend_SD_a(dst+data->DestLeft-data->Left,
									bdst1, bdst2, data->Width, BlendRatio);
		} else {
			TVPConstAlphaBlend_SD(dst+data->DestLeft-data->Left,
									bdst1, bdst2, data->Width, BlendRatio);
		}
	}

	return TJS_S_OK;
}
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
class tTVPBlurTransHandlerProvider : public iTVPTransHandlerProvider
{
	tjs_uint RefCount; // 参照カウンタ
public:
	tTVPBlurTransHandlerProvider() { RefCount = 1; }
	~tTVPBlurTransHandlerProvider() {; }

	tjs_error TJS_INTF_METHOD AddRef()
	{
		// iTVPBaseTransHandler の AddRef
		// 参照カウンタをインクリメント
		RefCount ++;
		return TJS_S_OK;
	}

	tjs_error TJS_INTF_METHOD Release()
	{
		// iTVPBaseTransHandler の Release
		// 参照カウンタをデクリメントし、0 になるならば delete this
		if(RefCount == 1)
			delete this;
		else
			RefCount--;
		return TJS_S_OK;
	}

	tjs_error TJS_INTF_METHOD GetName(
			/*out*/const tjs_char ** name)
	{
		// このトランジションの名前を返す
		if(name) *name = TJS_W("blur");
		return TJS_S_OK;
	}


	tjs_error TJS_INTF_METHOD StartTransition(
			/*in*/iTVPSimpleOptionProvider *options, // option provider
			/*in*/iTVPSimpleImageProvider *imagepro, // image provider
			/*in*/tTVPLayerType layertype, // destination layer type
			/*in*/tjs_uint src1w, tjs_uint src1h, // source 1 size
			/*in*/tjs_uint src2w, tjs_uint src2h, // source 2 size
			/*out*/tTVPTransType *type, // transition type
			/*out*/tTVPTransUpdateType * updatetype, // update typwe
			/*out*/iTVPBaseTransHandler ** handler // transition handler
			)
	{
		if(type) *type = ttExchange; // transition type : exchange
		if(updatetype) *updatetype = tutDivisible;
			// update type : divisible
		if(!handler) return TJS_E_FAIL;
		if(!options) return TJS_E_FAIL;

		if(src1w != src2w || src1h != src2h)
			return TJS_E_FAIL; // src1 と src2 のサイズが一致していないと駄目


		// オプションを得る
		tTJSVariant tmp;
		tjs_uint64 time;
		tjs_uint32 xblur1 = 32, yblur1 = 32;
		tjs_uint32 xblur2 = 32, yblur2 = 32;
		double accel = 1.0;
		bool dynamic = false;

		if(TJS_FAILED(options->GetValue(TJS_W("time"), &tmp)))
			return TJS_E_FAIL; // time 属性が指定されていない
		if(tmp.Type() == tvtVoid) return TJS_E_FAIL;
		auto duration = pluginSafety::readDuration(tmp);
		if(!duration) return TJS_E_FAIL;
		time = static_cast<tjs_int64>(duration.value);
		if(time < 2) time = 2; // あまり小さな数値を指定すると問題が起きるので

		if(TJS_SUCCEEDED(options->GetValue(TJS_W("blur1x"), &tmp)))
			if(tmp.Type() != tvtVoid) xblur1 = (tjs_int)tmp;
		if(TJS_SUCCEEDED(options->GetValue(TJS_W("blur1y"), &tmp)))
			if(tmp.Type() != tvtVoid) yblur1 = (tjs_int)tmp;
		if(TJS_SUCCEEDED(options->GetValue(TJS_W("blur1"), &tmp)))
			if(tmp.Type() != tvtVoid) xblur1 = yblur1 = (tjs_int)tmp;

		if(TJS_SUCCEEDED(options->GetValue(TJS_W("blur2x"), &tmp)))
			if(tmp.Type() != tvtVoid) xblur2 = (tjs_int)tmp;
		if(TJS_SUCCEEDED(options->GetValue(TJS_W("blur2y"), &tmp)))
			if(tmp.Type() != tvtVoid) yblur2 = (tjs_int)tmp;
		if(TJS_SUCCEEDED(options->GetValue(TJS_W("blur2"), &tmp)))
			if(tmp.Type() != tvtVoid) xblur2 = yblur2 = (tjs_int)tmp;

		if(TJS_SUCCEEDED(options->GetValue(TJS_W("accel"), &tmp)))
			if(tmp.Type() != tvtVoid) accel = (double)tmp;

		if(TJS_SUCCEEDED(options->GetValue(TJS_W("dynamic"), &tmp)))
			if(tmp.Type() != tvtVoid) dynamic = ((tjs_int)tmp != 0);

		// オブジェクトを作成
		*handler = new tTVPBlurTransHandler(time, layertype,
			src1w, src1h,
			xblur1, yblur1, xblur2, yblur2,
			accel, dynamic);

		return TJS_S_OK;
	}

} static * BlurTransHandlerProvider;
//---------------------------------------------------------------------------
void RegisterBlurTransHandlerProvider()
{
	// TVPAddTransHandlerProvider を使ってトランジションハンドラプロバイダを
	// 登録する
	if (BlurTransHandlerProvider)
		return;
	BlurTransHandlerProvider = new tTVPBlurTransHandlerProvider();
	try {
		TVPAddTransHandlerProvider(BlurTransHandlerProvider);
	} catch (...) {
		BlurTransHandlerProvider->Release();
		BlurTransHandlerProvider = nullptr;
	}
}
//---------------------------------------------------------------------------
void UnregisterBlurTransHandlerProvider()
{
	// TVPRemoveTransHandlerProvider を使ってトランジションハンドラプロバイダを
	// 登録抹消する
	if (!BlurTransHandlerProvider)
		return;
	TVPRemoveTransHandlerProvider(BlurTransHandlerProvider);
	BlurTransHandlerProvider->Release();
	BlurTransHandlerProvider = nullptr;
}
//---------------------------------------------------------------------------
