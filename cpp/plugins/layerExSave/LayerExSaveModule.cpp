// layerExSave.dll
//
// 移植自 wtnbgo/layerExSave（作者 わたなべごう）：
//   https://github.com/wtnbgo/layerExSave （commit 83b0ef7）
// 插件本体许可证遵循吉里吉里本体（KrKr2）；捆绑的 LodePNG 为 zlib
// license（thirdparty/lodepng/，上游已 namespace 化为 lexsave）。
//
// 为 Layer/Window/Math 增加 TLG5/PNG 保存与图层比对辅助方法：
//   - Window.startSaveLayerImage(layer, filename[, tags]) / cancel /
//     stop：异步保存（按扩展名 .png 分派 PNG，其余 TLG5），进度与完成
//     经 onSaveLayerImageProgress / onSaveLayerImageDone 回调通知；
//   - Layer.saveLayerImageTlg5(filename[, tags])：TLG5 保存（可选
//     TLG0.0 sds + tags chunk）；
//   - Layer.saveLayerImagePng(filename[, tags]) / saveLayerImagePngOctet
//     ([level|tags])：LodePNG 保存（pHYs/oFFs/vpAg 附加 chunk、comp_lv
//     压缩级别；octet 形式返回 PNG 字节流）；
//   - Layer.saveProvinceImage(filename)：province 按 256 调色板 PNG 保存；
//   - Layer.getCropRect / getCropRectZero / getDiffRect / getDiffPixel /
//     oozeColor / copyBlueToAlpha / isBlank / clearAlpha /
//     getAverageColor / getFingerPrintValue / getShrinkVectorOctet 与
//     Math.octetVectorSum：上游 utils.cpp 附带的图层辅助函数。
//
// 与上游的差异（行为语义保持，见各实现文件头注释）：
//   - 缓冲获取经 PluginSafety 的 LayerReadView/LayerWriteView（上游直接
//     解引用 mainImageBuffer 裸指针；本 fork 拒绝负 pitch 并校验几何）；
//   - 像素位序换成本 fork 的 A<<24|B<<16|G<<8|R（RGBA 内存序，低字节
//     R）。TLG5 文件通道序显式写 B,G,R,A（与 fork 解码端对称、跨引擎
//     可读）；PNG 明文本就是 R,G,B,A，直写；指纹/缩略向量按 B,G,R,A
//     语义序输出，与上游 Windows 版数值一致；
//   - Window 异步保存改为无线程的主线程分片模型（上游 detached 线程 +
//     窗口消息在 fork 上不可用），handler 为进程内全局递增整数；
//   - 上游 svh=0 的除零、全透明层解引用等未定义行为显式拒绝；
//   - 滑动压缩器使用 core 的 SlideCompressor，不捆绑上游 tlg5/slide.cpp。

#include "ncbind.hpp"

#include "LayerExSaveAsync.h"
#include "LayerExSaveEncoders.h"
#include "LayerExSaveUtils.h"
#include "common/PluginSafety.h"

#include "EventIntf.h"
#include "MsgIntf.h"
#include "ScriptMgnIntf.h"
#include "StorageIntf.h"

#include <spdlog/spdlog.h>

#include <zlib.h>

#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#define NCB_MODULE_NAME TJS_W("layerExSave.dll")

// 上游为 Windows 调用约定声明；本工程无此宏时保持为空
#ifndef TJS_INTF_METHOD
#define TJS_INTF_METHOD
#endif

namespace {
void LogPluginError(const char *msg) {
    if(auto logger = spdlog::get("plugin"))
        logger->error("[layerExSave] {}", msg);
}

// Native classes are registered lazily in a minimal startup.tjs environment.
// NCB_ATTACH_FUNCTION silently skips registration when its target class does
// not exist yet, while the module is still marked as loaded. Resolve the
// target classes in the pre-registration phase so
// Plugins.link("layerExSave.dll") works before a game creates its first
// Window/Layer as well as after normal KAG startup.
void EnsureTargetClassesRegistered() {
    static const tjs_char *kClasses[] = {TJS_W("Layer"), TJS_W("Window"),
                                         TJS_W("Math")};
    for(const tjs_char *name : kClasses) {
        tTJSVariant value;
        TVPExecuteExpression(name, &value);
        if(value.Type() != tvtObject)
            TVPThrowExceptionMessage(
                TJS_W("layerExSave.dll requires the Layer/Window/Math "
                      "classes."));
    }
}
} // namespace

NCB_PRE_REGIST_CALLBACK(EnsureTargetClassesRegistered);

// Registration-only translation units in static libraries can otherwise be
// discarded by the linker. PluginImpl.cpp references this anchor explicitly.
extern "C" void TVPLayerExSavePluginAnchor() {}

