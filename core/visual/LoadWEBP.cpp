#include <webp/decode.h>
#include <webp/encode.h>

#include "tjsCommHead.h"

#include "GraphicsLoaderIntf.h"
#include "LayerBitmapIntf.h"
#include "MsgIntf.h"
#include "ScriptMgnIntf.h"
#include "tjsDictionary.h"

#include <algorithm>
#include <cstring>
#include <vector>

// WebP 解码。位图内存是 RGBA 字节序，与 PNG 一致。
// 不支持的 8bit 调色板模式抛异常，灰度模式取 Y 平面。
void TVPLoadWEBP(void *formatdata, void *callbackdata,
                 tTVPGraphicSizeCallback sizecallback,
                 tTVPGraphicScanLineCallback scanlinecallback,
                 tTVPMetaInfoPushCallback metainfopushcallback,
                 tTJSBinaryStream *src, tjs_int keyidx,
                 tTVPGraphicLoadMode mode) {
    WebPDecoderConfig config;
    if(WebPInitDecoderConfig(&config) == 0) {
        TVPThrowExceptionMessage(TJS_W("Invalid WebP image"));
    }

    int datasize = src->GetSize();
    std::unique_ptr<uint8_t[]> data(new uint8_t[datasize]);
    src->ReadBuffer(data.get(), datasize);
    if(WebPGetFeatures(data.get(), datasize, &config.input) != VP8_STATUS_OK) {
        TVPThrowExceptionMessage(TJS_W("Invalid WebP image"));
    }

    unsigned int stride =
        sizecallback(callbackdata, config.input.width, config.input.height,
                     config.input.has_alpha ? gpfRGBA : gpfRGB);
#if 0
	WebPData webp_data = { data, datasize };
	WebPDemuxer* demux = WebPDemux(&webp_data);
	WebPChunkIterator chunk_iter;
	if (WebPDemuxGetChunk(demux, "USER", 1, &chunk_iter)) {
		chunk_iter.chunk.bytes;
		WebPDemuxReleaseChunkIterator(&chunk_iter);
	}

	WebPDemuxDelete(demux);
#endif
    uint8_t *scanline = (uint8_t *)scanlinecallback(callbackdata, 0);
    if(glmNormal == mode) {
        config.output.colorspace = MODE_RGBA;
        config.output.u.RGBA.rgba = scanline;
        config.output.u.RGBA.stride = stride;
        config.output.u.RGBA.size = config.input.height * stride;
        config.output.is_external_memory = 1;
        if(WebPDecode(data.get(), datasize, &config) != VP8_STATUS_OK) {
            TVPThrowExceptionMessage(TJS_W("Invalid WebP image(RGBA mode)"));
        }
    } else if(glmGrayscale == mode) {
        config.output.colorspace = MODE_YUV;
        unsigned int uvSize = config.input.width * config.input.height / 4 +
            config.input.width + config.input.width;
        std::unique_ptr<uint8_t[]> dummy(new uint8_t[uvSize]);
        config.output.u.YUVA.y = scanline;
        config.output.u.YUVA.u = dummy.get();
        config.output.u.YUVA.v = dummy.get();
        config.output.u.YUVA.a = nullptr;
        config.output.u.YUVA.y_stride = stride;
        config.output.u.YUVA.u_stride = config.input.width / 2;
        config.output.u.YUVA.v_stride = config.input.width / 2;
        config.output.u.YUVA.a_stride = 0;
        config.output.u.YUVA.y_size = config.input.height * stride;
        config.output.u.YUVA.u_size = uvSize;
        config.output.u.YUVA.v_size = uvSize;
        config.output.u.YUVA.a_size = 0;
        config.output.is_external_memory = 1;

        if(WebPDecode(data.get(), datasize, &config) != VP8_STATUS_OK) {
            TVPThrowExceptionMessage(
                TJS_W("Invalid WebP image(Grayscale Mode)"));
        }
    } else {
        TVPThrowExceptionMessage(
            TJS_W("WebP does not support palettized image"));
    }
    scanlinecallback(callbackdata, -1); // image was written
}

