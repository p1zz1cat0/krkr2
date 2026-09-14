// layerExAreaAverage.dll
//
// 移植自 wtnbgo/layerExAreaAverage（作者 kiyobee）：
//   https://github.com/wtnbgo/layerExAreaAverage
// 许可证遵循吉里吉里本体（KrKr2）。
//
// 为 Layer 增加面积平均法（area average）缩小专用复制方法：
//   Layer.stretchCopyAA(dleft, dtop, dwidth, dheight, src, sleft, stop, swidth, sheight)
// 仅支持缩小（放大抛异常）；对半透明像素按 alpha 加权颜色贡献，
// 全透明区域的颜色仅在区域内完全没有不透明像素时才被采用。
// 速度慢，适合工具/缩略图用途，不适合实时绘制。
//
// 与上游的差异（正常输入下输出逐位一致）：
//   - 缓冲获取经 PluginSafety 的 LayerReadView/LayerWriteView（上游直接读
//     mainImageBuffer 裸指针，层无图时解引用空指针）；
//   - 像素位序换成本 fork 的 A<<24|B<<16|G<<8|R（RGBA 内存序，低字节 R；
//     与 shrinkCopy 一致）。上游 Windows krkr2 为 A<<24|R<<16|G<<8|B，
//     R/B 通道读写与上游源码互换，颜色语义不变；
//   - 负坐标与零/负尺寸在上游是未定义行为（越界写、除零），入口显式拒绝；
//   - 定点累加器用 int64（上游 int32 在目标像素覆盖约 2048 个以上源像素时
//     溢出 UB，例如大图缩到 1x1）；
//   - 修正上游 ex/ey 的 "-1 钳制"（见 CopyAreaAverage 内注释）：上游在目标
//     覆盖区触及源图像右/下边界时丢最后一列/行，2:1 整数缩放必触发，
//     目标高/宽为 1 像素时整体不写入。

#include "ncbind.hpp"
#include "common/PluginSafety.h"

#include <cstddef>
#include <limits>
#include <spdlog/spdlog.h>

#include "TickCount.h"

#define NCB_MODULE_NAME TJS_W("layerExAreaAverage.dll")

// 上游为 Windows 调用约定声明；本工程无此宏时保持为空
#ifndef TJS_INTF_METHOD
#define TJS_INTF_METHOD
#endif

namespace {
void LogPluginError(const char *msg) {
    if (auto logger = spdlog::get("plugin"))
        logger->error("[layerExAreaAverage] {}", msg);
}

// Native classes are registered lazily in a minimal startup.tjs environment.
// NCB_ATTACH_FUNCTION silently skips registration when its target class does
// not exist yet, while the module is still marked as loaded.  Resolve Layer in
// the pre-registration phase so Plugins.link("layerExAreaAverage.dll") works
// before a game creates its first Window/Layer as well as after normal KAG
// startup.
void EnsureLayerClassRegistered() {
    tTJSVariant layerClass;
    TVPExecuteExpression(TJS_W("Layer"), &layerClass);
    if(layerClass.Type() != tvtObject)
        TVPThrowExceptionMessage(
            TJS_W("layerExAreaAverage.dll requires the Layer class."));
}
} // namespace

NCB_PRE_REGIST_CALLBACK(EnsureLayerClassRegistered);

// Registration-only translation units in static libraries can otherwise be
// discarded by the linker. PluginImpl.cpp references this anchor explicitly.
extern "C" void TVPLayerExAreaAveragePluginAnchor() {}

struct LayerExAreaAverage
{
	// 上游裁剪公式保持的矩形语义：坐标/尺寸均为 tjs_int
	struct CopyGeometry
	{
		tjs_int dLeft, dTop, dWidth, dHeight;
		tjs_int sLeft, sTop, sWidth, sHeight;
	};

	// 定点小数（上游 DOTBASE=12）。统一用 int64 域：上游 int32 的
	// MULFIXDOT/累加器在缩小比大时溢出 UB；非溢出输入与上游逐位一致。
	typedef tjs_int64 fixdot;
	static constexpr int kDotBase = 12;
	static constexpr fixdot kOneDot = static_cast<fixdot>(1) << kDotBase;
	static inline fixdot IntToFixDot(tjs_int64 v) { return v << kDotBase; }
	static inline fixdot RealToFixDot(double v) {
		return static_cast<fixdot>(v * static_cast<double>(kOneDot));
	}
	static inline tjs_int64 FixDotToInt(fixdot v) { return v >> kDotBase; }
	static inline fixdot MulFixDot(fixdot a, fixdot b) {
		return (a * b) >> kDotBase;
	}
	static inline tjs_int64 DivFixDot(fixdot a, fixdot b) { return a / b; }

