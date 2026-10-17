//---------------------------------------------------------------------------
#include <math.h>
#include "common.h"
#include "dim.h"


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
	Dim トランジション
	2011/12/09	0.2.1.0	初期リリース
*/
//---------------------------------------------------------------------------

// The original used MSVC x86 inline assembly. This port uses the scalar
// path, which is complete.
// #define USE_SSE2

// ホントはtTVPUniversalTransHandlerを継承したかったんだけど、Headerで定義
// されていないから断念。
class tTVPDimTransHandler : public iTVPDivisibleTransHandler
{
	//	boxDim トランジションハンドラクラスの実装

	tjs_int RefCount; // 参照カウンタ
	/*
	 * iTVPDivisibleTransHandler は 参照カウンタによる管理を行う
	 */

protected:
	bool          First;		// 一番最初の呼び出しかどうか
	tjs_int64     StartTick;	// トランジションを開始した tick count
	tjs_int64     Time;			// トランジションに要する時間
	tTVPLayerType LayerType;	// レイヤタイプ
	tjs_int       Width;		// 処理する画像の幅
	tjs_int       Height;		// 処理する画像の高さ

	tjs_int64     CurTime;		// 現在の tick count(0〜Time)
	tjs_int       Phase;		// 現在のフェーズ(0〜Vague+255)

	iTVPScanLineProvider *Ruleimg;	// グレースケールでブラーされたルール画像
	double        CurRatio;		// 現在の変化率
	double        Accel;		// dimする加速度。def=1.0、<-1で最初遅く最後早い、>1で最初早く最後遅い
	tjs_uint32    BlendTable[256];	// Universal Transitionのブレンドテーブル
	tjs_int       Vague;

	// 現在のRatio(0〜1.0)を求める
	inline double setCurrentRatio()
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

public:
	// コンストラクタ
	tTVPDimTransHandler(iTVPScanLineProvider *ruleimg, tjs_uint64 time, tjs_int vague, tTVPLayerType layertype,
		tjs_int width, tjs_int height, double accel)
	{
		RefCount    = 1;
		First       = true;

		LayerType   = layertype;
		Width       = width;
		Height      = height;
		Time        = time;

		Vague       = vague;
		Accel       = accel;
		Ruleimg     = ruleimg;
		Ruleimg->AddRef();
	}

