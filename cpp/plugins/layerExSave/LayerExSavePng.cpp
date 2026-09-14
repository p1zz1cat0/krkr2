// layerExSave.dll PNG 编码器
//
// 两条路径，与上游 wtnbgo/layerExSave 的行为分工保持一致：
//   - 自研 chunk 路径（本文件 PngChunkedEncoder）：固定 32bit RGBA8888、
//     filter 0、无 interlace，zlib deflate；上游用于 Window 异步保存
//     （CompressAndSave<CompressPNG>::save），支持 pHYs/oFFs/vpAg 附加
//     chunk 与 comp_lv 压缩级别。此处拆分为行粒度 step() 以支持主线程
//     分帧推进；zlib 以 Z_NO_FLUSH 分段喂入，输出与一次性编码一致。
//   - lodepng 路径（EncodeWithLodePNG / EncodeProvinceWithLodePNG）：
//     上游同步保存（Layer.saveLayerImagePng / saveLayerImagePngOctet /
//     saveProvinceImage）的默认路径，RGB/RGBA 自适应、附加 chunk 经
//     lodepng 的 unknown chunks 注入，comp_lv 传入 zlib deflate。
//
// 通道序：PNG 明文为 R,G,B,A 字节序，与本 fork 的 mainImageBuffer 内存序
// （低字节 R）一致，像素直写无需交换。上游 Windows krkr2 为 B,G,R,A，
// 其 writePixel/MakeVectorImage 的 p[2],p[1],p[0],p[3] 与这里的直写等效。

#include "LayerExSaveEncoders.h"

#include "MsgIntf.h"
#include "thirdparty/lodepng/lodepng.h"

#include <zlib.h>

#include <cstring>
#include <memory>

namespace LayerExSave {
namespace {

constexpr tjs_uint32 kPngTypeRgba8888 = 0x08060000L; // bitdepth 8 | RGBA
constexpr std::size_t kDeflateOutStep = 256 * 1024;

// 一条 zlib deflate 流，输出滚动收集进 out。
class DeflateStream {
public:
    DeflateStream(int level, std::vector<tjs_uint8> &out) : out_(out) {
        std::memset(&zs_, 0, sizeof(zs_));
        if(::deflateInit(&zs_, level) != Z_OK)
            TVPThrowExceptionMessage(TJS_W("layerExSave: deflate initialize"));
    }
    ~DeflateStream() { ::deflateEnd(&zs_); }

    DeflateStream(const DeflateStream &) = delete;
    DeflateStream &operator=(const DeflateStream &) = delete;

    // 喂入一段明文（Z_NO_FLUSH；分段喂入与一次性编码输出一致）。
    void feed(const tjs_uint8 *in, std::size_t size) {
        zs_.next_in = const_cast<Bytef *>(in);
        zs_.avail_in = static_cast<uInt>(size);
        pump(Z_NO_FLUSH);
    }

    // 结束输入；返回流是否完整结束（Z_STREAM_END）。
    // deflate 全部写完后才收缩输出到有效长度：pump 内不得缩小 vector，
    // 否则下一次 pump 期间 deflate 已写入的尾部会在随后的
    // resize(value-initialize) 中被清零。
    bool finish() {
        const int s = pump(Z_FINISH);
        out_.resize(zs_.total_out);
        return s == Z_STREAM_END;
    }

private:
    int pump(int flush) {
        int s = Z_OK;
        do {
            if(!zs_.avail_out) {
                const std::size_t produced = zs_.total_out;
                out_.resize(produced + kDeflateOutStep);
                zs_.next_out = out_.data() + produced;
                zs_.avail_out = static_cast<uInt>(out_.size() - produced);
            }
        } while((s = ::deflate(&zs_, flush)) == Z_OK);
        return s;
    }

    z_stream zs_{};
    std::vector<tjs_uint8> &out_;
};

// 单个 PNG chunk 的构建缓冲：[type(4)][payload...]。
class ChunkBuilder {
public:
    void begin(const char *type) {
        buf_.clear();
        buf_.insert(buf_.end(), type, type + 4);
    }
    void writeInt8(tjs_uint8 v) { buf_.push_back(v); }
    void writeBigInt32(tjs_uint32 v) {
        const tjs_uint8 b[4] = {static_cast<tjs_uint8>((v >> 24) & 0xff),
                                static_cast<tjs_uint8>((v >> 16) & 0xff),
                                static_cast<tjs_uint8>((v >> 8) & 0xff),
                                static_cast<tjs_uint8>(v & 0xff)};
        buf_.insert(buf_.end(), b, b + 4);
    }
    void writeBytes(const tjs_uint8 *p, std::size_t size) {
        buf_.insert(buf_.end(), p, p + size);
    }