	// 上游裁剪公式：目标矩形超出目标 image 时按比例收缩（同步收缩源矩形），
	// 源矩形超出源 image 时反向收缩。逐字移植上游的浮点计算顺序。
	// 上游未处理负坐标与完全出界：负坐标在入口已拒绝；完全出界经本函数
	// 自然收缩为退化矩形（dWidth/dHeight <= 0），由调用方跳过绘制。
	static void ClipToImages(CopyGeometry &g, tjs_int dImageWidth,
	                         tjs_int dImageHeight, tjs_int sImageWidth,
	                         tjs_int sImageHeight)
	{
		if(g.dLeft + g.dWidth > dImageWidth)
		{
			const tjs_int dw = dImageWidth - g.dLeft;
			g.sWidth = static_cast<tjs_int>(
				static_cast<double>(g.sWidth) *
				(static_cast<double>(dw) / static_cast<double>(g.dWidth)));
			g.dWidth = dw;
		}
		if(g.sLeft + g.sWidth > sImageWidth)
		{
			const tjs_int sw = sImageWidth - g.sLeft;
			g.dWidth = static_cast<tjs_int>(
				static_cast<double>(g.dWidth) *
				(static_cast<double>(sw) / static_cast<double>(g.sWidth)));
			g.sWidth = sw;
		}
		if(g.dTop + g.dHeight > dImageHeight)
		{
			const tjs_int dh = dImageHeight - g.dTop;
			g.sHeight = static_cast<tjs_int>(
				static_cast<double>(g.sHeight) *
				(static_cast<double>(dh) / static_cast<double>(g.dHeight)));
			g.dHeight = dh;
		}
		if(g.sTop + g.sHeight > sImageHeight)
		{
			const tjs_int sh = sImageHeight - g.sTop;
			g.dHeight = static_cast<tjs_int>(
				static_cast<double>(g.dHeight) *
				(static_cast<double>(sh) / static_cast<double>(g.sHeight)));
			g.sHeight = sh;
		}
	}