	~tTVPDimTransHandler()
	{
		Ruleimg->Release();
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
		if(RefCount == 1) {
			delete this;
		} else {
			RefCount--;
		}
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

	void Blend(tTVPDivisibleData *data);

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
tjs_error TJS_INTF_METHOD tTVPDimTransHandler::StartProcess(tjs_uint64 tick)
{
	// トランジションの画面更新一回ごとに呼ばれる

	// トランジションの画面更新一回につき、まず最初に StartProcess が呼ばれる
	// そのあと Process が複数回呼ばれる ( 領域を分割処理している場合 )
	// 最後に EndProcess が呼ばれる

	if (First) {	// 最初の実行
		StartTick = tick;
		First = false;
	}

	// 画像演算に必要な各パラメータを計算
	CurTime = ((tjs_int64)tick - StartTick);
	if (CurTime > Time) CurTime = Time;

	// CurRatio計算
	setCurrentRatio();
	Phase = (tjs_int)(CurRatio*(255+Vague));

	if (TVPIsTypeUsingAlpha(LayerType))
		TVPInitUnivTransBlendTable_d(BlendTable, Phase, Vague);
	else if(TVPIsTypeUsingAddAlpha(LayerType))
		TVPInitUnivTransBlendTable_a(BlendTable, Phase, Vague);
	else
		TVPInitUnivTransBlendTable(BlendTable, Phase, Vague);

	return TJS_S_TRUE;
}

//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD tTVPDimTransHandler::EndProcess()
{
	// トランジションの画面更新一回分が終わるごとに呼ばれる

	if(CurRatio >= 1.0) return TJS_S_FALSE; // トランジション終了

	return TJS_S_TRUE;
}


//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD tTVPDimTransHandler::Process(
			tTVPDivisibleData *data)
{
	// トランジションの各領域ごとに呼ばれる
	// 吉里吉里は画面を更新するときにいくつかの領域に分割しながら処理を行うので
	// このメソッドは通常、画面更新一回につき複数回呼ばれる

	// data には領域や画像に関する情報が入っている

	if (CurRatio == 0.0) {
		data->Dest     = data->Src1;
		data->DestLeft = data->Src1Left;
		data->DestTop  = data->Src1Top;
	} else if (CurRatio >= 1.0) {
		data->Dest     = data->Src2;
		data->DestLeft = data->Src2Left;
		data->DestTop  = data->Src2Top;
	} else {
		Blend(data);
	}

	return TJS_S_OK;
}
//---------------------------------------------------------------------------

// Univeral Transtionと同じBlend()関数をそのまま使う
void tTVPDimTransHandler::Blend(tTVPDivisibleData *data)
{
	// blend the image according with the rule graphic
	tjs_uint8 *dest;
	const tjs_uint8 *src1;
	const tjs_uint8 *src2;
	const tjs_uint8 *rule;

	if (TJS_FAILED(KaichoGetDstScanLine(data->Dest, data->DestTop, (void**)&dest)))
		return;
	if (TJS_FAILED(KaichoGetSrcScanLine(data->Src1, data->Src1Top, (const void**)&src1)))
		return;
	if (TJS_FAILED(KaichoGetSrcScanLine(data->Src2, data->Src2Top, (const void**)&src2)))
		return;
	if (TJS_FAILED(KaichoGetSrcScanLine(Ruleimg, data->Top, (const void**)&rule)))
		return;

	/* ルール画像表示用
	for (int y = 0; y < data->Height; y++) {
		data->Dest->GetScanLineForWrite(data->DestTop+y, (void**)&dest);
		Ruleimg->GetScanLine(data->Top+y, (const void**)&rule);
		for (int x = 0; x < data->Width; x++) {
			*dest++ = 0xff;
			*dest++ = *rule;
			*dest++ = *rule;
			*dest++ = *rule++;
		}
	}
	return;
	*/

	tjs_int destpitch;
	tjs_int src1pitch;
	tjs_int src2pitch;
	tjs_int rulepitch;

	destpitch = KaichoGetDstPitch(data->Dest);
	src1pitch = KaichoGetSrcPitch(data->Src1);
	src2pitch = KaichoGetSrcPitch(data->Src2);
	rulepitch = KaichoGetSrcPitch(Ruleimg);

	dest += data->DestLeft * sizeof(tjs_uint32);
	src1 += data->Src1Left * sizeof(tjs_uint32);
	src2 += data->Src2Left * sizeof(tjs_uint32);
	rule += data->Left * sizeof(tjs_uint8);

	tjs_int h = data->Height;
	if(Vague >= 512) {
		if(TVPIsTypeUsingAlpha(LayerType)) {
			while(h--) {
				TVPUnivTransBlend_d((tjs_uint32*)dest, (const tjs_uint32*)src1,
					(const tjs_uint32*)src2, rule, BlendTable, data->Width);
				dest += destpitch, src1 += src1pitch, src2 += src2pitch;
				rule += rulepitch;
			}
		} else if(TVPIsTypeUsingAddAlpha(LayerType)) {
			while(h--) {
				TVPUnivTransBlend_a((tjs_uint32*)dest, (const tjs_uint32*)src1,
					(const tjs_uint32*)src2, rule, BlendTable, data->Width);
				dest += destpitch, src1 += src1pitch, src2 += src2pitch;
				rule += rulepitch;
			}
		} else {
			while(h--) {
				TVPUnivTransBlend((tjs_uint32*)dest, (const tjs_uint32*)src1,
					(const tjs_uint32*)src2, rule, BlendTable, data->Width);
				dest += destpitch, src1 += src1pitch, src2 += src2pitch;
				rule += rulepitch;
			}
		}
	} else {
		tjs_int src1lv = Phase;
		tjs_int src2lv = Phase - Vague;

		if(TVPIsTypeUsingAlpha(LayerType)) {
			while(h--) {
				TVPUnivTransBlend_switch_d((tjs_uint32*)dest, (const tjs_uint32*)src1,
					(const tjs_uint32*)src2, rule, BlendTable, data->Width,
						src1lv, src2lv);
				dest += destpitch, src1 += src1pitch, src2 += src2pitch;
				rule += rulepitch;
			}
		} else if(TVPIsTypeUsingAddAlpha(LayerType)) {
			while(h--) {
				TVPUnivTransBlend_switch_a((tjs_uint32*)dest, (const tjs_uint32*)src1,
					(const tjs_uint32*)src2, rule, BlendTable, data->Width,
						src1lv, src2lv);
				dest += destpitch, src1 += src1pitch, src2 += src2pitch;
				rule += rulepitch;
			}
		} else {
			while(h--) {
				TVPUnivTransBlend_switch((tjs_uint32*)dest, (const tjs_uint32*)src1,
					(const tjs_uint32*)src2, rule, BlendTable, data->Width,
						src1lv, src2lv);
				dest += destpitch, src1 += src1pitch, src2 += src2pitch;
				rule += rulepitch;
			}
		}
	}
}
 

//---------------------------------------------------------------------------
class tTVPDimTransHandlerProvider : public iTVPTransHandlerProvider
{
	tjs_uint RefCount; // 参照カウンタ

