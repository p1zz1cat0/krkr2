// lz4_stream.cpp — 手写 LZ4 解压（无外部依赖）。
//
// 算法与 krkr2 psbfile 插件的 PSBLz4Stream 一致（同一份已验证的
// token/literal/match 解析），这里补上 Frame 层封装。压缩端（写）在
// PackinOne 场景不需要（runtime 只读游戏资源），故不实现。

#include "lz4_stream.h"

#include <cstring>
#include <stdexcept>

namespace packinone {
namespace {

constexpr int kMinMatch = 4;
constexpr int kLastLiterals = 5;
constexpr int kMFLimit = 12;
constexpr int kMatchLengthBits = 4;
constexpr int kMatchLengthMask = 0xF;
constexpr int kRunMask = 0xF;

std::uint32_t ReadLE32(const std::uint8_t *p) {
    return (std::uint32_t)p[0] | ((std::uint32_t)p[1] << 8) |
           ((std::uint32_t)p[2] << 16) | ((std::uint32_t)p[3] << 24);
}

void CopyOverlapped(std::uint8_t *data, int src, int dst, int count) {
    if(count <= 0)
        return;
    if(dst > src) {
        while(count > 0) {
            const int chunk = std::min(dst - src, count);
            std::memcpy(data + dst, data + src, chunk);
            dst += chunk;
            count -= chunk;
        }
    } else {
        std::memcpy(data + dst, data + src, count);
    }
}

int DecompressBlock(const std::uint8_t *block, int blockSize,
                    std::uint8_t *output, int outputSize) {
    int src = 0;
    const int iend = blockSize;
    int dst = 0;
    const int oend = outputSize;

    for(;;) {
        const int token = block[src++];
        int length = token >> kMatchLengthBits;
        if(kRunMask == length) {
            int n = 0;
            do {
                n = block[src++];
                length += n;
            } while((src < iend - kRunMask) && (0xFF == n));
            if(dst + length < dst || src + length < src)
                throw std::runtime_error("Invalid LZ4 compressed stream.");
        }

        const int copyEnd = dst + length;
        if((copyEnd > oend - kMFLimit) ||
           (src + length > iend - (3 + kLastLiterals))) {
            if((src + length != iend) || copyEnd > oend)
                throw std::runtime_error("Invalid LZ4 compressed stream.");
            std::memcpy(output + dst, block + src, length);
            src += length;
            dst += length;
            break;
        }
        std::memcpy(output + dst, block + src, length);
        src += length;
        dst = copyEnd;

        std::uint16_t recOffset = 0;
        std::memcpy(&recOffset, block + src, 2);
        const int offset = recOffset;
        src += 2;
        const int match = dst - offset;
        if(match < 0)
            throw std::runtime_error("Invalid LZ4 compressed stream.");

        length = token & kMatchLengthMask;
        if(kMatchLengthMask == length) {
            int n = 0;
            do {
                n = block[src++];
                if(src > iend - kLastLiterals)
                    throw std::runtime_error(
                        "Invalid LZ4 compressed stream.");
                length += n;
            } while(0xFF == n);
            if(dst + length < dst)
                throw std::runtime_error("Invalid LZ4 compressed stream.");
        }
        length += kMinMatch;

        CopyOverlapped(output, match, dst, length);
        dst += length;
    }
    return dst;
}

} // namespace

std::vector<std::uint8_t> Lz4BlockDecompress(const std::uint8_t *data,
                                             std::size_t size,
                                             std::size_t outCapacity) {
    std::vector<std::uint8_t> out(outCapacity);
    const int written = DecompressBlock(data, (int)size, out.data(),
                                        (int)outCapacity);
    out.resize(written);
    return out;
}

std::vector<std::uint8_t> Lz4FrameDecompress(const std::uint8_t *data,
                                             std::size_t size) {
    if(size < 13 || ReadLE32(data) != 0x184D2204)
        throw std::runtime_error("Invalid LZ4 frame magic.");

    std::size_t pos = 4;
    const std::uint8_t flags = data[pos++];
    const int version = flags >> 6;
    if(version != 1)
        throw std::runtime_error("Invalid LZ4 frame version.");
    const bool independent = 0 != (flags & 0x20);
    const bool blockChecksum = 0 != (flags & 0x10);
    const bool contentLength = 0 != (flags & 8);
    const bool contentChecksum = 0 != (flags & 4);
    const bool hasDict = 0 != (flags & 1);
    (void)independent;

    const std::uint8_t bd = data[pos++];
    std::size_t blockSize = 0;
    switch((bd >> 4) & 7) {
        case 4: blockSize = 0x10000; break;
        case 5: blockSize = 0x40000; break;
        case 6: blockSize = 0x100000; break;
        case 7: blockSize = 0x400000; break;
        default: throw std::runtime_error("Invalid LZ4 block size.");
    }

    std::size_t outCapacity = 0;
    if(contentLength) {
        if(pos + 8 > size)
            throw std::runtime_error("Truncated LZ4 frame.");
        std::uint64_t len = ReadLE32(data + pos);
        len |= (std::uint64_t)ReadLE32(data + pos + 4) << 32;
        pos += 8;
        outCapacity = (std::size_t)len;
    }
    if(hasDict) {
        if(pos + 4 > size)
            throw std::runtime_error("Truncated LZ4 frame.");
        pos += 4;
    }
    if(pos + 1 > size)
        throw std::runtime_error("Truncated LZ4 frame.");
    pos += 1; // header checksum

    std::vector<std::uint8_t> out;
    out.reserve(outCapacity ? outCapacity : size * 4);
    std::vector<std::uint8_t> block(blockSize);

    for(;;) {
        if(pos + 4 > size)
            throw std::runtime_error("Truncated LZ4 frame.");
        const std::int32_t blockLen = (std::int32_t)ReadLE32(data + pos);
        pos += 4;
        if(blockLen == 0) {
            if(contentChecksum && pos + 4 <= size)
                pos += 4;
            break;
        }
        if(blockLen < 0) {
            const int rawLen = blockLen & 0x7FFFFFFF;
            if(pos + rawLen > size)
                throw std::runtime_error("Truncated LZ4 block.");
            out.insert(out.end(), data + pos, data + pos + rawLen);
            pos += rawLen;
        } else {
            if(pos + blockLen > size)
                throw std::runtime_error("Truncated LZ4 block.");
            if((std::size_t)blockLen > block.size())
                block.resize(blockLen);
            const int written =
                DecompressBlock(data + pos, blockLen, block.data(),
                                (int)block.size());
            out.insert(out.end(), block.begin(), block.begin() + written);
            pos += blockLen;
        }
        if(blockChecksum) {
            if(pos + 4 > size)
                throw std::runtime_error("Truncated LZ4 frame.");
            pos += 4;
        }
    }
    return out;
}

} // namespace packinone