	// 面积平均核心：对目标矩形每个像素，计算其覆盖的源区域面积加权平均。
	// 前置条件（由 stretchCopyAA 验证）：g 经 ClipToImages 裁剪，目标矩形
	// 完整落在 dst image 内、源矩形完整落在 src image 内，四个尺寸均为正。
	// 裸缓冲 + 已验证几何的签名使确定性单测可以脱离 TJS 运行时直接调用；
	// 缓冲验证（布局/pitch/空指针）属于壳层的 LayerReadView/LayerWriteView。
	static void CopyAreaAverage(tjs_uint8 *dstBuffer, tjs_int dstPitch,
	                            const tjs_uint8 *srcBuffer, tjs_int srcPitch,
	                            const CopyGeometry &g, tjs_int sImageWidth,
	                            tjs_int sImageHeight)
	{
		const fixdot sl = IntToFixDot(g.sLeft);
		const fixdot st = IntToFixDot(g.sTop);
		const fixdot rw = RealToFixDot(static_cast<double>(g.sWidth) / g.dWidth);
		const fixdot rh =
			RealToFixDot(static_cast<double>(g.sHeight) / g.dHeight);

		for(tjs_int y = 0; y < g.dHeight; y++)
		{
			tjs_uint32 *outpixel = reinterpret_cast<tjs_uint32 *>(
				dstBuffer +
				static_cast<std::size_t>(y + g.dTop) *
					static_cast<std::size_t>(dstPitch));
			outpixel += g.dLeft;
			for(tjs_int x = 0; x < g.dWidth; x++)
			{
				// 缩小后目标像素 (x, y) 对应的源区域（fixdot）
				const fixdot x1 = sl + x * rw;
				const fixdot y1 = st + y * rh;
				const fixdot x2 = x1 + rw;
				const fixdot y2 = y1 + rh;

				// 整数源区域（区域覆盖到的源像素闭区间）
				tjs_int sx = static_cast<tjs_int>(FixDotToInt(x1));
				tjs_int sy = static_cast<tjs_int>(FixDotToInt(y1));
				tjs_int ex = static_cast<tjs_int>(FixDotToInt(x2 + kOneDot - 1));
				tjs_int ey = static_cast<tjs_int>(FixDotToInt(y2 + kOneDot - 1));
				// 上游对 ex/ey 的 "-1 钳制"（ex >= sImageWidth → ex =
				// sImageWidth-1）在目标覆盖区触及源图像右/下边界时把最后一
				// 列/行排除在平均之外：2:1 整数缩放时最右/最下目标像素内容
				// 错误，目标高/宽为 1 像素时内层循环整体空转、什么都不写。
				// 循环是半开区间 [sx, ex)，裁剪保证 x2 <= sImageWidth*kOneDot
				// ⇒ ex <= sImageWidth ⇒ ax <= sImageWidth-1，本就安全；此处
				// 只保留真越界防御，不收缩合法覆盖区。
				if(ex > sImageWidth) ex = sImageWidth;
				if(ey > sImageHeight) ey = sImageHeight;

				tjs_int64 totalarea_a = 0, accA = 0;
				tjs_int64 totalarea_rgb = 0, accR = 0, accG = 0, accB = 0;
				tjs_int64 totalarea_trgb = 0, trR = 0, trG = 0, trB = 0;
				for(tjs_int ay = sy; ay < ey; ay++)
				{
					const tjs_uint32 *inpixel =
						reinterpret_cast<const tjs_uint32 *>(
							srcBuffer +
							static_cast<std::size_t>(ay) *
								static_cast<std::size_t>(srcPitch));
					inpixel += sx;

					fixdot e1 = IntToFixDot(ay);
					fixdot e2 = IntToFixDot(ay + 1);
					if(e1 < y1) e1 = y1; // 上端
					if(e2 > y2) e2 = y2; // 下端
					const fixdot ah = e2 - e1;
					for(tjs_int ax = sx; ax < ex; ax++)
					{
						e1 = IntToFixDot(ax);
						e2 = IntToFixDot(ax + 1);
						if(e1 < x1) e1 = x1; // 左端
						if(e2 > x2) e2 = x2; // 右端
						const fixdot aw = e2 - e1;
						fixdot area = MulFixDot(aw, ah);
						totalarea_a += area;
						const tjs_uint32 alpha = (*inpixel >> 24) & 0xFF;
						accA += alpha * area;
						if(alpha > 0)
						{
							// α 值低的像素对颜色的贡献按 alpha 缩小（上游语义）
							// 本 fork 的 mainImageBuffer 位序为 A<<24|B<<16|G<<8|R
							// （RGBA 内存序，低字节 R；shrinkCopy 同此约定），
							// 与上游 Windows krkr2 的 A<<24|R<<16|G<<8|B 相反，
							// R/B 通道读法与上游源码互换
							area = (area * alpha) >> 8;
							accR += ((*inpixel) & 0xFF) * area;
							accG += ((*inpixel >> 8) & 0xFF) * area;
							accB += ((*inpixel >> 16) & 0xFF) * area;
							totalarea_rgb += area;
						}
						else
						{
							trR += ((*inpixel) & 0xFF) * area;
							trG += ((*inpixel >> 8) & 0xFF) * area;
							trB += ((*inpixel >> 16) & 0xFF) * area;
							totalarea_trgb += area;
						}
						inpixel++;
					}
				}

				// 上游防御分支：拒绝负坐标后每个目标像素至少覆盖一个源像素，
				// totalarea_a 恒为正，此分支不可达。
				if(totalarea_a == 0) continue;

				// 平均值计算并写入
				accA = DivFixDot(accA, totalarea_a);
				if(totalarea_rgb == 0)
				{
					totalarea_rgb = totalarea_trgb;
					accR = trR;
					accG = trG;
					accB = trB;
				}
				if(totalarea_rgb == 0)
				{
					accR = accG = accB = 0;
				}
				else
				{
					accR = DivFixDot(accR, totalarea_rgb);
					accG = DivFixDot(accG, totalarea_rgb);
					accB = DivFixDot(accB, totalarea_rgb);
				}
				// 输出位序同输入：A<<24 | B<<16 | G<<8 | R（fork RGBA 内存序）
				*outpixel = ((static_cast<tjs_int>(accA) & 0xFF) << 24) |
				            ((static_cast<tjs_int>(accB) & 0xFF) << 16) |
				            ((static_cast<tjs_int>(accG) & 0xFF) << 8) |
				            (static_cast<tjs_int>(accR) & 0xFF);

				outpixel++;
			}
		}
	}

	// 上游直接 (tjs_int)AsInteger() 截断；先检查 32 位范围再收窄，
	// 越界视为参数错误而不是静默截断
	static bool ReadCoord(const tTJSVariant &v, bool allowZero, tjs_int &out)
	{
		const tjs_int64 raw = v.AsInteger();
		if(raw < (allowZero ? 0 : 1) ||
		   raw > std::numeric_limits<tjs_int>::max())
			return false;
		out = static_cast<tjs_int>(raw);
		return true;
	}