	inline void addALineToIntegralImage8(DWORD *dst, DWORD *dstprev, BYTE *src, tjs_int srcwidth, tjs_int xblur);
	inline void drawALineFromIntegralImageToImage8(BYTE *dst, tjs_int dstwidth, DWORD *src1, DWORD *src2, tjs_int xblur, tjs_int sq);
	void DoBoxBlur(iTVPScanLineProvider *img, tjs_int xblur, tjs_int yblur);
	void NegateGrayImage(iTVPScanLineProvider *img);
public:
	tTVPDimTransHandlerProvider() { RefCount = 1; }
	~tTVPDimTransHandlerProvider() {; }

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
		if(name) *name = TJS_W("dim");
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

		if(TJS_FAILED(options->GetValue(TJS_W("time"), &tmp)))
			return TJS_E_FAIL; // time 属性が指定されていない
		if(tmp.Type() == tvtVoid)
			return TJS_E_FAIL;
		auto duration = pluginSafety::readDuration(tmp);
		if(!duration)
			return TJS_E_FAIL;
		tjs_int64 time = static_cast<tjs_int64>(duration.value);
		if(time < 2) time = 2; // あまり小さな数値を指定すると問題が起きるので

		tjs_int vague = 64;
		if(TJS_SUCCEEDED(options->GetValue(TJS_W("vague"), &tmp)))
			if (tmp.Type() != tvtVoid)
				vague = (tjs_int)tmp;
		tjs_int32 xblur=16, yblur=16;
		if(TJS_SUCCEEDED(options->GetValue(TJS_W("blur"), &tmp)))
			if (tmp.Type() != tvtVoid)
				xblur = yblur = (tjs_int32)tmp;
		if(TJS_SUCCEEDED(options->GetValue(TJS_W("xblur"), &tmp)))
			if (tmp.Type() != tvtVoid)
				xblur = (tjs_int32)tmp;
		if(TJS_SUCCEEDED(options->GetValue(TJS_W("yblur"), &tmp)))
			if (tmp.Type() != tvtVoid)
				yblur = (tjs_int32)tmp;
		double accel = 1.0;
		if(TJS_SUCCEEDED(options->GetValue(TJS_W("accel"), &tmp)))
			if(tmp.Type() != tvtVoid)
				accel = (double)tmp;

		// retrieve "rule" option and load it as an image
		const tjs_char *rulename;

