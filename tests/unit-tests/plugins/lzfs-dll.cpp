//
// lzfs 解码核心回归：直接包含插件源文件，用官方 LZ4F 生成样本，
// 断言"完整帧解压出原文 / 其余一律判负（raw 透传）"的判定与内容。
// 不依赖 TJS 运行时，媒体集成由 tests/test_files/lzfs 的 smoke 覆盖。
//

#include <catch2/catch_test_macros.hpp>

#include <lz4frame.h>

#include <cstdint>
#include <vector>

#include "lzfs/lzfs.cpp"

namespace {

using Bytes = std::vector<tjs_uint8>;

// 用官方 LZ4F 生成一个完整 frame（按给定的 preferences）。
Bytes makeFrame(const Bytes &payload, const LZ4F_preferences_t &prefs) {
    Bytes frame(LZ4F_compressFrameBound(payload.size(), &prefs));
    const size_t written = LZ4F_compressFrame(
        frame.data(), frame.size(), payload.data(), payload.size(), &prefs);
    REQUIRE(!LZ4F_isError(written));
    frame.resize(written);
    return frame;
}

// 可压但不平凡的数据：重复句子 + 逐个位置变化的字节，压缩后必须真的
// 走 LZ4 块解码（全 stored block 的"假解压"测不出解码错误）。
Bytes testPayload(size_t size) {
    static const char pattern[] = "the quick brown fox jumps over the dog. ";
    Bytes payload(size);
    for(size_t i = 0; i < size; ++i) {
        payload[i] = static_cast<tjs_uint8>(
            pattern[i % (sizeof(pattern) - 1)] ^ ((i / 64) & 0x55));
    }
    return payload;
}

// 伪随机（不可压）数据：让 LZ4F 走 stored block / 多输出块路径。
Bytes noisePayload(size_t size) {
    Bytes payload(size);
    std::uint64_t state = 0x2545F4914F6CDD1Dull;
    for(size_t i = 0; i < size; ++i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        payload[i] = static_cast<tjs_uint8>(state >> 24);
    }
    return payload;
}

bool decode(const Bytes &bytes, Bytes &decoded) {
    tTVPMemoryStream stream;
    if(!bytes.empty())
        stream.WriteBuffer(bytes.data(), static_cast<tjs_uint>(bytes.size()));
    stream.Seek(0, TJS_BS_SEEK_SET);
    return decodeLz4Frame(&stream, decoded);
}

} // namespace

TEST_CASE("lzfs decodes a complete LZ4 frame byte-identically") {
    const Bytes payload = testPayload(8 * 1024);
    const Bytes frame = makeFrame(payload, LZ4F_preferences_t{});

    Bytes decoded;
    REQUIRE(decode(frame, decoded));
    CHECK(decoded == payload);
}

TEST_CASE("lzfs honors checksums, content size and linked blocks") {
    const Bytes payload = testPayload(70 * 1024);
    LZ4F_preferences_t prefs{};
    prefs.frameInfo.blockMode = LZ4F_blockLinked;
    prefs.frameInfo.blockChecksumFlag = LZ4F_blockChecksumEnabled;
    prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
    prefs.frameInfo.contentSize = payload.size();
    const Bytes frame = makeFrame(payload, prefs);

    Bytes decoded;
    REQUIRE(decode(frame, decoded));
    CHECK(decoded == payload);
}

TEST_CASE("lzfs decodes multi-block incompressible payloads") {
    // 300 KiB 噪声：帧必须走 stored block 且输出超过一个 64 KiB 解压块，
    // 覆盖读块循环的多次迭代。
    const Bytes payload = noisePayload(300 * 1024);
    const Bytes frame = makeFrame(payload, LZ4F_preferences_t{});

    Bytes decoded;
    REQUIRE(decode(frame, decoded));
    CHECK(decoded == payload);
}

TEST_CASE("lzfs rejects a frame whose content checksum is broken") {
    const Bytes payload = testPayload(4 * 1024);
    LZ4F_preferences_t prefs{};
    prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
    Bytes frame = makeFrame(payload, prefs);
    frame.back() ^= 0xff; // 帧尾是 content checksum

    Bytes decoded;
    CHECK_FALSE(decode(frame, decoded));
}

TEST_CASE("lzfs rejects truncated frames at any cut") {
    const Bytes payload = testPayload(4 * 1024);
    const Bytes frame = makeFrame(payload, LZ4F_preferences_t{});

    for(size_t cut : {size_t{1}, size_t{4}, size_t{16}, frame.size() / 2,
                      frame.size() - 1}) {
        Bytes decoded;
        CHECK_FALSE(decode(Bytes(frame.begin(),
                                 frame.begin() + static_cast<ptrdiff_t>(cut)),
                           decoded));
    }
}

TEST_CASE("lzfs rejects payloads that are not a complete LZ4 frame") {
    const Bytes cases[] = {
        {},
        {0x00, 0x01, 0x02, 0x03},
        {0x04, 0x22, 0x4d, 0x18}, // 只有 magic
        {0x04, 0x22, 0x4d, 0x18, 0x40, 0x40}, // magic + 截断的 descriptor
    };
    for(const Bytes &bytes : cases) {
        Bytes decoded;
        CHECK_FALSE(decode(bytes, decoded));
    }
}

TEST_CASE("lzfs ignores bytes after a complete frame") {
    const Bytes payload = testPayload(2 * 1024);
    Bytes frame = makeFrame(payload, LZ4F_preferences_t{});
    frame.insert(frame.end(), {0x00, 0xff, 0x13, 0x37});

    Bytes decoded;
    REQUIRE(decode(frame, decoded));
    CHECK(decoded == payload);
}

TEST_CASE("lzfs decodes only the first of two concatenated frames") {
    const Bytes first = testPayload(1 * 1024);
    const Bytes second = testPayload(3 * 1024);
    Bytes stream = makeFrame(first, LZ4F_preferences_t{});
    const Bytes secondFrame = makeFrame(second, LZ4F_preferences_t{});
    stream.insert(stream.end(), secondFrame.begin(), secondFrame.end());

    Bytes decoded;
    REQUIRE(decode(stream, decoded));
    CHECK(decoded == first);
}
