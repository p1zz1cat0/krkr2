// layerExRaster.dll
//
// 移植自 krkrz/krkr2 上游：
//   kirikiri2/trunk/kirikiri2/src/plugins/win32/layerExRaster/main.cpp
// 作者わたなべごう，许可证遵循吉里吉里本体（KrKr2）。
//
// 为 Layer 添加光栅处理风复制命令：
//   Layer.copyRaster(layer, maxh, lines, cycle, time)
//   把指定图层的图像按正弦波逐行位移复制到自身（用于擦除/波纹等演出）。

#include "ncbind.hpp"
#include <math.h>

#include "layerExBase.hpp"

#define NCB_MODULE_NAME TJS_W("layerExRaster.dll")

// 链接锚点：注册型插件是聚合库 PRIVATE source，无外部引用会被链接器剥离
extern "C" void TVPLayerExRasterPluginAnchor() {}

// 最小 startup.tjs 环境下 Layer 类尚未创建，NCB_ATTACH_CLASS_WITH_HOOK 会在
// 目标类不存在时静默跳过注册。在 pre-regist 阶段解析 Layer，保证
// Plugins.link("layerExRaster.dll") 在游戏创建第一个窗口前后都能生效。
void EnsureLayerClassRegistered() {
    tTJSVariant layerClass;
    TVPExecuteExpression(TJS_W("Layer"), &layerClass);
    if(layerClass.Type() != tvtObject)
        TVPThrowExceptionMessage(
            TJS_W("layerExRaster.dll requires the Layer class."));
}

NCB_PRE_REGIST_CALLBACK(EnsureLayerClassRegistered);

/**
 * 光栅处理风复制（继承 layerExBase 的缓冲/裁剪缓存）
 */
struct layerExRaster : public layerExBase
{
public:
	// 构造函数
	layerExRaster(DispatchT obj) : layerExBase(obj) {}

	/**
	 * 光栅复制命令
	 * @param layer 绘制源图层
	 * @param maxh  最大振幅(pixel)
	 * @param lines 一个周期内的扫描线数
	 * @param cycle 周期指定(msec)
	 * @param time  当前时间
	 */
	void copyRaster(tTJSVariant layer, int maxh, int lines, int cycle, tjs_int64 time) {

		// 读取源图层图像信息
		tjs_int width, height, pitch;
		unsigned char* buffer;
		{
			iTJSDispatch2 *layerobj = layer.AsObjectNoAddRef();
			tTJSVariant var;
			layerobj->PropGet(0, TJS_W("imageWidth"), NULL, &var, layerobj);
			width = (tjs_int)var;
			layerobj->PropGet(0, TJS_W("imageHeight"), NULL, &var, layerobj);
			height = (tjs_int)var;
			layerobj->PropGet(0, TJS_W("mainImageBuffer"), NULL, &var, layerobj);
			// 上游为 32 位 Windows 直接截断指针；AsInteger 返回 64 位，arm64 安全
			buffer = reinterpret_cast<unsigned char*>(var.AsInteger());
			layerobj->PropGet(0, TJS_W("mainImageBufferPitch"), NULL, &var, layerobj);
			pitch = (tjs_int)var;
		}

		if (_width != width || _height != height) {
			return;
		}

		// 角速度计算
		double omega = 2 * M_PI / lines;

		//double tt = sin((3.14159265358979/2.0) * time / cycle);
		//tjs_int CurH = (tjs_int)(tt * maxh);
		tjs_int CurH = (tjs_int)maxh;

		// 当前周期相位
		double rad = - omega * time / cycle * (height/2);

		// 裁剪起点对齐
		rad += omega * _clipTop;
		_buffer += _pitch * _clipTop + _clipLeft * 4;
		buffer  +=  pitch * _clipTop + _clipLeft * 4;

		// 逐扫描线处理
		tjs_int n;
		for (n = 0; n < _clipHeight; n++, rad += omega) {
			tjs_int d = (tjs_int)(sin(rad) * CurH);
			if (d >= 0) {
				int w = _clipWidth - d;
				const tjs_uint32 *src = (const tjs_uint32*)(buffer + n * pitch);
				tjs_uint32 *dest = (tjs_uint32 *)(_buffer + n * _pitch) + d;
				for (tjs_int i=0;i<w;i++) {
					*dest++ = *src++;
				}
			} else {
				int w = _clipWidth + d;
				const tjs_uint32 *src = (const tjs_uint32*)(buffer + n * pitch) - d;
				tjs_uint32 *dest = (tjs_uint32 *)(_buffer + n * _pitch);
				for (tjs_int i=0;i<w;i++) {
					*dest++ = *src++;
				}
			}
		}

		redraw();
	}
};

// ----------------------------------- 类的注册

NCB_GET_INSTANCE_HOOK(layerExRaster)
{
	// 实例获取器
	NCB_INSTANCE_GETTER(objthis) { // objthis 为 iTJSDispatch2* 类型的上下文
		ClassT* obj = GetNativeInstance(objthis);	// 获取原生实例指针
		if (!obj) {
			obj = new ClassT(objthis);				// 不存在则创建
			SetNativeInstance(objthis, obj);		// 在 objthis 上注册为原生实例
		}
		obj->reset();
		return obj;
	}
	// 析构器（在真正的方法被调用后调用）
	~NCB_GET_INSTANCE_HOOK_CLASS () {
	}
};


// 附加到 Layer 类
NCB_ATTACH_CLASS_WITH_HOOK(layerExRaster, Layer) {
	NCB_METHOD(copyRaster);
}