	// 参数读取与几何校验（TJS 运行时无关，单测直接调用）：返回 TJS_S_OK
	// 或错误码；放大输入按上游语义经 TVPThrowExceptionMessage 抛出。
	// param[4]（源层对象）由调用方读取，不在此校验。
	static tjs_error ValidateParams(tjs_int numparams, tTJSVariant **param,
	                                CopyGeometry &g)
	{
		if(numparams < 9) return TJS_E_BADPARAMCOUNT;

		if(!ReadCoord(*param[0], true, g.dLeft) ||
		   !ReadCoord(*param[1], true, g.dTop) ||
		   !ReadCoord(*param[2], false, g.dWidth) ||
		   !ReadCoord(*param[3], false, g.dHeight) ||
		   !ReadCoord(*param[5], true, g.sLeft) ||
		   !ReadCoord(*param[6], true, g.sTop) ||
		   !ReadCoord(*param[7], false, g.sWidth) ||
		   !ReadCoord(*param[8], false, g.sHeight))
		{
			LogPluginError(
				"stretchCopyAA: coordinates must be >= 0 and sizes must be > "
				"0 (upstream leaves such input undefined)");
			return TJS_E_INVALIDPARAM;
		}

		// 扩大处理不可行（上游抛异常；消息原文为日文）
		if(g.dWidth > g.sWidth || g.dHeight > g.sHeight)
		{
			TVPThrowExceptionMessage(TJS_W(
				"stretchCopyAA does not support enlargement: destination "
				"rectangle must not be larger than source rectangle."));
			return TJS_E_FAIL;
		}
		return TJS_S_OK;
	}

	static tjs_error TJS_INTF_METHOD stretchCopyAA(tTJSVariant *result,
	                                               tjs_int numparams,
	                                               tTJSVariant **param,
	                                               iTJSDispatch2 *objthis)
	{
		tjs_uint64 tick = 0;
		tick = TVPGetTickCount(); // 开始时刻

		CopyGeometry g{};
		const tjs_error vr = ValidateParams(numparams, param, g);
		if(vr != TJS_S_OK) return vr;

		iTJSDispatch2 *srcobj = param[4]->AsObjectNoAddRef();

		// 目标层可写布局。经 Layer 类对象读取属性（见 PluginSafety.h 对
		// KAGEX3 等游戏侧子类覆写 imageWidth 的说明）。
		const auto dstView = pluginSafety::LayerWriteView::create(objthis);
		if(!dstView)
		{
			LogPluginError("destination layer has no writable image");
			return TJS_E_INVALIDPARAM;
		}
		const auto srcView = pluginSafety::LayerReadView::create(srcobj);
		if(!srcView)
		{
			LogPluginError("source layer has no readable image");
			return TJS_E_INVALIDPARAM;
		}

		ClipToImages(g, dstView.value.width(), dstView.value.height(),
		             srcView.value.width(), srcView.value.height());

		// 防御自证：裁剪后的矩形必须完整落在各自 image 内（数学上由
		// ClipToImages 与入口检查保证，此检查把违约定为显式错误而不是
		// 越界访问）
		if(g.dWidth > 0 && g.dHeight > 0 && g.sWidth > 0 && g.sHeight > 0)
		{
			if(g.dLeft + g.dWidth > dstView.value.width() ||
			   g.dTop + g.dHeight > dstView.value.height() ||
			   g.sLeft + g.sWidth > srcView.value.width() ||
			   g.sTop + g.sHeight > srcView.value.height())
			{
				LogPluginError(
					"stretchCopyAA: clipped rectangle exceeds image bounds");
				return TJS_E_FAIL;
			}
			CopyAreaAverage(dstView.value.pixels(), dstView.value.pitchBytes(),
			                srcView.value.pixels(), srcView.value.pitchBytes(),
			                g, srcView.value.width(), srcView.value.height());
		}

		// 通知区域更新（上游无条件调用，传裁剪后矩形）
		{
			tTJSVariant val[4];
			tTJSVariant *pval[4] = { val, val + 1, val + 2, val + 3 };
			val[0] = static_cast<tjs_int64>(g.dLeft);
			val[1] = static_cast<tjs_int64>(g.dTop);
			val[2] = static_cast<tjs_int64>(g.dWidth);
			val[3] = static_cast<tjs_int64>(g.dHeight);
			static tjs_uint32 update_hint = 0;
			objthis->FuncCall(0, TJS_W("update"), &update_hint, NULL, 4, pval,
			                  objthis);
		}

		// 上游每次调用经 TVPAddLog 记录耗时；成功路径降为 debug，
		// 避免游戏运行日志噪音
		if(auto logger = spdlog::get("plugin"))
			logger->debug("[layerExAreaAverage] stretch copy by area average:"
			              "RESULT ({},{})->({},{}), time = {}(ms)",
			              g.sWidth, g.sHeight, g.dWidth, g.dHeight,
			              TVPGetTickCount() - tick);

		return TJS_S_OK;
	}
};

NCB_ATTACH_FUNCTION(stretchCopyAA, Layer, LayerExAreaAverage::stretchCopyAA);
