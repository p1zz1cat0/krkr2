#ifndef YOGHOURT_LAYEREXSAVE_SAVE_SNAPSHOT_H
#define YOGHOURT_LAYEREXSAVE_SAVE_SNAPSHOT_H

// [krkr2] 异步保存的线程边界（fork 专有文件，上游没有对应物）。
//
// 上游 Main.cpp 的 worker 线程直接读 Layer 的 TJS 属性（mainImageBuffer、
// imageWidth、pitch）并读 tags 字典，再调 TVPCreateStream 写文件。本 fork
// 不允许工作线程触碰 TJS 与 Layer 缓冲：编码期间脚本仍可能改动图层，视图
// 跨线持有既是数据竞争也违反 PluginSafety 的 view 契约。因此异步保存把
// 「读 TJS」和「写 storage」全部收在主线程：
//
//   - 主线程用 LayerReadView 一次性拷贝像素到 SaveSnapshot::buffer；
//   - 主线程把 tags 字典 marshalling 成纯数据的 SaveTags；
//   - worker 只做编码（compress.hpp 的 CompressBase），完成后 post 消息；
//   - 主线程在 receiver 里写文件（StorageIntf 的流打开/写入保持主线程职责，
//     与 fork 既有的主线程分片实现一致）。
//
// 这样 worker 侧没有任何 TJS 依赖，同步路径与异步路径仍然共用同一套编码器，
// 只有「取缓冲」和「写文件」两处由调用方决定。

#include "tjs.h"

#include <vector>

/**
 * タグ情報のスナップショット
 *
 * 上游把 tags 字典直接交给编码器读（ncbPropAccessor / EnumMembers）。异步
 * 路径在主线程先用本类型完成读取，编码器只接触纯数据；同步路径的
 * iTJSDispatch2 构造也走同一条读取代码，两条路径对 tags 的解释完全一致。
 */
class SaveTags {
    struct Entry {
        ttstr name;
        tTJSVariant value;
    };

    std::vector<Entry> entries;

public:
    SaveTags() = default;

    // [krkr2] 主线程专用：从 TJS 字典取出全部成员（跳过 hidden）。
    // 传 NULL 得到空集合，与上游「tags 省略」语义一致。
    explicit SaveTags(iTJSDispatch2 *tagsDict);

    [[nodiscard]] bool Empty() const { return entries.empty(); }

    [[nodiscard]] bool HasValue(const tjs_char *name) const;
    [[nodiscard]] int IntValue(const tjs_char *name,
                               int defaultValue = 0) const;
    [[nodiscard]] ttstr StringValue(const tjs_char *name) const;

    // TLG5 の tags chunk テキスト（上游 TagsCaller の
    // "len:name=len:value," 形式）
    [[nodiscard]] ttstr EncodeTlg5StructuredData() const;

    // 枚举コールバックからの取り込み（SaveTagsEnumCaller 専用ではなく
    // 公開：EnumMembers のコールバッククラスが呼ぶ）
    void Add(const tTJSVariant &name, const tTJSVariant &value);
};

/**
 * 保存対象レイヤのスナップショット
 *
 * 像素缓冲是紧凑布局（pitchBytes == width * 4）；worker 侧的任何编码都不再
 * 触碰原图层。像素内存序与本 fork 一致：R,G,B,A（低字节 R）。
 */
struct SaveSnapshot {
    std::vector<tjs_uint8> buffer;
    long width = 0;
    long height = 0;
    long pitchBytes = 0;
    SaveTags tags;

    [[nodiscard]] const tjs_uint8 *Row(long y) const {
        return buffer.data() + static_cast<std::size_t>(y) *
                                   static_cast<std::size_t>(pitchBytes);
    }
};

// [krkr2] 主线程专用：按 PluginSafety 的 LayerReadView 读取几何与像素，拷贝
// 成紧凑缓冲并读取 tags。失败返回 false（调用方按上游语义抛 "invalid
// layer"）。tagsDict 传 NULL 表示不带标签。
bool CaptureSaveSnapshot(iTJSDispatch2 *layer, iTJSDispatch2 *tagsDict,
                         SaveSnapshot &snapshot);

#endif // YOGHOURT_LAYEREXSAVE_SAVE_SNAPSHOT_H