    // 完整 chunk 字节：[payload len][type][payload][crc32]（大端长度）。
    std::vector<tjs_uint8> complete() const {
        const unsigned long crc =
            ::crc32(0, buf_.data(), static_cast<uInt>(buf_.size()));
        const tjs_uint32 payloadLen =
            static_cast<tjs_uint32>(buf_.size() - 4);
        std::vector<tjs_uint8> full;
        full.reserve(buf_.size() + 8);
        full.push_back(static_cast<tjs_uint8>((payloadLen >> 24) & 0xff));
        full.push_back(static_cast<tjs_uint8>((payloadLen >> 16) & 0xff));
        full.push_back(static_cast<tjs_uint8>((payloadLen >> 8) & 0xff));
        full.push_back(static_cast<tjs_uint8>(payloadLen & 0xff));
        full.insert(full.end(), buf_.begin(), buf_.end());
        full.push_back(static_cast<tjs_uint8>((crc >> 24) & 0xff));
        full.push_back(static_cast<tjs_uint8>((crc >> 16) & 0xff));
        full.push_back(static_cast<tjs_uint8>((crc >> 8) & 0xff));
        full.push_back(static_cast<tjs_uint8>(crc & 0xff));
        return full;
    }

    void finishTo(OutputBuffer &main) const {
        const std::vector<tjs_uint8> full = complete();
        main.write(full.data(), full.size());
    }

    void appendTo(std::vector<tjs_uint8> &out) const {
        const std::vector<tjs_uint8> full = complete();
        out.insert(out.end(), full.begin(), full.end());
    }

    // lodepng unknown chunks 注入用：type 与 payload 分体。
    const char *type() const {
        return reinterpret_cast<const char *>(buf_.data());
    }
    const tjs_uint8 *payload() const { return buf_.data() + 4; }
    std::size_t payloadSize() const { return buf_.size() - 4; }

private:
    std::vector<tjs_uint8> buf_;
};

// 上游 ChunkSetResoXY / ChunkSetOffsXY / ChunkSetVpagWH 的构建逻辑。
void buildResoChunk(const PngTagParams &tags, ChunkBuilder &chunk) {
    chunk.begin("pHYs");
    chunk.writeBigInt32(static_cast<tjs_uint32>(tags.resoX));
    chunk.writeBigInt32(static_cast<tjs_uint32>(tags.resoY));
    chunk.writeInt8(tags.resoMeter ? 1 : 0);
}
void buildOffsChunk(const PngTagParams &tags, ChunkBuilder &chunk) {
    chunk.begin("oFFs");
    chunk.writeBigInt32(static_cast<tjs_uint32>(tags.offsX));
    chunk.writeBigInt32(static_cast<tjs_uint32>(tags.offsY));
    chunk.writeInt8(tags.offsMicrometer ? 1 : 0);
}
void buildVpagChunk(const PngTagParams &tags, ChunkBuilder &chunk) {
    chunk.begin("vpAg");
    chunk.writeBigInt32(static_cast<tjs_uint32>(tags.vpagW));
    chunk.writeBigInt32(static_cast<tjs_uint32>(tags.vpagH));
    chunk.writeInt8(tags.vpagMicrometer ? 1 : 0);
}

// 分片 PNG 编码器（自研 chunk 路径）。
class PngChunkedEncoder final : public ChunkedEncoder {
public:
    PngChunkedEncoder(const ImageSource &image, const PngTagParams &tags,
                      int compLv)
        : image_(image), deflate_(compLv, deflateOut_) {
        // PNG signature + IHDR
        header_.write("\x89PNG\x0D\x0A\x1A\x0A", 8);
        {
            ChunkBuilder ihdr;
            ihdr.begin("IHDR");
            ihdr.writeBigInt32(static_cast<tjs_uint32>(image_.width));
            ihdr.writeBigInt32(static_cast<tjs_uint32>(image_.height));
            ihdr.writeBigInt32(kPngTypeRgba8888);
            ihdr.writeInt8(0); // 非 interlace
            ihdr.finishTo(header_);
        }
        // 附加 chunk（IHDR 之后、IDAT 之前）
        ChunkBuilder chunk;
        if(tags.hasReso) {
            buildResoChunk(tags, chunk);
            chunk.finishTo(header_);
        }
        if(tags.hasOffs) {
            buildOffsChunk(tags, chunk);
            chunk.finishTo(header_);
        }
        if(tags.hasVpag) {
            buildVpagChunk(tags, chunk);
            chunk.finishTo(header_);
        }
        plain_.reserve((static_cast<std::size_t>(image_.width) * 4 + 1) *
                       kRowsPerStep);
        header_.shrinkToWritten(); // 头部定长，丢弃预留容量
    }

