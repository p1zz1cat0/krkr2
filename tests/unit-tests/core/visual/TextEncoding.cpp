// Text encoding detection for files without a BOM (core/base/TextEncoding.cpp).
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

#include "TextEncoding.h"

namespace {

std::string Detect(const std::string &bytes) {
    std::uint8_t bom = 0;
    return checkTextEncoding(bytes.data(), bytes.size(), bom);
}

} // namespace

TEST_CASE("short SJIS stand file is cp932, not a single-byte code page") {
    // 9-nine fgimage/翔.stand. uchardet gives WINDOWS-1256 for it, so
    // "翔Ａ.sinfo" was not found and StandImage threw.
    const std::string sho =
        "[%[\r\n \"facexoff\"=>-15,\r\n \"faceyoff\"=>-226,\r\n"
        " \"filename\"=>\"\xE3\xC4\x82\x60\",\r\n"
        " \"leveloffset\"=>[[0,104],[0,0],[0,-79],[0,-207]],\r\n"
        " \"xoffset\"=>-12,\r\n \"yoffset\"=>504],void]\r\n";
    REQUIRE(Detect(sho) == "cp932");
}

TEST_CASE("ASCII and UTF-8 text keep their encodings") {
    REQUIRE(Detect("[%[\"x\"=>1],void]") == "ASCII");
    REQUIRE(Detect("\"filename\"=>\"\xE7\xBF\x94\xEF\xBC\xA1\"") == "UTF-8");
}