		tjs_error er = options->GetAsString(TJS_W("rule"), &rulename);
		if(TJS_FAILED(er))
			TVPThrowExceptionMessage(TJS_W("オプション %1 を指定してください"), TJS_W("rule"));
		iTVPScanLineProvider *ruleimg;
		er = imagepro->LoadImage(rulename, 8, 0x02ffffff, src1w, src1h, &ruleimg);
		if(TJS_FAILED(er))
			TVPThrowExceptionMessage(TJS_W("ルール画像 %1 を読み込むことができません"), rulename);

		DoBoxBlur(ruleimg, xblur, yblur);
		// 反転が必要なら反転する
		if(TJS_SUCCEEDED(options->GetValue(TJS_W("neg"), &tmp)))
			if(tmp.Type() != tvtVoid)
				NegateGrayImage(ruleimg);

		// オブジェクトを作成
		try {
			*handler = new tTVPDimTransHandler(ruleimg, time, vague, layertype, src1w, src1h, accel);
		} catch(...) {
			ruleimg->Release();
			throw;
		}
		ruleimg->Release();

		return TJS_S_OK;
	}

} static * DimTransHandlerProvider;


// Integral Image dst に、src から一ライン追加する。
inline void tTVPDimTransHandlerProvider::addALineToIntegralImage8(DWORD *dst, DWORD *dstprev, BYTE *src, tjs_int srcwidth, tjs_int xblur)
{
	DWORD sum = 0;
#ifndef USE_SSE2
	BYTE col = *src;
	for (int x = -xblur-1; x < 0; x++) {
		sum += col;
		*dst++ = sum + *dstprev++;
	}
	for (int x = 0; x < srcwidth-1; x++) {
		sum += *src++;
		*dst++ = sum + *dstprev++;
	}
	col = *src++;
	for (int x = srcwidth-1; x < srcwidth+xblur; x++) {
		sum += col;
		*dst++ = sum + *dstprev++;
	}
#else
	__asm {
		mov		esi,   src
		mov		edi,   dst
		mov		ebx,   dstprev
		mov		ecx,   xblur
		inc		ecx
	ADDALINETOINTEGRALIMAGE8_LOOP1:
		mov		edx,   [esi]
		and		edx,   0xff
		add		eax,   edx	// sum += *src
		mov		edx,   [ebx]
		add		edx,   eax
		mov		[edi], edx	// *dst = sum + *dstprev
		add		ebx,   4
		add		edi,   4
		loop		ADDALINETOINTEGRALIMAGE8_LOOP1

		mov		ecx,   srcwidth
		dec		ecx
	ADDALINETOINTEGRALIMAGE8_LOOP2:
		mov		edx,   [esi]
		and		edx,   0xff
		add		eax,   edx	// sum += *src
		mov		edx,   [ebx]
		add		edx,   eax
		mov		[edi], edx	// *dst = sum + *dstprev
		inc		esi
		add		ebx,   4
		add		edi,   4
		loop		ADDALINETOINTEGRALIMAGE8_LOOP2

		mov		ecx,   xblur
		inc		ecx
	ADDALINETOINTEGRALIMAGE8_LOOP3:
		mov		edx,   [esi]
		and		edx,   0xff
		add		eax,   edx	// sum += *src
		mov		edx,   [ebx]
		add		edx,   eax
		mov		[edi], edx	// *dst = sum + *dstprev
		add		ebx,   4
		add		edi,   4
		loop		ADDALINETOINTEGRALIMAGE8_LOOP3
	}
#endif 
}