    bool step() override {
        if(finished_)
            return false;
        if(nextRow_ >= image_.height) {
            finishTail();
            return false;
        }
        const tjs_int yLim = nextRow_ + kRowsPerStep < image_.height
                                 ? nextRow_ + kRowsPerStep
                                 : image_.height;
        plain_.clear();
        for(tjs_int y = nextRow_; y < yLim; y++) {
            const tjs_uint8 *src = image_.row(y);
            plain_.push_back(0); // filter type 0
            // PNG 明文即 R,G,B,A，与 fork 内存序一致，整行直拷
            plain_.insert(plain_.end(), src,
                          src + static_cast<std::size_t>(image_.width) * 4);
        }
        deflate_.feed(plain_.data(), plain_.size());
        nextRow_ = yLim;
        return true;
    }

    int progressPercent() const override {
        if(image_.height == 0)
            return 100;
        const int pct = nextRow_ * 100 / image_.height;
        return pct > 100 ? 100 : pct;
    }

    bool finished() const override { return finished_; }

    const std::vector<tjs_uint8> &data() const override { return out_; }

private:
    static constexpr tjs_int kRowsPerStep = 32;

    void finishTail() {
        if(!deflate_.finish())
            TVPThrowExceptionMessage(TJS_W("layerExSave: deflate stream"));
        out_.insert(out_.end(), header_.data().begin(),
                    header_.data().end());
        {
            ChunkBuilder idat;
            idat.begin("IDAT");
            idat.writeBytes(deflateOut_.data(), deflateOut_.size());
            idat.appendTo(out_);
        }
        {
            ChunkBuilder iend;
            iend.begin("IEND");
            iend.appendTo(out_);
        }
        finished_ = true;
    }