void TVPLoadHeaderWEBP(void *formatdata, tTJSBinaryStream *src,
                       iTJSDispatch2 **dic) {
    WebPDecoderConfig config;
    if(WebPInitDecoderConfig(&config) == 0) {
        TVPThrowExceptionMessage(TJS_W("Invalid WebP image"));
    }

    int datasize = src->GetSize();
    std::unique_ptr<uint8_t[]> data(new uint8_t[datasize]);
    src->ReadBuffer(data.get(), datasize);
    if(WebPGetFeatures(data.get(), datasize, &config.input) != VP8_STATUS_OK) {
        TVPThrowExceptionMessage(TJS_W("Invalid WebP image"));
    }

    *dic = TJSCreateDictionaryObject();
    tTJSVariant val((tjs_int32)config.input.width);
    (*dic)->PropSet(TJS_MEMBERENSURE, TJS_W("width"), nullptr, &val, (*dic));
    val = tTJSVariant((tjs_int32)config.input.height);
    (*dic)->PropSet(TJS_MEMBERENSURE, TJS_W("height"), nullptr, &val, (*dic));
    val = tTJSVariant(config.input.has_alpha ? 32 : 24);
    (*dic)->PropSet(TJS_MEMBERENSURE, TJS_W("bpp"), nullptr, &val, (*dic));
    val = tTJSVariant(config.input.has_alpha ? 1 : 0);
    (*dic)->PropSet(TJS_MEMBERENSURE, TJS_W("has_alpha"), nullptr, &val,
                    (*dic));
    val = tTJSVariant(config.input.has_animation ? 1 : 0);
    (*dic)->PropSet(TJS_MEMBERENSURE, TJS_W("has_animation"), nullptr, &val,
                    (*dic));
    val = tTJSVariant((tjs_int32)config.input.format);
    (*dic)->PropSet(TJS_MEMBERENSURE, TJS_W("format"), nullptr, &val, (*dic));
}