// インテグラルイメージから、ブラーした画像を一ライン作成する
inline void tTVPDimTransHandlerProvider::drawALineFromIntegralImageToImage8(BYTE *dst, tjs_int dstwidth, DWORD *src1, DWORD *src2, tjs_int xblur, tjs_int sq)
{
	int xblurwidth = xblur*2+1;
#ifndef USE_SSE2
	for (int x = 0; x < dstwidth; x++) {
		*dst++ = ((*src1 - *(src1+xblurwidth) - *src2 + *(src2+xblurwidth))/sq)&0xff;
		src1++, src2++;
	}
#else
	float rsqf = (float)65536/sq;
	__asm {
		mov			esi,  src1
		mov			edx,  src2
		mov			edi,  dst
		mov			ebx,  xblurwidth
		sal			ebx,  2
		mov			eax,  rsqf
		movd		xmm1, eax
		pshufd		xmm1, xmm1, 0x0
		mov			ecx,  dstwidth
		sar			ecx,  2
		jz			DRAWALINEFROMINTEGRALIMAGETOIMAGE8_NEXT
	DRAWALINEFROMINTEGRALIMAGETOIMAGE8_LOOPx4:
		movdqu		xmm0, [esi]
		movdqu		xmm2, [edx+ebx]
		paddd		xmm0, xmm2
		movdqu		xmm2, [esi+ebx]
		psubd		xmm0, xmm2
		movdqu		xmm2, [edx]
		psubd		xmm0, xmm2	// xmm0 = |sum1|sum2|sum3|sum4|
		cvtdq2ps	xmm0, xmm0
		mulps		xmm0, xmm1
		cvtps2dq	xmm0, xmm0
		psrld		xmm0, 16	// xmm0 = |g1|g2|g3|g4|(dword)
		packssdw	xmm0, xmm0
		packuswb	xmm0, xmm0	// xmm0 = |g1|g2|g3|g4|(byte)
		movd		[edi],xmm0
		add			esi,  16
		add			edx,  16
		add			edi,  4
		loop		DRAWALINEFROMINTEGRALIMAGETOIMAGE8_LOOPx4

	DRAWALINEFROMINTEGRALIMAGETOIMAGE8_NEXT:
		mov			ecx,   dstwidth
		and			ecx,   0x3
		jz			DRAWALINEFROMINTEGRALIMAGETOIMAGE8_END
	DRAWALINEFROMINTEGRALIMAGETOIMAGE8_LOOPx1:
		mov			eax,  [esi]
		add			eax,  [edx+ebx]
		sub			eax,  [esi+ebx]
		sub			eax,  [edx]	// xmm0 = |sum1|sum2|sum3|sum4|
		movd		xmm0, eax
		cvtdq2ps	xmm0, xmm0
		mulps		xmm0, xmm1	// xmm0 *= 65536/sq
		cvtps2dq	xmm0, xmm0
		psrld		xmm0, 16	// xmm0 = |g1|g2|g3|g4|(dword)
		packssdw	xmm0, xmm0
		packuswb	xmm0, xmm0	// xmm0 = |g1|g2|g3|g4|(byte)
		movd		eax,  xmm0
		mov			[edi],al
		add			esi,  4
		add			edx,  4
		inc			edi
		loop		DRAWALINEFROMINTEGRALIMAGETOIMAGE8_LOOPx1
	DRAWALINEFROMINTEGRALIMAGETOIMAGE8_END:
	}
#endif
}