namespace {

// ---------------------------------------------------------------------------
// 视图与像素辅助
// ---------------------------------------------------------------------------

// 主图像只读源（同步路径直接指向 Layer 缓冲，仅在本调用内使用）。
bool MakeImageSource(iTJSDispatch2 *layer, LayerExSave::ImageSource &src) {
    const auto view = pluginSafety::LayerReadView::create(layer);
    if(!view)
        return false;
    src.pixels = view.value.pixels();
    src.width = view.value.width();
    src.height = view.value.height();
    src.pitchBytes = view.value.pitchBytes();
    return true;
}

// 主图像可写源（写函数的目标；与 ImageSource 分开以保持读指针 const）。
struct MutableSource {
    tjs_uint8 *pixels = nullptr;
    tjs_int width = 0, height = 0, pitchBytes = 0;
};

bool MakeWriteImageSource(iTJSDispatch2 *layer, MutableSource &src) {
    const auto view = pluginSafety::LayerWriteView::create(layer);
    if(!view)
        return false;
    src.pixels = view.value.pixels();
    src.width = view.value.width();
    src.height = view.value.height();
    src.pitchBytes = view.value.pitchBytes();
    return true;
}

// Layer 类对象（从全局解析；KAGEX3 等子类覆盖 imageWidth 的情况与
// PluginSafety 的 LayerReadView 相同，经类对象 PropGet 规避）。
iTJSDispatch2 *QueryLayerClass() {
    try {
        tTJS *engine = TVPGetScriptEngine();
        if(!engine)
            return nullptr;
        iTJSDispatch2 *global = engine->GetGlobalNoAddRef();
        if(!global)
            return nullptr;
        tTJSVariant value;
        if(TJS_FAILED(global->PropGet(0, TJS_W("Layer"), nullptr, &value,
                                      global)))
            return nullptr;
        return value.AsObjectNoAddRef();
    } catch(...) {
        return nullptr;
    }
}

// province 缓冲（1 字节/像素）没有 pluginSafety 视图，本地读取属性并
// 校验几何（负 pitch / 越界 span 拒绝）。
bool MakeProvinceSource(iTJSDispatch2 *layer, const tjs_uint8 *&pixels,
                        tjs_int &width, tjs_int &height,
                        tjs_int &pitchBytes) {
    if(!layer ||
       TJS_FAILED(layer->IsInstanceOf(0, nullptr, nullptr, TJS_W("Layer"),
                                      layer)))
        return false;
    iTJSDispatch2 *layerClass = QueryLayerClass();
    if(!layerClass)
        return false;
    const auto readInt = [&](const tjs_char *name,
                             tjs_int &out) -> bool {
        tTJSVariant value;
        if(TJS_FAILED(layerClass->PropGet(0, name, nullptr, &value, layer)))
            return false;
        if(value.Type() != tvtInteger)
            return false;
        const tjs_int64 v = value;
        if(v < 0 || v > std::numeric_limits<tjs_int>::max())
            return false;
        out = static_cast<tjs_int>(v);
        return true;
    };
    tjs_intptr_t buffer = 0;
    const auto readPointer = [&](const tjs_char *name) -> bool {
        tTJSVariant value;
        if(TJS_FAILED(layerClass->PropGet(0, name, nullptr, &value, layer)))
            return false;
        if(value.Type() != tvtInteger)
            return false;
        buffer = static_cast<tjs_intptr_t>(static_cast<tjs_int64>(value));
        return buffer != 0;
    };
    if(!readInt(TJS_W("imageWidth"), width) ||
       !readInt(TJS_W("imageHeight"), height) ||
       !readInt(TJS_W("provinceImageBufferPitch"), pitchBytes) ||
       !readPointer(TJS_W("provinceImageBuffer")))
        return false;
    if(width <= 0 || height <= 0 || pitchBytes < width)
        return false;
    const tjs_int64 span =
        static_cast<tjs_int64>(height - 1) * pitchBytes + width;
    if(span > static_cast<tjs_int64>(pluginSafety::kDefaultOperationLimit))
        return false;
    pixels = reinterpret_cast<const tjs_uint8 *>(buffer);
    return true;
}

// 异步保存用：主线程一次性拷贝为紧凑缓冲（编码期间不持有 Layer）。
std::vector<tjs_uint8> CopyCompactPixels(const LayerExSave::ImageSource &src) {
    std::vector<tjs_uint8> out(static_cast<std::size_t>(src.width) *
                               src.height * 4);
    for(tjs_int y = 0; y < src.height; y++)
        std::memcpy(out.data() +
                        static_cast<std::size_t>(y) * src.width * 4,
                    src.row(y), static_cast<std::size_t>(src.width) * 4);
    return out;
}

void WriteStream(const ttstr &filename, const std::vector<tjs_uint8> &data) {
    // 本树 tTJSBinaryStream 是普通 C++ 类（虚析构），用 delete 替代上游
    // 插件的 out->Destruct()
    tTJSBinaryStream *out = TVPCreateStream(filename, TJS_BS_WRITE);
    if(!out)
        TVPThrowExceptionMessage(TJS_W("%1:can't open"), filename);
    try {
        out->Write(data.data(), static_cast<tjs_uint>(data.size()));
    } catch(...) {
        delete out;
        throw;
    }
    delete out;
}

// ---------------------------------------------------------------------------
// tags 序列化（主线程完成，编码器只接触纯数据）
// ---------------------------------------------------------------------------

// 上游 TagsCaller：TLG5 tags chunk 的序列化格式
// "len:name=len:value,..."（跳过 hidden 成员）。
class TagsSerializer final : public tTJSDispatch {
public:
    explicit TagsSerializer(ttstr &store) : store_(store) {}

