// lz4_stream.h — PackinOne 的 LZ4 压缩流。
//
// 官方 PackinOne.dll 内部集成了 LZ4（RTTI: LZ4StreamBase /
// LZ4CompressStream / LZ4DecompressStream，64KB 块、LZ4 Frame 封装，
// 见官方逆向文档 M12-04-02）。这些类是 C++ 内部类，不直接暴露给 TJS；
// 本文件提供同语义的手写实现（无外部依赖），供 PackinOne 的
// StoragesFstat/存档等内部路径与未来需要处使用。
//
// 仅实现解压（游戏运行期只需要读）；压缩端保留接口但标为未实现。
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace packinone {

// 解压一个 LZ4 Frame（magic 0x184D2204，支持 content length /
// block checksum / content checksum，64KB~4MB 块）。返回原始字节。
std::vector<std::uint8_t> Lz4FrameDecompress(const std::uint8_t *data,
                                             std::size_t size);

// 解压单个 LZ4 块（token/literal/match 序列）。返回解压后字节。
std::vector<std::uint8_t> Lz4BlockDecompress(const std::uint8_t *data,
                                             std::size_t size,
                                             std::size_t outCapacity);

} // namespace packinone
