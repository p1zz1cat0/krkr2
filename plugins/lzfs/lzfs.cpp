// lzfs.dll
//
// 闭源 KiriKiri 插件的兼容实现：注册 "lzfs" storage media，对经 lzfs://
// 打开的存储做 LZ4 frame 透明解压。
//
// 行为规格来自对真 DLL 的逆向记录（liulifox233/Kirakira 的
// crates/krkr-plugins/src/lzfs.rs，其逆向笔记 lzfs.md 记录了函数地址与
// 语义；本仓库的记录在 vendor fork 的 KRKR_PLUGINS.md）：
//   - V2Link(0x10001630) 注册名为 "lzfs" 的 media（LZ4Storage，
//     GetName 返回 "lzfs"），V2Unlink(0x10001080) 注销；
//   - Open(0x10001ac0) 把名字交给引擎内层存储搜索（TVPCreateIStream）：
//     文件是完整 LZ4 frame 时返回解压后的内存流，否则把同一内层流
//     rewind 后原样返回（raw passthrough）；
//   - Init(0x10001250) 用 LZ4F 逐 64KiB 读喂解码器；任何 LZ4F 错误都判
//     "不是 LZ4"；
//   - CheckExistentStorage 委托内层存储；GetListAt 是 no-op；
//     没有 TJS 成员；只读。
//
// 与 reference 的两处记录性差异：
//   1. reference 的读循环在"帧永不完整"的输入上挂死（截断帧、空文件；
//      lzfs.md 记录的 bug）。本实现把输入耗尽并入"不是 LZ4"，走 raw
//      透传。
//   2. reference 打开内层失败时抛 cannot open lz4 file:%1；本实现让底层
//      TVPCreateStream 的异常原样传播（保留更具体的失败原因）。已确认
//      没有脚本或插件依赖该文本。
//
// 解码直接用 vcpkg 的官方 LZ4F（与真 DLL 同族库），不手写 block 解码。

#include "ncbind.hpp"
#include "StorageIntf.h"
#include "UtilStreams.h"

#include <lz4frame.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <vector>

#define NCB_MODULE_NAME TJS_W("lzfs.dll")

namespace {

// reference 的 LZ4Stream::Init 以 64KiB 读块喂 LZ4F（lzfs.md:15）。
constexpr size_t kLzfsReadBlock = 64 * 1024;

// 尝试把 input 从当前位置起的完整 LZ4 frame 解压进 decoded。
//
// false 覆盖 reference 判"不是 LZ4"的全部情形：非 frame、帧损坏、校验
// 失败、帧不完整（reference 在这里会挂死，见文件头）。帧完整时帧后
// 剩下的字节和 reference 一样不再读。
bool decodeLz4Frame(tTJSBinaryStream *input, std::vector<tjs_uint8> &decoded) {
    LZ4F_dctx *rawCtx = nullptr;
    if(LZ4F_isError(LZ4F_createDecompressionContext(&rawCtx, LZ4F_VERSION))) {
        return false; // 只有资源失败能到这里；按"不是 LZ4"透传原文
    }
    // 读流/解码在异常下展开时（分配失败、档案流错误）也要释放上下文。
    std::unique_ptr<LZ4F_dctx, decltype(&LZ4F_freeDecompressionContext)> ctx(
        rawCtx, &LZ4F_freeDecompressionContext);

    bool complete = false;
    std::vector<tjs_uint8> inBuf(kLzfsReadBlock);
    std::vector<tjs_uint8> outBuf(kLzfsReadBlock);
    size_t inPos = 0;
    size_t inLen = 0;
    while(true) {
        if(inPos == inLen) {
            inLen =
                input->Read(inBuf.data(), static_cast<tjs_uint>(inBuf.size()));
            inPos = 0;
            if(inLen == 0)
                break; // 帧未完成而输入耗尽：截断，判"不是 LZ4"
        }
        size_t dstSize = outBuf.size();
        size_t srcSize = inLen - inPos;
        const size_t hint = LZ4F_decompress(ctx.get(), outBuf.data(), &dstSize,
                                            inBuf.data() + inPos, &srcSize,
                                            nullptr);
        if(LZ4F_isError(hint))
            break; // 损坏/校验失败 → raw
        inPos += srcSize;
        decoded.insert(decoded.end(), outBuf.data(), outBuf.data() + dstSize);
        if(hint == 0) {
            complete = true; // 帧完整
            break;
        }
        if(srcSize == 0 && dstSize == 0 && inPos < inLen) {
            // 输入还有富余却既没消耗也没产出：防御路径，宁可判"不是
            // LZ4"也不冒险挂死。
            break;
        }
    }
    return complete;
}

// "lzfs" storage media：reference 的 LZ4Storage 的纯 wrapper 语义。
class LzfsMedia : public iTVPStorageMedia {
public:
    LzfsMedia() { _ref = 1; }

