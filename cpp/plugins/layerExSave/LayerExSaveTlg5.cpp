// layerExSave.dll TLG5 编码器
//
// 算法与上游 wtnbgo/layerExSave 的 savetlg5.cpp 一致（4 色固定、4 行一个
// block、通道间差分 + slide 辞典压缩、可选 TLG0.0 sds + tags chunk 包装），
// 并按上游 SaveInfo::start 的语义由扩展名分派（.png 之外全部 TLG5）。
//
// 与上游的差异：
//   - 滑动压缩器直接使用 core 的 SlideCompressor（cpp/core/visual/
//     SaveTLG.h，krkr2 本体同源实现，Encode/Store/Restore 接口相同），
//     不再捆绑上游的 tlg5/slide.cpp。
//   - 文件通道序显式写 B,G,R,A：fork 的 mainImageBuffer 内存序为 R,G,B,A，
//     解码端 TVPTLG5ComposeColors4To4_c 期望文件通道 0→内存字节 2。上游
//     Windows krkr2 内存序即 B,G,R,A，"按内存序直写"与这里的显式写出
//     逐字节等效。
//   - 编码拆分为 block 粒度的 step()（上游是一次性循环 + progress 回调）。

#include "LayerExSaveEncoders.h"

#include "SaveTLG.h"

#include <cstring>

namespace LayerExSave {
namespace {

constexpr int kTlg5BlockHeight = 4;
constexpr int kTlg5Colors = 4; // 固定 4 色（BGRA 通道序）

class Tlg5Encoder final : public ChunkedEncoder {
public:
    Tlg5Encoder(const ImageSource &image, const ttstr &tagsText)
        : image_(image), tags_(tagsText), out_(), compressor_() {
        const tjs_int w = image_.width, h = image_.height;
        blockCount_ = (h - 1) / kTlg5BlockHeight + 1;
        for(int c = 0; c < kTlg5Colors; c++) {
            inBuf_[c].resize(static_cast<std::size_t>(w) * kTlg5BlockHeight);
            outBuf_[c].resize(static_cast<std::size_t>(w) * kTlg5BlockHeight * 9 / 4);
        }
        blockSize_.assign(static_cast<std::size_t>(blockCount_), 0);

        if(!tags_.IsEmpty()) {
            // TLG0.0 Structured Data Stream 头 + raw TLG 长度占位
            out_.write("TLG0.0\x00sds\x1a\x00", 11);
            rawLenPos_ = out_.tell();
            out_.writeInt32(0);
        }
        // TLG5.0 raw 头
        out_.write("TLG5.0\x00raw\x1a\x00", 11);
        out_.writeInt8(kTlg5Colors);
        out_.writeInt32(static_cast<tjs_uint32>(w));
        out_.writeInt32(static_cast<tjs_uint32>(h));
        out_.writeInt32(kTlg5BlockHeight);
        blockSizePos_ = out_.tell();
        for(int i = 0; i < blockCount_; i++)
            out_.writeInt32(0); // block 尺寸表占位
    }