    tjs_error TJS_INTF_METHOD
    FuncCall(tjs_uint32 flag, const tjs_char *membername, tjs_uint32 *hint,
             tTJSVariant *result, tjs_int numparams, tTJSVariant **param,
             iTJSDispatch2 *objthis) override {
        (void)flag;
        (void)membername;
        (void)hint;
        (void)objthis;
        if(numparams > 1) {
            const tTVInteger flags = param[1]->AsInteger();
            if(!(flags & TJS_HIDDENMEMBER)) {
                const ttstr name = *param[0];
                const ttstr value = *param[2];
                store_ += ttstr(name.GetNarrowStrLen()) + TJS_W(":") + name +
                          TJS_W("=") +
                          ttstr(value.GetNarrowStrLen()) + TJS_W(":") + value +
                          TJS_W(",");
            }
        }
        if(result)
            *result = true;
        return TJS_S_OK;
    }

private:
    ttstr &store_;
};

ttstr SerializeTlg5Tags(iTJSDispatch2 *dict) {
    ttstr tags;
    if(!dict)
        return tags;
    TagsSerializer serializer(tags);
    tTJSVariantClosure closure(&serializer);
    dict->EnumMembers(TJS_IGNOREPROP, &closure, dict);
    return tags;
}

// 上游 ChunkSetResoXY / ChunkSetOffsXY / ChunkSetVpagWH / comp_lv 的
// 字典读取（PngTagParams 纯数据化）。
void ReadPngTags(iTJSDispatch2 *dict, LayerExSave::PngTagParams &out) {
    ncbPropAccessor dic(dict);
    if(dic.HasValue(TJS_W("reso_x")) || dic.HasValue(TJS_W("reso_y"))) {
        out.hasReso = true;
        out.resoX = static_cast<tjs_int>(dic.getIntValue(TJS_W("reso_x")));
        out.resoY = static_cast<tjs_int>(dic.getIntValue(TJS_W("reso_y")));
        out.resoMeter = dic.getStrValue(TJS_W("reso_unit")) ==
                        ttstr(TJS_W("meter"));
    }
    if(dic.HasValue(TJS_W("offs_x")) || dic.HasValue(TJS_W("offs_y"))) {
        out.hasOffs = true;
        out.offsX = static_cast<tjs_int>(dic.getIntValue(TJS_W("offs_x")));
        out.offsY = static_cast<tjs_int>(dic.getIntValue(TJS_W("offs_y")));
        out.offsMicrometer = dic.getStrValue(TJS_W("offs_unit")) ==
                             ttstr(TJS_W("micrometer"));
    }
    if(dic.HasValue(TJS_W("vpag_w")) || dic.HasValue(TJS_W("vpag_h"))) {
        out.hasVpag = true;
        out.vpagW = static_cast<tjs_int>(dic.getIntValue(TJS_W("vpag_w")));
        out.vpagH = static_cast<tjs_int>(dic.getIntValue(TJS_W("vpag_h")));
        out.vpagMicrometer = dic.getStrValue(TJS_W("vpag_unit")) ==
                             ttstr(TJS_W("micrometer"));
    }
    if(dic.HasValue(TJS_W("comp_lv"))) {
        out.hasCompLv = true;
        out.compLv = static_cast<tjs_int>(
            dic.getIntValue(TJS_W("comp_lv"), Z_DEFAULT_COMPRESSION));
    }
}

// param 形参中可选的 tags 字典（void / 缺省时为 null）。
iTJSDispatch2 *OptionalTagsParam(tjs_int numparams, tTJSVariant **param,
                                 tjs_int index) {
    if(numparams <= index || param[index]->Type() != tvtObject)
        return nullptr;
    return param[index]->AsObjectNoAddRef();
}

// ---------------------------------------------------------------------------
// Window 异步保存
// ---------------------------------------------------------------------------

// Window.startSaveLayerImage(layer, filename[, tags]) → handler
tjs_error TJS_INTF_METHOD
StartSaveLayerImageFunc(tTJSVariant *result, tjs_int numparams,
                        tTJSVariant **param, iTJSDispatch2 *objthis) {
    if(numparams < 2)
        return TJS_E_BADPARAMCOUNT;
    iTJSDispatch2 *layer = param[0]->AsObjectNoAddRef();
    const ttstr filename = static_cast<ttstr>(*param[1]);

    LayerExSave::ImageSource src;
    if(!MakeImageSource(layer, src))
        TVPThrowExceptionMessage(TJS_W("%1:invalid layer"), filename);

    // 上游 SaveInfo::start：.png → PNG，其余 → TLG5
    ttstr ext(TVPExtractStorageExt(filename));
    ext.ToLowerCase();

    std::vector<tjs_uint8> pixels = CopyCompactPixels(src);
    LayerExSave::ImageSource compactSrc{pixels.data(), src.width, src.height,
                                        src.width * 4};
    std::unique_ptr<LayerExSave::ChunkedEncoder> encoder;
    if(ext == TJS_W(".png")) {
        LayerExSave::PngTagParams tags;
        iTJSDispatch2 *dict = OptionalTagsParam(numparams, param, 2);
        if(dict)
            ReadPngTags(dict, tags);
        encoder = LayerExSave::CreatePngChunkEncoder(compactSrc, tags);
    } else {
        const ttstr tagsText =
            SerializeTlg5Tags(OptionalTagsParam(numparams, param, 2));
        encoder = LayerExSave::CreateTlg5Encoder(compactSrc, tagsText);
    }

    const tjs_int64 handler = LayerExSave::SaveManager::Instance().Start(
        objthis, layer, filename, std::move(pixels), compactSrc,
        std::move(encoder));
    if(result)
        *result = static_cast<tjs_int>(handler);
    return TJS_S_OK;
}

// Window.cancelSaveLayerImage(handler)
tjs_error TJS_INTF_METHOD
CancelSaveLayerImageFunc(tTJSVariant *result, tjs_int numparams,
                         tTJSVariant **param, iTJSDispatch2 *objthis) {
    (void)result;
    (void)objthis;
    if(numparams < 1)
        return TJS_E_BADPARAMCOUNT;
    LayerExSave::SaveManager::Instance().Cancel(
        static_cast<tjs_int64>(param[0]->AsInteger()));
    return TJS_S_OK;
}

// Window.stopSaveLayerImage(handler)
tjs_error TJS_INTF_METHOD
StopSaveLayerImageFunc(tTJSVariant *result, tjs_int numparams,
                       tTJSVariant **param, iTJSDispatch2 *objthis) {
    (void)result;
    (void)objthis;
    if(numparams < 1)
        return TJS_E_BADPARAMCOUNT;
    LayerExSave::SaveManager::Instance().Stop(
        static_cast<tjs_int64>(param[0]->AsInteger()));
    return TJS_S_OK;
}

// ---------------------------------------------------------------------------
// Layer 同步保存
// ---------------------------------------------------------------------------

// Layer.saveLayerImageTlg5(filename[, tags])
tjs_error TJS_INTF_METHOD
SaveLayerImageTlg5Func(tTJSVariant *result, tjs_int numparams,
                       tTJSVariant **param, iTJSDispatch2 *objthis) {
    (void)result;
    if(numparams < 1)
        return TJS_E_BADPARAMCOUNT;
    const ttstr filename = static_cast<ttstr>(*param[0]);
    LayerExSave::ImageSource src;
    if(!MakeImageSource(objthis, src))
        TVPThrowExceptionMessage(TJS_W("%1:invalid layer"), filename);
    const ttstr tagsText =
        SerializeTlg5Tags(OptionalTagsParam(numparams, param, 1));
    auto encoder = LayerExSave::CreateTlg5Encoder(src, tagsText);
    while(encoder->step()) {
    }
    WriteStream(filename, encoder->data());
    return TJS_S_OK;
}

// Layer.saveLayerImagePng(filename[, tags])（LodePNG 路径；上游在无图或
// 编码失败时静默不写文件，此处保持该语义并记录日志）
tjs_error TJS_INTF_METHOD
SaveLayerImagePngFunc(tTJSVariant *result, tjs_int numparams,
                      tTJSVariant **param, iTJSDispatch2 *objthis) {
    (void)result;
    if(numparams < 1)
        return TJS_E_BADPARAMCOUNT;
    const ttstr filename = static_cast<ttstr>(*param[0]);
    LayerExSave::ImageSource src;
    if(!MakeImageSource(objthis, src))
        return TJS_S_OK; // 上游 MakeVectorImage 失败为静默 no-op
    LayerExSave::PngTagParams tags;
    iTJSDispatch2 *dict = OptionalTagsParam(numparams, param, 1);
    if(dict)
        ReadPngTags(dict, tags);
    std::vector<tjs_uint8> png;
    if(!LayerExSave::EncodeWithLodePNG(src, &tags, png)) {
        LogPluginError("PNG encoding failed; file not written");
        return TJS_S_OK;
    }
    WriteStream(filename, png);
    return TJS_S_OK;
}

// Layer.saveLayerImagePngOctet([level|tags]) → octet（失败时空串，上游
// 语义；manual.tjs 描述的"省略时 1"是 TJS 包装层的默认值，本壳的省略
// 等价于上游 C++ 层的 NULL = lodepng 默认压缩）
tjs_error TJS_INTF_METHOD
SaveLayerImagePngOctetFunc(tTJSVariant *result, tjs_int numparams,
                           tTJSVariant **param, iTJSDispatch2 *objthis) {
    if(!result)
        return TJS_S_OK;
    LayerExSave::ImageSource src;
    if(!MakeImageSource(objthis, src)) {
        *result = TJS_W("");
        return TJS_S_OK;
    }
    // 参数形态与上游 EncodeLodePNGCommon 一致：字典读 tags，整数读压缩级别
    LayerExSave::PngTagParams tags;
    const LayerExSave::PngTagParams *tagsPtr = nullptr;
    if(numparams >= 1 && param[0]->Type() == tvtObject) {
        ReadPngTags(param[0]->AsObjectNoAddRef(), tags);
        tagsPtr = &tags;
    }
    std::vector<tjs_uint8> png;
    if(!LayerExSave::EncodeWithLodePNG(src, tagsPtr, png)) {
        *result = TJS_W("");
        return TJS_S_OK;
    }
    tTJSVariantOctet *oct = TJSAllocVariantOctet(png.data(),
                                                 static_cast<tjs_uint>(png.size()));
    *result = oct;
    oct->Release();
    return TJS_S_OK;
}

// Layer.saveProvinceImage(filename)（256 调色板 PNG；province 不存在时
// 抛异常，上游 LodePNG 模式语义）
tjs_error TJS_INTF_METHOD
SaveProvinceImageFunc(tTJSVariant *result, tjs_int numparams,
                      tTJSVariant **param, iTJSDispatch2 *objthis) {
    (void)result;
    if(numparams < 1)
        return TJS_E_BADPARAMCOUNT;
    const tjs_uint8 *pixels = nullptr;
    tjs_int width = 0, height = 0, pitchBytes = 0;
    if(!MakeProvinceSource(objthis, pixels, width, height, pitchBytes))
        TVPThrowExceptionMessage(TJS_W("no province image"));
    std::vector<tjs_uint8> png;
    if(!LayerExSave::EncodeProvinceWithLodePNG(pixels, width, height,
                                               pitchBytes, png))
        return TJS_E_NOTIMPL; // 上游编码失败路径
    WriteStream(static_cast<ttstr>(*param[0]), png);
    return TJS_S_OK;
}

// ---------------------------------------------------------------------------
// Layer 杂项函数（上游 utils.cpp）
// ---------------------------------------------------------------------------

// 结果字典 %[x, y, w, h]。
void MakeCropResult(tTJSVariant *result, const LayerExSave::CropResult &crop) {
    ncbDictionaryAccessor dict;
    dict.SetValue(TJS_W("x"), crop.x);
    dict.SetValue(TJS_W("y"), crop.y);
    dict.SetValue(TJS_W("w"), crop.w);
    dict.SetValue(TJS_W("h"), crop.h);
    *result = dict;
}

// Layer.getCropRect() / getCropRectZero()
template <bool kZeroMode>
tjs_error TJS_INTF_METHOD
GetCropRectFunc(tTJSVariant *result, tjs_int numparams, tTJSVariant **param,
                iTJSDispatch2 *objthis) {
    (void)numparams;
    (void)param;
    LayerExSave::ImageSource src;
    if(!MakeImageSource(objthis, src))
        TVPThrowExceptionMessage(TJS_W("Invalid layer image."));
    const LayerExSave::CropResult crop = LayerExSave::CalcCropRect(
        src.pixels, src.width, src.height, src.pitchBytes, kZeroMode);
    result->Clear();
    if(crop.hasCrop)
        MakeCropResult(result, crop);
    return TJS_S_OK;
}

// Layer.getDiffRect(base)
tjs_error TJS_INTF_METHOD
GetDiffRectFunc(tTJSVariant *result, tjs_int numparams, tTJSVariant **param,
                iTJSDispatch2 *objthis) {
    (void)numparams;
    if(numparams < 1)
        return TJS_E_BADPARAMCOUNT;
    LayerExSave::ImageSource to, from;
    if(!MakeImageSource(objthis, to) ||
       !MakeImageSource(param[0]->AsObjectNoAddRef(), from))
        TVPThrowExceptionMessage(TJS_W("Invalid layer image."));
    LayerExSave::CropResult crop;
    if(!LayerExSave::CalcDiffRect(from.pixels, from.width, from.height,
                                  from.pitchBytes, to.pixels, to.width,
                                  to.height, to.pitchBytes, crop))
        TVPThrowExceptionMessage(TJS_W("Different layer size."));
    result->Clear();
    if(crop.hasCrop)
        MakeCropResult(result, crop);
    return TJS_S_OK;
}

// Layer.getDiffPixel(base[, samecol, diffcol]) → 差分像素数
tjs_error TJS_INTF_METHOD
GetDiffPixelFunc(tTJSVariant *result, tjs_int numparams, tTJSVariant **param,
                 iTJSDispatch2 *objthis) {
    if(numparams < 1)
        return TJS_E_BADPARAMCOUNT;
    bool sameFill = false, diffFill = false;
    tjs_uint32 sameColor = 0, diffColor = 0;
    if(numparams >= 2 && param[1]->Type() != tvtVoid) {
        sameColor = static_cast<tjs_uint32>(param[1]->AsInteger());
        sameFill = true;
    }
    if(numparams >= 3 && param[2]->Type() != tvtVoid) {
        diffColor = static_cast<tjs_uint32>(param[2]->AsInteger());
        diffFill = true;
    }
    MutableSource to;
    LayerExSave::ImageSource from;
    if(!MakeWriteImageSource(objthis, to) ||
       !MakeImageSource(param[0]->AsObjectNoAddRef(), from))
        TVPThrowExceptionMessage(TJS_W("Invalid layer image."));
    const tjs_int count = LayerExSave::CalcDiffPixel(
        from.pixels, from.width, from.height, from.pitchBytes, to.pixels,
        to.width, to.height, to.pitchBytes, sameFill, sameColor, diffFill,
        diffColor);
    if(result)
        *result = count;
    return TJS_S_OK;
}

// Layer.oozeColor(level[, threshold, fillColor])
tjs_error TJS_INTF_METHOD
OozeColorFunc(tTJSVariant *result, tjs_int numparams, tTJSVariant **param,
              iTJSDispatch2 *objthis) {
    (void)result;
    if(numparams < 1)
        return TJS_E_BADPARAMCOUNT;
    const tjs_int level = static_cast<tjs_int>(param[0]->AsInteger());
    // 上游先做 unsigned char 截断再 clamp（负值变高位值是上游 artifact）
    const tjs_int threshold =
        numparams > 1
            ? static_cast<tjs_int>(static_cast<tjs_uint8>(
                  static_cast<tjs_int64>(param[1]->AsInteger()) & 0xff))
            : 1;
    const tjs_uint32 fillColor =
        numparams > 2
            ? static_cast<tjs_uint32>(static_cast<tjs_int64>(
                  param[2]->AsInteger()))
            : 0;
    MutableSource src;
    if(!MakeWriteImageSource(objthis, src))
        TVPThrowExceptionMessage(TJS_W("Invalid layer image."));
    LayerExSave::OozeColor(src.pixels, src.width, src.height, src.pitchBytes,
                           level, threshold, fillColor);
    return TJS_S_OK;
}

// Layer.copyBlueToAlpha(src)
tjs_error TJS_INTF_METHOD
CopyBlueToAlphaFunc(tTJSVariant *result, tjs_int numparams,
                    tTJSVariant **param, iTJSDispatch2 *objthis) {
    (void)result;
    if(numparams < 1)
        return TJS_E_BADPARAMCOUNT;
    MutableSource dst;
    LayerExSave::ImageSource src;
    if(!MakeWriteImageSource(objthis, dst) ||
       !MakeImageSource(param[0]->AsObjectNoAddRef(), src))
        TVPThrowExceptionMessage(TJS_W("Invalid layer image."));
    LayerExSave::CopyBlueToAlpha(dst.pixels, dst.width, dst.height,
                                 dst.pitchBytes, src.pixels, src.width,
                                 src.height, src.pitchBytes);
    return TJS_S_OK;
}

// Layer.isBlank(x, y, w, h)
tjs_error TJS_INTF_METHOD
IsBlankFunc(tTJSVariant *result, tjs_int numparams, tTJSVariant **param,
            iTJSDispatch2 *objthis) {
    if(numparams < 4)
        return TJS_E_BADPARAMCOUNT;
    LayerExSave::ImageSource src;
    if(!MakeImageSource(objthis, src))
        TVPThrowExceptionMessage(TJS_W("Invalid layer image."));
    const bool blank = LayerExSave::IsBlank(
        src.pixels, src.width, src.height, src.pitchBytes,
        static_cast<tjs_int>(param[0]->AsInteger()),
        static_cast<tjs_int>(param[1]->AsInteger()),
        static_cast<tjs_int>(param[2]->AsInteger()),
        static_cast<tjs_int>(param[3]->AsInteger()));
    if(result)
        *result = blank;
    return TJS_S_OK;
}

// Layer.clearAlpha([threshold, fillColor])
tjs_error TJS_INTF_METHOD
ClearAlphaFunc(tTJSVariant *result, tjs_int numparams, tTJSVariant **param,
               iTJSDispatch2 *objthis) {
    (void)result;
    const tjs_int threshold =
        numparams <= 0
            ? 0
            : static_cast<tjs_int>(static_cast<tjs_int64>(param[0]->AsInteger()));
    const tjs_uint32 fillColor =
        static_cast<tjs_uint32>(
            static_cast<tjs_int64>(numparams > 1 ? param[1]->AsInteger() : 0)) &
        0xffffff;
    MutableSource src;
    if(!MakeWriteImageSource(objthis, src))
        TVPThrowExceptionMessage(TJS_W("Invalid layer image."));
    LayerExSave::ClearAlpha(src.pixels, src.width, src.height, src.pitchBytes,
                            threshold, fillColor);
    return TJS_S_OK;
}

// Layer.getAverageColor(x, y, w, h)
tjs_error TJS_INTF_METHOD
GetAverageColorFunc(tTJSVariant *result, tjs_int numparams,
                    tTJSVariant **param, iTJSDispatch2 *objthis) {
    if(numparams < 4)
        return TJS_E_BADPARAMCOUNT;
    LayerExSave::ImageSource src;
    if(!MakeImageSource(objthis, src))
        TVPThrowExceptionMessage(TJS_W("Invalid layer image."));
    const tjs_uint32 color = LayerExSave::CalcAverageColor(
        src.pixels, src.width, src.height, src.pitchBytes,
        static_cast<tjs_int>(param[0]->AsInteger()),
        static_cast<tjs_int>(param[1]->AsInteger()),
        static_cast<tjs_int>(param[2]->AsInteger()),
        static_cast<tjs_int>(param[3]->AsInteger()));
    if(result)
        *result = static_cast<tjs_int>(color);
    return TJS_S_OK;
}

// Layer.getFingerPrintValue([ignore_transparent])
tjs_error TJS_INTF_METHOD
GetFingerPrintValueFunc(tTJSVariant *result, tjs_int numparams,
                        tTJSVariant **param, iTJSDispatch2 *objthis) {
    const bool ignoreTransparent =
        (numparams > 0 && param[0]->Type() != tvtVoid)
            ? param[0]->operator bool()
            : true;
    LayerExSave::ImageSource src;
    if(!MakeImageSource(objthis, src))
        TVPThrowExceptionMessage(TJS_W("Invalid layer image."));
    if(result)
        *result = static_cast<tTVInteger>(LayerExSave::FingerprintValue(
            src.pixels, src.width, src.height, src.pitchBytes,
            ignoreTransparent));
    return TJS_S_OK;
}

// Layer.getShrinkVectorOctet([w, h]) → octet
tjs_error TJS_INTF_METHOD
GetShrinkVectorOctetFunc(tTJSVariant *result, tjs_int numparams,
                         tTJSVariant **param, iTJSDispatch2 *objthis) {
    const tjs_int shrinkW =
        (numparams > 0 && param[0]->Type() != tvtVoid)
            ? static_cast<tjs_int>(param[0]->AsInteger())
            : 16;
    const tjs_int shrinkH =
        (numparams > 1 && param[1]->Type() != tvtVoid)
            ? static_cast<tjs_int>(param[1]->AsInteger())
            : 16;
    LayerExSave::ImageSource src;
    if(!MakeImageSource(objthis, src))
        TVPThrowExceptionMessage(TJS_W("Invalid layer image."));
    const std::vector<tjs_uint32> vector =
        LayerExSave::ShrinkVector(src.pixels, src.width, src.height,
                                  src.pitchBytes, shrinkW, shrinkH);
    if(result) {
        tTJSVariantOctet *oct = TJSAllocVariantOctet(
            reinterpret_cast<const tjs_uint8 *>(vector.data()),
            static_cast<tjs_uint>(vector.size() * sizeof(tjs_uint32)));
        *result = oct;
        oct->Release();
    }
    return TJS_S_OK;
}

// Math.octetVectorSum(a[, b]) → [Σab, Σ(a-b)^2, Σa^2, Σb^2, Σa, Σb, Σdiff]
tjs_error TJS_INTF_METHOD
OctetVectorSumFunc(tTJSVariant *result, tjs_int numparams,
                   tTJSVariant **param, iTJSDispatch2 *objthis) {
    (void)objthis;
    if(numparams < 2)
        return TJS_E_BADPARAMCOUNT;
    if(param[0]->Type() != tvtOctet)
        return TJS_E_INVALIDPARAM;
    tTJSVariantOctet *oct1 = param[0]->AsOctetNoAddRef();
    tTJSVariantOctet *oct2 =
        (param[1]->Type() == tvtOctet) ? param[1]->AsOctetNoAddRef() : nullptr;
    if(!oct1 || (oct2 && oct1->GetLength() != oct2->GetLength()))
        return TJS_E_INVALIDPARAM;

    LayerExSave::VectorSumWork work;
    if(!LayerExSave::OctetVectorSum(
           oct1->GetData(), oct1->GetLength(),
           oct2 ? oct2->GetData() : nullptr,
           oct2 ? oct2->GetLength() : 0, work))
        return TJS_E_INVALIDPARAM;

    if(result) {
        iTJSDispatch2 *arr = TJSCreateArrayObject();
        if(!arr)
            return TJS_E_FAIL;
        tTJSVariant v;
        const tjs_int64 values[7] = {work.csum, work.dsum, work.psum1,
                                     work.psum2, work.nsum1, work.nsum2,
                                     static_cast<tjs_int64>(work.diff)};
        for(tjs_int i = 0; i < 7; i++) {
            v = static_cast<tTVInteger>(values[i]);
            arr->PropSetByNum(TJS_MEMBERENSURE, i, &v, arr);
        }
        *result = tTJSVariant(arr, arr);
        arr->Release();
    }
    return TJS_S_OK;
}

} // namespace