    ~LzfsMedia() override = default;

    void AddRef() override { _ref++; }

    void Release() override {
        if(_ref == 1)
            delete this;
        else
            _ref--;
    }

    void GetName(ttstr &name) override { name = TJS_W("lzfs"); }

    void NormalizeDomainName(ttstr &) override {}

    void NormalizePathName(ttstr &) override {}

    // media 名下的文件是否存在完全由内层存储栈决定（lzfs.md:63）。
    bool CheckExistentStorage(const ttstr &name) override {
        return TVPIsExistentStorage(name);
    }

    tTJSBinaryStream *Open(const ttstr &name, tjs_uint32 flags) override {
        tTJSBinaryStream *inner = TVPCreateStream(name, flags);
        if(!inner)
            TVPThrowExceptionMessage(TJS_W("cannot open lz4 file:%1"), name);

        if((flags & TJS_BS_ACCESS_MASK) != TJS_BS_READ) {
            // 没有任何写路径会产出 LZ4 frame（reference 同样把内层流
            // 原样给出），不做探测。
            return inner;
        }

        std::vector<tjs_uint8> decoded;
        bool isLz4Frame = false;
        try {
            isLz4Frame = decodeLz4Frame(inner, decoded);
        } catch(...) {
            // 读流或解码期间的异常不能泄漏内层流（同 PSBFile.cpp 对流的
            // 处理）；异常照原样传播。
            delete inner;
            throw;
        }
        if(isLz4Frame) {
            delete inner;
            auto *stream = new tTVPMemoryStream();
            if(!decoded.empty()) {
                stream->WriteBuffer(decoded.data(),
                                    static_cast<tjs_uint>(decoded.size()));
            }
            stream->Seek(0, TJS_BS_SEEK_SET);
            return stream;
        }

        // 不是（完整有效的）LZ4：同一内层流 rewind 后原样返回。
        inner->SetPosition(0);
        return inner;
    }

    void GetListAt(const ttstr &, iTVPStorageLister *) override {}

    // 解压视图没有本地可访问名（lzfs.md:66-67 返回空串）：交回空串，
    // 让调用方走存储接口而不是本地文件系统。
    void GetLocallyAccessibleName(ttstr &name) override { name.Clear(); }

private:
    int _ref = 0;
};

LzfsMedia *lzfsMedia = nullptr;

// Plugins.link("lzfs.dll") 触发模块注册（ncbAutoRegister::LoadModule →
// 本回调）：与 reference 的 V2Link 一致，media 只在模块被链接时注册；
// 未链接它的游戏里 lzfs:// 名字仍按"无此 media"处理。
void initLzfsMedia() {
    if(lzfsMedia)
        return;
    lzfsMedia = new LzfsMedia();
    TVPRegisterStorageMedia(lzfsMedia);
    lzfsMedia->Release(); // 注册表持有引用；模块只留裸指针用于注销
    if(auto logger = spdlog::get("plugin")) {
        logger->info("lzfs: media registered (LZ4 frame passthrough)");
    }
}

void deInitLzfsMedia() {
    if(!lzfsMedia)
        return;
    TVPUnregisterStorageMedia(lzfsMedia);
    lzfsMedia = nullptr;
}

} // namespace

NCB_PRE_REGIST_CALLBACK(initLzfsMedia);
NCB_POST_UNREGIST_CALLBACK(deInitLzfsMedia);

// 注册型翻译单元在静态库里会被链接器剥离，PluginImpl.cpp 显式引用本锚点。
extern "C" void TVPLzfsPluginAnchor() {}
