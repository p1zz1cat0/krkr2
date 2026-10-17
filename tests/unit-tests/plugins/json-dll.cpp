//
// json UTF-8 编码器回归测试：直接包含插件源文件，不依赖 TJS 运行时。
// 原生层只处理 BMP 的转换助手会把代理对编码成两个三字节序列，这里用
// emoji（R != 相邻字符）钉住合并行为，并覆盖 ASCII/2/3/4 字节边界。
//

#include <cstring>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "json/json.cpp"

TEST_CASE("json encodeUtf8 handles all byte widths") {
    const tjs_char ascii[] = { TJS_W('A'), 0 };
    const tjs_char two[] = { 0x00e9, 0 };      // U+00E9
    const tjs_char three[] = { 0x3042, 0 };    // U+3042
    const tjs_char four[] = { static_cast<tjs_char>(0xd83d),
                              static_cast<tjs_char>(0xde00), 0 }; // U+1F600

    CHECK(encodeUtf8(ascii, 1) == "A");
    CHECK(encodeUtf8(two, 1) == std::string("\xc3\xa9"));
    CHECK(encodeUtf8(three, 1) == std::string("\xe3\x81\x82"));
    CHECK(encodeUtf8(four, 2) == std::string("\xf0\x9f\x98\x80"));
}

TEST_CASE("json encodeUtf8 does not emit a BOM") {
    const tjs_char text[] = { TJS_W('{'), TJS_W('}'), 0 };
    const std::string encoded = encodeUtf8(text, 2);
    REQUIRE(encoded.size() == 2);
    CHECK(static_cast<unsigned char>(encoded[0]) == '{');
}