// ---------------------------------------------------------------------------
// 注册
// ---------------------------------------------------------------------------

NCB_ATTACH_FUNCTION(startSaveLayerImage, Window, StartSaveLayerImageFunc);
NCB_ATTACH_FUNCTION(cancelSaveLayerImage, Window, CancelSaveLayerImageFunc);
NCB_ATTACH_FUNCTION(stopSaveLayerImage, Window, StopSaveLayerImageFunc);

NCB_ATTACH_FUNCTION(saveLayerImageTlg5, Layer, SaveLayerImageTlg5Func);
NCB_ATTACH_FUNCTION(saveLayerImagePng, Layer, SaveLayerImagePngFunc);
NCB_ATTACH_FUNCTION(saveLayerImagePngOctet, Layer, SaveLayerImagePngOctetFunc);
NCB_ATTACH_FUNCTION(saveProvinceImage, Layer, SaveProvinceImageFunc);

NCB_ATTACH_FUNCTION(getCropRect, Layer, GetCropRectFunc<false>);
NCB_ATTACH_FUNCTION(getCropRectZero, Layer, GetCropRectFunc<true>);
NCB_ATTACH_FUNCTION(getDiffRect, Layer, GetDiffRectFunc);
NCB_ATTACH_FUNCTION(getDiffPixel, Layer, GetDiffPixelFunc);
NCB_ATTACH_FUNCTION(oozeColor, Layer, OozeColorFunc);
NCB_ATTACH_FUNCTION(copyBlueToAlpha, Layer, CopyBlueToAlphaFunc);
NCB_ATTACH_FUNCTION(isBlank, Layer, IsBlankFunc);
NCB_ATTACH_FUNCTION(clearAlpha, Layer, ClearAlphaFunc);
NCB_ATTACH_FUNCTION(getAverageColor, Layer, GetAverageColorFunc);
NCB_ATTACH_FUNCTION(getFingerPrintValue, Layer, GetFingerPrintValueFunc);
NCB_ATTACH_FUNCTION(getShrinkVectorOctet, Layer, GetShrinkVectorOctetFunc);

NCB_ATTACH_FUNCTION(octetVectorSum, Math, OctetVectorSumFunc);
