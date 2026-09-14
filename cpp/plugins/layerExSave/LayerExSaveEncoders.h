// layerExSave.dll 编码器层
//
// 移植自 wtnbgo/layerExSave（作者 わたなべごう）：
//   https://github.com/wtnbgo/layerExSave （commit 83b0ef7）
// 许可证遵循吉里吉里本体（KrKr2）；LodePNG 部分为 zlib license（见
// thirdparty/lodepng/）。
//
// 本文件声明 TLG5 / PNG 的"可分片"编码器。与上游一次性循环编码不同，
// 编码被拆成 step() 工作单元，供异步保存（Window.startSaveLayerImage）在
// 主线程事件泵上分帧推进，也供同步保存（Layer.saveLayerImageTlg5 等）
// 一次 while(step()) 跑完。两条路径字节输出一致。
//
// 通道序约定（与 fork 的 mainImageBuffer 内存序 A<<24|B<<16|G<<8|R 对齐）：
//   - ImageSource 的内存字节序为 R,G,B,A（低字节 R，与 shrinkCopy 等在树
//     插件一致）；上游 Windows krkr2 为 B,G,R,A。
//   - TLG5 文件的 4 色通道序与写入方内存序绑定；本 fork 解码端
//     （TVPTLG5ComposeColors4To4_c）期望文件通道序 B,G,R,A（文件通道 0
//     落到内存字节 2）。因此编码时显式写 p[2],p[1],p[0],p[3]，产物与
//     Windows krkr2 写出的 TLG5 逐字节同构，跨引擎可读。
//   - PNG 原生 RGBA 字节序与本 fork 内存序一致（p[0..3] 直写）。

#ifndef YOGHOURT_LAYEREXSAVE_ENCODERS_H
#define YOGHOURT_LAYEREXSAVE_ENCODERS_H

#include "tjs.h"

#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>

namespace LayerExSave {

// 已校验几何的只读像素源。同步保存直接指进 Layer 的 mainImageBuffer
// （视图只在同步调用内使用）；异步保存指向 manager 持有的紧凑拷贝。
struct ImageSource {
    const tjs_uint8 *pixels = nullptr; // row 0 起点
    tjs_int width = 0;
    tjs_int height = 0;
    tjs_int pitchBytes = 0;

    const tjs_uint8 *row(tjs_int y) const {
        return pixels + static_cast<std::size_t>(y) *
                            static_cast<std::size_t>(pitchBytes);
    }
};

// TLG5/PNG 编码共用的顺序输出缓冲：追加写入 + 占位区随机回填。
// 上游 CompressBase 的输出缓冲等价物（vector + 追加位置）。
class OutputBuffer {
public:
    void write(const void *buf, std::size_t size) {
        ensure(cur_ + size);
        std::memcpy(data_.data() + cur_, buf, size);
        cur_ += size;
    }
    void writeInt8(tjs_uint8 v) { write(&v, 1); }
    void writeInt32(tjs_uint32 v) { writeInt32At(v, cur_); cur_ += 4; }
    void writeInt32At(tjs_uint32 v, std::size_t pos) {
        ensure(pos + 4);
        tjs_uint8 *p = data_.data() + pos;
        p[0] = static_cast<tjs_uint8>(v & 0xff);
        p[1] = static_cast<tjs_uint8>((v >> 8) & 0xff);
        p[2] = static_cast<tjs_uint8>((v >> 16) & 0xff);
        p[3] = static_cast<tjs_uint8>((v >> 24) & 0xff);
    }
    std::size_t tell() const { return cur_; }
    const std::vector<tjs_uint8> &data() const { return data_; }

    // 预留写入空间（与 advance 配对，供跳过占位区后回填）。
    void reserveExtra(std::size_t size) { ensure(cur_ + size); }
    void advance(std::size_t size) { cur_ += size; }

    // 丢弃 cur_ 之后的预留容量，使 data().size() 恰为已写入字节数
    // （编码完成后调用，写文件按 data().size() 取整段输出）。
    void shrinkToWritten() { data_.resize(cur_); }

    // ttstr 的 narrow（上游 ToNarrowStr 语义）写入；narrowLen 必须等于
    // text.GetNarrowStrLen()。
    void writeNarrow(const ttstr &text, tjs_uint narrowLen) {
        ensure(cur_ + narrowLen);
        text.ToNarrowStr(reinterpret_cast<tjs_nchar *>(data_.data() + cur_),
                         narrowLen);
        cur_ += narrowLen;
    }

private:
    void ensure(std::size_t size) {
        if(size > data_.size())
            data_.resize(size < 1024 ? 1024 : size * 2);
    }
    std::vector<tjs_uint8> data_;
    std::size_t cur_ = 0;
};

// PNG 附加 chunk 与压缩级别参数（主线程从 TJS 字典读出后的纯数据）。
struct PngTagParams {
    bool hasReso = false;
    tjs_int resoX = 0, resoY = 0;
    bool resoMeter = false; // reso_unit == "meter"

    bool hasOffs = false;
    tjs_int offsX = 0, offsY = 0;
    bool offsMicrometer = false;

    bool hasVpag = false;
    tjs_int vpagW = 0, vpagH = 0;
    bool vpagMicrometer = false;

    bool hasCompLv = false;
    int compLv = -1; // 0..9；lodepng 路径 0 附加 filter 策略变化
};

// 可分片编码器基类。全部方法只在构造时的调用线程（主线程）使用。
class ChunkedEncoder {
public:
    virtual ~ChunkedEncoder() = default;

    // 推进一个工作单元；整幅图编码完成后返回 false，此后 finished() 为
    // true 且 data() 可用。编码中途失败以 TJS 异常报告（与上游一致）。
    virtual bool step() = 0;

    // 0..100 的整数进度（异步保存按整数跳变过滤事件）。
    virtual int progressPercent() const = 0;

    virtual bool finished() const = 0;

    // finished() 之后有效；文件写回由调用方完成。
    virtual const std::vector<tjs_uint8> &data() const = 0;
};

// TLG5 编码器（可选 TLG0.0 sds + tags chunk 包装）。
// tagsText 为上游 TagsCaller 序列化格式："len:name=len:value,..."；
// 空字符串表示不带 tags chunk。
std::unique_ptr<ChunkedEncoder>
CreateTlg5Encoder(const ImageSource &image, const ttstr &tagsText);

// PNG 编码器（自研 chunk 路径，固定 32bit RGBA8888、filter 0、无 interlace，
// 与上游异步保存路径一致；支持 pHYs / oFFs / vpAg 附加 chunk）。
std::unique_ptr<ChunkedEncoder>
CreatePngChunkEncoder(const ImageSource &image, const PngTagParams &tags);

// PNG 编码器（LodePNG 路径，与上游同步保存一致：RGB/RGBA 自适应、附加
// chunk 经 unknown chunks 注入、comp_lv 传入 zlib deflate）。
// params 为 null 时使用 LodePNG 默认压缩。编码失败返回 false（上游语义：
// 调用方静默不写文件）。
bool EncodeWithLodePNG(const ImageSource &image, const PngTagParams *params,
                       std::vector<tjs_uint8> &pngOut);

// province（1 字节/像素）→ 256 调色板 PNG（上游 encodeProvinceImage 语义）。
bool EncodeProvinceWithLodePNG(const tjs_uint8 *pixels, tjs_int width,
                               tjs_int height, tjs_int pitchBytes,
                               std::vector<tjs_uint8> &pngOut);

} // namespace LayerExSave

#endif