    const ImageSource image_;
    OutputBuffer header_;
    std::vector<tjs_uint8> deflateOut_;
    std::vector<tjs_uint8> out_;
    std::vector<tjs_uint8> plain_;
    DeflateStream deflate_;
    tjs_int nextRow_ = 0;
    bool finished_ = false;
};

// lodepng 路径：从 ImageSource 生成 RGBA（或 RGB）明文；alpha 全 255 时
// 退化 RGB（上游 MakeVectorImage 语义）。
bool makeVectorImage(const ImageSource &image, std::vector<tjs_uint8> &out,
                     bool &alpha) {
    if(image.width <= 0 || image.height <= 0 || !image.pixels)
        return false;
    const std::size_t len = static_cast<std::size_t>(image.width) * 4;
    out.assign(len * image.height, 0);
    alpha = false;
    for(tjs_int y = 0; y < image.height; y++) {
        const tjs_uint8 *p = image.row(y);
        tjs_uint8 *w = out.data() + len * y;
        for(tjs_int x = 0; x < image.width; x++, p += 4) {
            *w++ = p[0];
            *w++ = p[1];
            *w++ = p[2];
            if((*w++ = p[3]) != 255)
                alpha = true;
        }
    }
    return true;
}

// 上游 MakeDefaultPalette：8bit 索引按 0b GGG RRR BB 布局展开。
void makeDefaultPalette(std::vector<tjs_uint8> &palette) {
    palette.resize(256 * 4);
    static const tjs_uint8 table2bit[4] = {0, 128, 192, 255};
    static const tjs_uint8 table3bit[8] = {0, 64, 96, 128, 160, 192, 224, 255};
    for(int i = 0; i < 256; i++) {
        palette[i * 4 + 0] = table3bit[(i >> 2) & 0x7]; // R
        palette[i * 4 + 1] = table3bit[(i >> 5) & 0x7]; // G
        palette[i * 4 + 2] = table2bit[i & 0x3];        // B
        palette[i * 4 + 3] = i ? 255 : 0;
    }
}

// 上游 CustomDeflate：lodepng 的 custom_zlib 钩子（zlib deflate 实现）。
unsigned customDeflate(unsigned char **out, std::size_t *outsize,
                       const unsigned char *in, std::size_t insize,
                       const lexsave::LodePNGCompressSettings *settings) {
    const int compLv =
        settings
            ? static_cast<int>(
                  reinterpret_cast<tjs_intptr_t>(settings->custom_context))
            : 1;
    std::vector<tjs_uint8> deflated;
    DeflateStream stream(compLv, deflated);
    stream.feed(in, insize);
    if(!stream.finish())
        return 13; // deflate 未正常结束
    *out = static_cast<unsigned char *>(malloc(deflated.size()));
    if(!*out)
        return 83; // alloc fail
    std::memcpy(*out, deflated.data(), deflated.size());
    *outsize = deflated.size();
    return 0;
}

// 上游 SetCustomChunk：把构建好的 chunk 注入 lodepng 的 unknown chunks。
void injectCustomChunk(lexsave::lodepng::State &state,
                       const ChunkBuilder &chunk) {
    if(lexsave::lodepng_chunk_create(
           &state.info_png.unknown_chunks_data[0],
           &state.info_png.unknown_chunks_size[0],
           static_cast<unsigned>(chunk.payloadSize()), chunk.type(),
           chunk.payload()) != 0)
        TVPThrowExceptionMessage(TJS_W("layerExSave: chunk create"));
}

} // namespace

std::unique_ptr<ChunkedEncoder>
CreatePngChunkEncoder(const ImageSource &image, const PngTagParams &tags) {
    int compLv = Z_DEFAULT_COMPRESSION;
    if(tags.hasCompLv)
        compLv = tags.compLv;
    return std::make_unique<PngChunkedEncoder>(image, tags, compLv);
}

// lodepng 同步编码（saveLayerImagePng / saveLayerImagePngOctet 共用）。
// params 为空时使用默认压缩（上游 EncodeLodePNGCommon 语义）。
bool EncodeWithLodePNG(const ImageSource &image, const PngTagParams *params,
                       std::vector<tjs_uint8> &pngOut) {
    std::vector<tjs_uint8> plain;
    bool alpha = false;
    if(!makeVectorImage(image, plain, alpha))
        return false;

    lexsave::lodepng::State state;
    state.encoder.auto_convert = 0; // 关闭自动颜色判定
    state.info_png.color.colortype =
        alpha ? lexsave::LCT_RGBA : lexsave::LCT_RGB;
    state.info_png.color.bitdepth = 8;
    state.info_png.color.key_defined = 0;

    if(params) {
        ChunkBuilder chunk;
        if(params->hasReso) {
            buildResoChunk(*params, chunk);
            injectCustomChunk(state, chunk);
        }
        if(params->hasOffs) {
            buildOffsChunk(*params, chunk);
            injectCustomChunk(state, chunk);
        }
        if(params->hasVpag) {
            buildVpagChunk(*params, chunk);
            injectCustomChunk(state, chunk);
        }
        if(params->hasCompLv && params->compLv >= 0) {
            state.encoder.zlibsettings.custom_zlib = &customDeflate;
            state.encoder.zlibsettings.custom_context = reinterpret_cast<void *>(
                static_cast<tjs_intptr_t>(params->compLv));
            if(!params->compLv)
                state.encoder.filter_strategy = lexsave::LFS_ZERO;
        }
    }

    return lexsave::lodepng::encode(pngOut, plain, image.width, image.height,
                                    state) == 0;
}

// province（1 字节/像素）→ 256 调色板 PNG（上游 encodeProvinceImage 语义）。
bool EncodeProvinceWithLodePNG(const tjs_uint8 *pixels, tjs_int width,
                               tjs_int height, tjs_int pitchBytes,
                               std::vector<tjs_uint8> &pngOut) {
    if(!pixels || width <= 0 || height <= 0)
        return false;
    std::vector<tjs_uint8> plain(static_cast<std::size_t>(width) * height);
    for(tjs_int y = 0; y < height; y++)
        std::memcpy(plain.data() + static_cast<std::size_t>(y) * width,
                    pixels + static_cast<std::size_t>(y) * pitchBytes,
                    static_cast<std::size_t>(width));

    lexsave::lodepng::State state;
    // 显式关闭 lodepng 的 auto_convert：上游未设置该位，lodepng 会把
    // 调色板输入自动升级为 RGBA，违背 manual.tjs "256パレット式PNG固定"
    // 的承诺，这里是有意偏离（保持调色板输出）。
    state.encoder.auto_convert = 0;
    state.info_png.color.colortype = state.info_raw.colortype = lexsave::LCT_PALETTE;
    state.info_png.color.bitdepth = state.info_raw.bitdepth = 8;
    state.info_png.color.key_defined = state.info_raw.key_defined = 0;
    std::vector<tjs_uint8> palette;
    makeDefaultPalette(palette);
    for(int i = 0; i < 256; i++) {
        lexsave::lodepng_palette_add(&state.info_png.color, palette[i * 4 + 0],
                                     palette[i * 4 + 1], palette[i * 4 + 2],
                                     palette[i * 4 + 3]);
        lexsave::lodepng_palette_add(&state.info_raw, palette[i * 4 + 0],
                                     palette[i * 4 + 1], palette[i * 4 + 2],
                                     palette[i * 4 + 3]);
    }
    return lexsave::lodepng::encode(pngOut, plain, width, height, state) == 0;
}

} // namespace LayerExSave