    bool step() override {
        if(finished_)
            return false;
        if(nextBlock_ >= blockCount_) {
            finishTail();
            return false;
        }

        const tjs_int w = image_.width, h = image_.height;
        const tjs_int blkY = nextBlock_ * kTlg5BlockHeight;
        tjs_int yLim = blkY + kTlg5BlockHeight;
        if(yLim > h)
            yLim = h;

        // 行内逐字节差分（在文件通道序 B,G,R,A 上做，与解码端一致）。
        // 上游语义：prevcl 每行重置，对"与上一行的差值"再做行内水平差分链。
        std::size_t inp = 0;
        for(tjs_int y = blkY; y < yLim; y++) {
            const tjs_uint8 *current = image_.row(y);
            const tjs_uint8 *upper = y != 0 ? image_.row(y - 1) : nullptr;
            int prevCl[kTlg5Colors] = {0, 0, 0, 0};
            for(tjs_int x = 0; x < w; x++) {
                // fork 内存序 R,G,B,A → 文件通道序 B,G,R,A
                const tjs_uint8 *p = current + x * 4;
                int cur[kTlg5Colors] = {p[2], p[1], p[0], p[3]};
                int val[kTlg5Colors];
                for(int c = 0; c < kTlg5Colors; c++) {
                    int cl = cur[c];
                    if(upper) {
                        const tjs_uint8 *u = upper + x * 4;
                        const int up[kTlg5Colors] = {u[2], u[1], u[0], u[3]};
                        cl -= up[c];
                    }
                    val[c] = cl - prevCl[c];
                    prevCl[c] = cl;
                }
                // TLG5 通道合成公式（固定，与通道语义无关）
                inBuf_[0][inp] = static_cast<tjs_uint8>(val[0] - val[1]);
                inBuf_[1][inp] = static_cast<tjs_uint8>(val[1]);
                inBuf_[2][inp] = static_cast<tjs_uint8>(val[2] - val[1]);
                inBuf_[3][inp] = static_cast<tjs_uint8>(val[3]);
                inp++;
            }
        }

        // 每通道压缩；压缩不划算时回退为 raw（上游语义，Restore 后重取）
        int blockSize = 0;
        for(int c = 0; c < kTlg5Colors; c++) {
            long wrote = 0;
            compressor_.Store();
            compressor_.Encode(inBuf_[c].data(), static_cast<long>(inp),
                               outBuf_[c].data(), wrote);
            if(wrote < static_cast<long>(inp)) {
                out_.writeInt8(0x00);
                out_.writeInt32(static_cast<tjs_uint32>(wrote));
                out_.write(outBuf_[c].data(), static_cast<std::size_t>(wrote));
                blockSize += static_cast<int>(wrote) + 4 + 1;
            } else {
                compressor_.Restore();
                out_.writeInt8(0x01);
                out_.writeInt32(static_cast<tjs_uint32>(inp));
                out_.write(inBuf_[c].data(), inp);
                blockSize += static_cast<int>(inp) + 4 + 1;
            }
        }
        blockSize_[nextBlock_] = blockSize;
        nextBlock_++;
        return true;
    }

    int progressPercent() const override {
        if(blockCount_ == 0)
            return 100;
        int pct = nextBlock_ * 100 / blockCount_;
        return pct > 100 ? 100 : pct;
    }

    bool finished() const override { return finished_; }

    const std::vector<tjs_uint8> &data() const override { return out_.data(); }

private:
    void finishTail() {
        // 回填 block 尺寸表
        std::size_t pos = blockSizePos_;
        for(int i = 0; i < blockCount_; i++, pos += 4)
            out_.writeInt32At(static_cast<tjs_uint32>(blockSize_[i]), pos);
        if(!tags_.IsEmpty()) {
            // 回填 raw 长度并追加 tags chunk
            const std::size_t rawEnd = out_.tell();
            out_.writeInt32At(
                static_cast<tjs_uint32>(rawEnd - rawLenPos_ - 4), rawLenPos_);
            out_.write("tags", 4);
            // 上游 tags 长度按 narrow 字节计
            const tjs_uint tagsLen = tags_.GetNarrowStrLen();
            out_.writeInt32(tagsLen);
            if(tagsLen > 0)
                out_.writeNarrow(tags_, tagsLen);
        }
        out_.shrinkToWritten();
        finished_ = true;
    }

    const ImageSource image_;
    const ttstr tags_;
    OutputBuffer out_;
    SlideCompressor compressor_;
    std::vector<tjs_uint8> inBuf_[kTlg5Colors];
    std::vector<tjs_uint8> outBuf_[kTlg5Colors];
    std::vector<int> blockSize_;
    int blockCount_ = 0;
    int nextBlock_ = 0;
    std::size_t blockSizePos_ = 0;
    std::size_t rawLenPos_ = 0;
    bool finished_ = false;
};

} // namespace

std::unique_ptr<ChunkedEncoder> CreateTlg5Encoder(const ImageSource &image,
                                                  const ttstr &tagsText) {
    return std::make_unique<Tlg5Encoder>(image, tagsText);
}

} // namespace LayerExSave