namespace {

// 保存选项表。键名来自上游 krglhwebp 的 WEBP_OPTION 列表。
// 每项写明边界。超界的值夹取，不抛异常。
struct WebPConfigOption {
    const tjs_char *Name;
    void (*Apply)(WebPConfig &config, double value);
    double Minimum;
    double Maximum;
};

#define WEBP_CONFIG_OPTION(name, field, type, low, high)                       \
    {                                                                          \
        TJS_W(#name),                                                          \
            [](WebPConfig &config, double value) {                             \
                config.field = static_cast<type>(value);                       \
            },                                                                 \
            low, high                                                          \
    }

const WebPConfigOption WebPConfigOptions[] = {
    WEBP_CONFIG_OPTION(lossless, lossless, int, 0, 1),
    WEBP_CONFIG_OPTION(quality, quality, float, 0, 100),
    WEBP_CONFIG_OPTION(method, method, int, 0, 6),
    WEBP_CONFIG_OPTION(image_hint, image_hint, WebPImageHint, 0, 3),
    WEBP_CONFIG_OPTION(target_size, target_size, int, 0, 1 << 30),
    WEBP_CONFIG_OPTION(target_PSNR, target_PSNR, float, 0, 100),
    WEBP_CONFIG_OPTION(segments, segments, int, 1, 4),
    WEBP_CONFIG_OPTION(sns_strength, sns_strength, int, 0, 100),
    WEBP_CONFIG_OPTION(filter_strength, filter_strength, int, 0, 100),
    WEBP_CONFIG_OPTION(filter_sharpness, filter_sharpness, int, 0, 7),
    WEBP_CONFIG_OPTION(filter_type, filter_type, int, 0, 1),
    WEBP_CONFIG_OPTION(autofilter, autofilter, int, 0, 1),
    WEBP_CONFIG_OPTION(alpha_compression, alpha_compression, int, 0, 1),
    WEBP_CONFIG_OPTION(alpha_filtering, alpha_filtering, int, 0, 2),
    WEBP_CONFIG_OPTION(alpha_quality, alpha_quality, int, 0, 100),
    WEBP_CONFIG_OPTION(pass, pass, int, 1, 10),
    WEBP_CONFIG_OPTION(preprocessing, preprocessing, int, 0, 2),
    WEBP_CONFIG_OPTION(partitions, partitions, int, 0, 3),
    WEBP_CONFIG_OPTION(partition_limit, partition_limit, int, 0, 100),
    WEBP_CONFIG_OPTION(emulate_jpeg_size, emulate_jpeg_size, int, 0, 1),
    WEBP_CONFIG_OPTION(thread_level, thread_level, int, 0, 1),
    WEBP_CONFIG_OPTION(low_memory, low_memory, int, 0, 1),
    WEBP_CONFIG_OPTION(near_lossless, near_lossless, int, 0, 100),
    WEBP_CONFIG_OPTION(exact, exact, int, 0, 1),
};

#undef WEBP_CONFIG_OPTION

// type 后缀写法，与 jpg90 / png24 一致。
// "webp"         默认（DRAWING 预设，quality 100）
// "webp<数字>"   质量 0..100
// "webplossless" 无损，并保留透明像素的 RGB
void ApplyWebPSaveMode(const ttstr &mode, WebPConfig &config) {
    if(!mode.StartsWith(TJS_W("webp")) || mode.length() <= 4)
        return;
    const ttstr suffix = mode.SubString(4, mode.length() - 4);
    if(suffix == TJS_W("lossless")) {
        config.lossless = 1;
        // 默认 exact=0 会改写透明像素的 RGB，读回不再逐像素相等。
        config.exact = 1;
        return;
    }
    bool digits = true;
    int quality = 0;
    for(tjs_uint i = 0; i < suffix.length(); i++) {
        const tjs_char c = suffix[i];
        if(c < TJS_W('0') || c > TJS_W('9')) {
            digits = false;
            break;
        }
        quality = std::min(100, quality * 10 + (c - TJS_W('0')));
    }
    if(digits)
        config.quality = static_cast<float>(quality);
}

// 未知键忽略。值按 WebPConfigOptions 的边界夹取。
void ApplyWebPSaveMeta(iTJSDispatch2 *meta, WebPConfig &config) {
    struct MetaDictionaryEnumCallback : public tTJSDispatch {
        WebPConfig *config_;
        explicit MetaDictionaryEnumCallback(WebPConfig *config) :
            config_(config) {}
        tjs_error FuncCall(tjs_uint32 flag, const tjs_char *membername,
                           tjs_uint32 *hint, tTJSVariant *result,
                           tjs_int numparams, tTJSVariant **param,
                           iTJSDispatch2 *objthis) override {
            if(numparams < 3)
                return TJS_E_BADPARAMCOUNT;
            const tjs_uint32 memberflags =
                static_cast<tjs_uint32>(static_cast<tjs_int>(*param[1]));
            if(memberflags & TJS_HIDDENMEMBER) {
                if(result)
                    *result = static_cast<tjs_int>(1);
                return TJS_S_OK;
            }
            const ttstr name = *param[0];
            for(const WebPConfigOption &option : WebPConfigOptions) {
                if(name != option.Name)
                    continue;
                double value = param[2]->AsReal();
                if(!(value == value))
                    break;
                value = std::clamp(value, option.Minimum, option.Maximum);
                option.Apply(*config_, value);
                break;
            }
            if(result)
                *result = static_cast<tjs_int>(1);
            return TJS_S_OK;
        }
    } callback(&config);
    tTJSVariantClosure closure(&callback, nullptr);
    meta->EnumMembers(TJS_IGNOREPROP, &closure, meta);
}

// 位图内存按 RGBA 排布。GetScanLine 的行宽是 width * 4 字节。
std::vector<uint8_t> CopyBitmapToRGBA(const iTVPBaseBitmap *image) {
    const tjs_uint width = image->GetWidth();
    const tjs_uint height = image->GetHeight();
    const tjs_uint stride = width * sizeof(tjs_uint32);
    std::vector<uint8_t> data(static_cast<size_t>(stride) * height);
    for(tjs_uint y = 0; y < height; y++)
        memcpy(data.data() + static_cast<size_t>(y) * stride,
               image->GetScanLine(y), stride);
    return data;
}

} // namespace

bool TVPAcceptSaveAsWebP(void *formatdata, const ttstr &type,
                         class iTJSDispatch2 **dic) {
    bool result = false;
    if(type.StartsWith(TJS_W("webp")))
        result = true;
    else if(type == TJS_W(".webp"))
        result = true;
    if(result && dic) {
        tTJSVariant options;
        TVPExecuteExpression(
            TJS_W("(const)%[") TJS_W("\"lossless\"=>(const)%[\"type\"=>"
                                     "\"boolean\",\"desc\"=>\"lossless "
                                     "compression\",\"default\"=>false],")
                TJS_W("\"quality\"=>(const)%[\"type\"=>\"range\",\"min\"=>0,"
                      "\"max\"=>100,\"desc\"=>\"0 is small, 100 is best "
                      "quality\",\"default\"=>100],")
                    TJS_W("\"method\"=>(const)%[\"type\"=>\"select\","
                          "\"items\"=>(const)[\"fastest\",\"1\",\"2\",\"3\","
                          "\"4\",\"5\",\"slowest\"],\"desc\"=>\"encoder "
                          "speed\",\"default\"=>4],")
                        TJS_W("\"alpha_quality\"=>(const)%[\"type\"=>"
                              "\"range\",\"min\"=>0,\"max\"=>100,\"desc\"=>"
                              "\"alpha channel quality\",\"default\"=>100]") TJS_W("]"),
            nullptr, &options);
        if(options.Type() == tvtObject) {
            *dic = options.AsObject();
        }
    }
    return result;
}

void TVPSaveAsWebP(void *formatdata, tTJSBinaryStream *dst,
                   const iTVPBaseBitmap *image, const ttstr &mode,
                   iTJSDispatch2 *meta) {
    const tjs_uint width = image->GetWidth();
    const tjs_uint height = image->GetHeight();
    if(width == 0 || height == 0)
        TVPThrowInternalError;

    WebPConfig config;
    if(WebPConfigPreset(&config, WEBP_PRESET_DRAWING, 100) == 0)
        TVPThrowExceptionMessage(TJS_W("WebP encoder preset failed"));
    ApplyWebPSaveMode(mode, config);
    if(meta)
        ApplyWebPSaveMeta(meta, config);
    if(WebPValidateConfig(&config) == 0)
        TVPThrowExceptionMessage(TJS_W("WebP encoder config is invalid"));

    const tjs_uint stride = width * sizeof(tjs_uint32);
    std::vector<uint8_t> data = CopyBitmapToRGBA(image);

    WebPPicture picture;
    if(WebPPictureInit(&picture) == 0)
        TVPThrowExceptionMessage(TJS_W("WebP picture init failed"));
    picture.width = static_cast<int>(width);
    picture.height = static_cast<int>(height);
    picture.use_argb = 1;
    if(WebPPictureImportRGBA(&picture, data.data(), stride) == 0) {
        WebPPictureFree(&picture);
        TVPThrowExceptionMessage(TJS_W("WebP picture import failed"));
    }

    // 编码结果先入内存。写流失败时抛 TJS 异常，异常不穿过 libwebp 的 C 帧。
    WebPMemoryWriter writer;
    WebPMemoryWriterInit(&writer);
    picture.writer = WebPMemoryWrite;
    picture.custom_ptr = &writer;

    const int encoded = WebPEncode(&config, &picture);
    const int errorCode = picture.error_code;
    WebPPictureFree(&picture);
    if(encoded == 0) {
        WebPMemoryWriterClear(&writer);
        TVPThrowExceptionMessage(TJS_W("WebP encode error: %1"),
                                 ttstr(errorCode));
    }
    try {
        dst->WriteBuffer(writer.mem, writer.size);
    } catch(...) {
        WebPMemoryWriterClear(&writer);
        throw;
    }
    WebPMemoryWriterClear(&writer);
}