void tTVPDimTransHandlerProvider::DoBoxBlur(iTVPScanLineProvider *img, tjs_int xblur, tjs_int yblur)
{
	tjs_int width, height, pitch;
	img->GetWidth(&width), img->GetHeight(&height);
	pitch = KaichoGetSrcPitch(img);
	(void)pitch;
	// iimg = インテグラルイメージ
	int  iimgwidth = width+xblur*2+1;
	int  iimgpitch = iimgwidth*sizeof(DWORD);
	int  iimgsiz   = (yblur*2+1+1)*iimgpitch;
	BYTE *iimg     = (BYTE*)TJSAlignedAlloc(iimgsiz, 4);
	// 最初のインテグラルイメージ((xblur*2+1)x(yblur*2+1))作成

	DWORD *iimgp = (DWORD*)iimg;
	// 最初の列は0で埋める
	for	(int x = -xblur-1; x < width+xblur; x++)
		*iimgp++ = 0;
	// 次からの列は「ここまでの和」＋「一つ上の値」
	BYTE *src;
	for (int y = -yblur; y < yblur+1 ; y++) {
		tjs_int line = y;
		if (line < 0)
			line = 0;
		else if (line >= height)
			line = height-1;
		if (TJS_FAILED(KaichoGetSrcScanLine(img, line, (const void**)&src))) {
			TJSAlignedDealloc(iimg);
			return;
		}
		iimgp = (DWORD*)(iimg + (yblur+1+y)*iimgpitch);
		addALineToIntegralImage8(iimgp, (DWORD*)((BYTE*)iimgp-iimgpitch), src, width, xblur);
	}
	// これで最初のインテグラルイメージ(iimgwidth x yblur*2+1+1) が iimg 以下にできた

	int sq = (xblur*2+1)*(yblur*2+1);
	int xblurwidth = (xblur*2+1)*sizeof(DWORD);
	BYTE *dst;
	// 上・下ポインタ
	DWORD *iimgp1 = (DWORD*)iimg, *iimgp2 = (DWORD*)(iimg+(yblur*2+1)*iimgpitch);
	for (int y = 0; y < height; y++) {
		// 今のIntegralImageで描画
		if (TJS_FAILED(KaichoGetDstScanLine(img, y, (void**)&dst))) {
			TJSAlignedDealloc(iimg);
			return;
		}
		drawALineFromIntegralImageToImage8(dst, width, iimgp1, iimgp2, xblur, sq);
		// ↑4000/6800 くらいの重さ
		// 新しいラインを追加する
		BYTE *imgyblurp;
		tjs_int line = y+yblur+1;
		if (line >= height)
			line = height-1;
		if (TJS_FAILED(KaichoGetSrcScanLine(img, line, (const void**)&imgyblurp))) {
			TJSAlignedDealloc(iimg);
			return;
		}
		addALineToIntegralImage8(iimgp1, iimgp2, imgyblurp, width, xblur);
		iimgp2 = iimgp1;
		iimgp1 = (DWORD*)((BYTE*)iimgp1 + iimgpitch);
		if ((BYTE*)iimgp1 >= iimg+iimgsiz)
			iimgp1 = (DWORD*)iimg;
		// ↑1000/6800 くらいの重さ
	}
	TJSAlignedDealloc(iimg);
}


void tTVPDimTransHandlerProvider::NegateGrayImage(iTVPScanLineProvider *img)
{
	tjs_int width, height, pitch;
	img->GetWidth(&width), img->GetHeight(&height);
	pitch = KaichoGetDstPitch(img);
	(void)pitch;
	BYTE *p;

	for (int y = 0; y < height; y++) {
		if (TJS_FAILED(KaichoGetDstScanLine(img, y, (void **)&p)))
			return;
		for (int x = 0; x < width; x++) {
			*p = 255-*p;
			p++;
		}
	}
}

//---------------------------------------------------------------------------
void RegisterDimTransHandlerProvider()
{
	// TVPAddTransHandlerProvider を使ってトランジションハンドラプロバイダを
	// 登録する
	if (DimTransHandlerProvider)
		return;
	DimTransHandlerProvider = new tTVPDimTransHandlerProvider();
	try {
		TVPAddTransHandlerProvider(DimTransHandlerProvider);
	} catch (...) {
		DimTransHandlerProvider->Release();
		DimTransHandlerProvider = nullptr;
	}
}
//---------------------------------------------------------------------------
void UnregisterDimTransHandlerProvider()
{
	// TVPRemoveTransHandlerProvider を使ってトランジションハンドラプロバイダを
	// 登録抹消する
	if (!DimTransHandlerProvider)
		return;
	TVPRemoveTransHandlerProvider(DimTransHandlerProvider);
	DimTransHandlerProvider->Release();
	DimTransHandlerProvider = nullptr;
}
//---------------------------------------------------------------------------
