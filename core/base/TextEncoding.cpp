// Encoding detection for text without a BOM. This file only uses uchardet,
// so unit tests can link it without the full engine.
#include <cstdint>
#include <string>
#include <uchardet.h>

#include "TextEncoding.h"

// 严格校验 UTF-8 序列（含多字节字符的完整性与合法性）
static bool isValidUTF8(const unsigned char *raw, size_t size) {
    size_t i = 0;
    while(i < size) {
        unsigned char c = raw[i];
        if(c < 0x80) {
            i++;
        } else if(c >= 0xC2 && c <= 0xDF) {
            if(i + 1 >= size || (raw[i + 1] & 0xC0) != 0x80)
                return false;
            i += 2;
        } else if(c >= 0xE0 && c <= 0xEF) {
            if(i + 2 >= size || (raw[i + 1] & 0xC0) != 0x80 ||
               (raw[i + 2] & 0xC0) != 0x80)
                return false;
            if(c == 0xE0 && raw[i + 1] < 0xA0)
                return false;
            if(c == 0xED && raw[i + 1] >= 0xA0)
                return false;
            i += 3;
        } else if(c >= 0xF0 && c <= 0xF4) {
            if(i + 3 >= size || (raw[i + 1] & 0xC0) != 0x80 ||
               (raw[i + 2] & 0xC0) != 0x80 || (raw[i + 3] & 0xC0) != 0x80)
                return false;
            if(c == 0xF0 && raw[i + 1] < 0x90)
                return false;
            if(c == 0xF4 && raw[i + 1] >= 0x90)
                return false;
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

std::string checkTextEncoding(const void *buf, size_t size,
                              std::uint8_t &bomSize) {
    auto raw = static_cast<const unsigned char *>(buf);
    std::string encoding;
    // uchardet 对几乎纯 ASCII 的短文本（如立绘 .stand 配置）经常无法判定
    // 或误判。若内容含非 ASCII 高位字节，说明是日文 SJIS/cp932 编码
    // （日本游戏脚本常见），按 cp932 解析更稳妥；全 ASCII 才按 ASCII 处理。
    auto hasHighByte = [&]() {
        for(size_t i = 0; i < size; ++i) {
            if(raw[i] >= 0x80) {
                return true;
            }
        }
        return false;
    };
    // --- 检查 BOM ---
    if(size >= 4 && raw[0] == 0xFF && raw[1] == 0xFE && raw[2] == 0x00 &&
       raw[3] == 0x00) {
        // UTF-32LE BOM
        bomSize = 4;
        encoding = "UTF-32LE";
    } else if(size >= 4 && raw[0] == 0x00 && raw[1] == 0x00 && raw[2] == 0xFE &&
              raw[3] == 0xFF) {
        // UTF-32BE BOM
        bomSize = 4;
        encoding = "UTF-32BE";
    } else if(size >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        // UTF-16LE BOM
        bomSize = 2;
        encoding = "UTF-16LE";
    } else if(size >= 2 && raw[0] == 0xFE && raw[1] == 0xFF) {
        // UTF-16BE BOM
        bomSize = 2;
        encoding = "UTF-16BE";
    } else if(size >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
        // UTF-8 BOM
        bomSize = 3;
        encoding = "UTF-8";
    } else {
        // ---------- 普通文本：用 uchardet 检测编码 ----------
        uchardet_t ud = uchardet_new();
        uchardet_handle_data(ud, reinterpret_cast<const char *>(raw), size);
        uchardet_data_end(ud);
        encoding = uchardet_get_charset(ud);
        uchardet_delete(ud);
        // uchardet gives single-byte code pages for short SJIS text. For
        // example, 9-nine 翔.stand ("翔Ａ" = E3 C4 82 60) gives WINDOWS-1256.
        // KiriKiri games do not use these code pages, so do not trust them.
        auto isSingleByteGuess = [](const std::string &e) {
            return e.empty() || e.rfind("WINDOWS-125", 0) == 0 ||
                e.rfind("ISO-8859-", 0) == 0 || e.rfind("IBM", 0) == 0 ||
                e.rfind("MAC-", 0) == 0 || e.rfind("KOI8-", 0) == 0;
        };
        if(encoding == "SHIFT_JIS") {
            encoding = "cp932";
        } else if(isSingleByteGuess(encoding)) {
            // Decide from the content: ASCII only → ASCII; strict
            // UTF-8 → UTF-8; other high bytes → cp932.
            if(!hasHighByte()) {
                encoding = "ASCII";
            } else if(isValidUTF8(raw, size)) {
                encoding = "UTF-8";
            } else {
                encoding = "cp932";
            }
        }
    }

    return encoding;
}
