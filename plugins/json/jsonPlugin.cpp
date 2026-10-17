// json.dll
//
// 移植自 krkrsdl3 的 plugins/json.cpp（原作者 Go Watanabe，许可证遵循
// 吉里吉里本体）。解析器保留原实现的宽松语法：注释、单引号字符串、
// '=' / '=>' 键值分隔符、分号分隔、尾部逗号等。
//
// 对外接口（挂到 Scripts 类）：
//   Scripts.evalJSON(text)
//   Scripts.evalJSONStorage(filename, utf8 = false)
//   Scripts.saveJSON(filename, value, utf8 = false, newline = 0)
//   Scripts.toJSONString(value, newline = 0)
//
// 本 fork 的 tjs_char 是 UTF-16，原实现的字节流读写改成：
//   - 读取走 TVPCreateTextStreamForRead，由核心处理编码检测。
//   - 写入 utf8=true 时为 UTF-8；false 时为 UTF-16LE。两者都不写 BOM，
//     与 Windows 版 json.dll 的行为一致。

#include "tp_stub.h"
#include "ncbind.hpp"
#include "BinaryStream.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#define NCB_MODULE_NAME TJS_W("json.dll")

namespace {

constexpr tjs_char kUnicodeBom = 0xfeff;

//----------------------------------------------------------------------------
// Parser
//----------------------------------------------------------------------------

class JsonReader {
public:
    explicit JsonReader(const ttstr &text) : _text(text), _pos(0) {}

    bool eof() const { return _pos >= _text.GetLen(); }

    tjs_char peek() const { return eof() ? 0 : _text[_pos]; }

    tjs_char get() { return eof() ? 0 : _text[_pos++]; }

    void unget() {
        if(_pos > 0)
            --_pos;
    }

    bool consume(tjs_char ch) {
        next();
        if(peek() != ch)
            return false;
        ++_pos;
        return true;
    }

    // Skip whitespace and the comment forms accepted by the original plugin.
    tjs_char next() {
        for(;;) {
            const tjs_char ch = get();
            if(ch == 0)
                return 0;
            if(ch == kUnicodeBom)
                continue;
            if(ch == TJS_W('#')) {
                skipToEol();
                continue;
            }
            if(ch == TJS_W('/')) {
                const tjs_char next_ch = get();
                if(next_ch == TJS_W('/')) {
                    skipToEol();
                    continue;
                }
                if(next_ch == TJS_W('*')) {
                    for(;;) {
                        const tjs_char c = get();
                        if(c == 0)
                            fail(TJS_W("unterminated comment"));
                        if(c == TJS_W('*')) {
                            if(get() == TJS_W('/'))
                                break;
                            unget();
                        }
                    }
                    continue;
                }
                unget();
                return TJS_W('/');
            }
            if(ch > TJS_W(' '))
                return ch;
        }
    }

    void fail(const tjs_char *message) {
        TVPThrowExceptionMessage(message);
    }

    tTJSVariant parse() {
        tTJSVariant value;
        parseValue(value);
        if(next() != 0)
            fail(TJS_W("JSON parse error: trailing input"));
        return value;
    }

private:
    void skipToEol() {
        for(;;) {
            const tjs_char c = get();
            if(c == 0 || c == TJS_W('\r') || c == TJS_W('\n'))
                return;
        }
    }

    void parseValue(tTJSVariant &value) {
        const tjs_char ch = next();
        switch(ch) {
            case TJS_W('"'):
            case TJS_W('\''):
                parseString(ch, value);
                return;
            case TJS_W('{'):
                parseObject(value);
                return;
            case TJS_W('['):
                parseArray(value);
                return;
            case 0:
                fail(TJS_W("JSON parse error: unexpected end of input"));
            default:
                break;
        }

        if(ch == TJS_W('-') || ch == TJS_W('+') || ch == TJS_W('.') ||
           (ch >= TJS_W('0') && ch <= TJS_W('9'))) {
            parseNumber(ch, value);
            return;
        }

        if(ch >= TJS_W('a') && ch <= TJS_W('z')) {
            ttstr word;
            tjs_char c = ch;
            while(c >= TJS_W('a') && c <= TJS_W('z')) {
                word += c;
                c = get();
            }
            unget();

            if(word == TJS_W("true"))
                value = true;
            else if(word == TJS_W("false"))
                value = false;
            else if(word == TJS_W("null") || word == TJS_W("void"))
                value.Clear();
            else {
                ttstr message = TJS_W("JSON parse error: unknown keyword '");
                message += word;
                message += TJS_W("'");
                TVPThrowExceptionMessage(message.c_str());
            }
            return;
        }

        fail(TJS_W("JSON parse error: invalid value"));
    }

    void parseObject(tTJSVariant &value) {
        iTJSDispatch2 *dict = TJSCreateDictionaryObject();
        value = tTJSVariant(dict, dict);
        dict->Release();

        for(;;) {
            const tjs_char ch = next();
            if(ch == 0)
                fail(TJS_W("JSON parse error: object must end with '}'"));
            if(ch == TJS_W('}'))
                return;

            if(ch != TJS_W(',') && ch != TJS_W(';')) {
                unget();
                tTJSVariant key;
                parseKey(key);

                const tjs_char sep = next();
                if(sep == TJS_W('=')) {
                    if(get() != TJS_W('>'))
                        unget();
                } else if(sep != TJS_W(':')) {
                    fail(TJS_W("JSON parse error: expected ':' after key"));
                }

                tTJSVariant member;
                parseValue(member);
                dict->PropSet(TJS_MEMBERENSURE, key.GetString(), nullptr,
                              &member, dict);
            }

            const tjs_char tail = next();
            if(tail == TJS_W(',' ) || tail == TJS_W(';'))
                continue;
            if(tail == TJS_W('}'))
                return;
            fail(TJS_W("JSON parse error: expected ',' or '}'"));
        }
    }

    void parseKey(tTJSVariant &key) {
        const tjs_char ch = next();
        if(ch == TJS_W('"') || ch == TJS_W('\'')) {
            parseString(ch, key);
            return;
        }

        // Bare keys are accepted for the historical non-strict syntax.
        ttstr name;
        tjs_char c = ch;
        while(c != 0 && c != TJS_W(':') && c != TJS_W('=') &&
              c != TJS_W(' ') && c != TJS_W('\t') && c != TJS_W('\r') &&
              c != TJS_W('\n')) {
            name += c;
            c = get();
        }
        unget();
        if(name.IsEmpty())
            fail(TJS_W("JSON parse error: empty object key"));
        key = name;
    }

    void parseArray(tTJSVariant &value) {
        iTJSDispatch2 *array = TJSCreateArrayObject();
        value = tTJSVariant(array, array);
        array->Release();

        tjs_int index = 0;
        for(;;) {
            const tjs_char ch = next();
            if(ch == 0)
                fail(TJS_W("JSON parse error: array must end with ']'"));
            if(ch == TJS_W(']'))
                return;
            if(ch == TJS_W(',') || ch == TJS_W(';')) {
                // Preserve the original plugin's empty-column behavior.
                tTJSVariant empty;
                array->PropSetByNum(TJS_MEMBERENSURE, index++, &empty, array);
            } else {
                unget();
                tTJSVariant element;
                parseValue(element);
                array->PropSetByNum(TJS_MEMBERENSURE, index++, &element, array);
            }

            const tjs_char tail = next();
            if(tail == TJS_W(',' ) || tail == TJS_W(';'))
                continue;
            if(tail == TJS_W(']'))
                return;
            fail(TJS_W("JSON parse error: expected ',' or ']'"));
        }
    }

    static int hexValue(tjs_char ch) {
        if(ch >= TJS_W('0') && ch <= TJS_W('9'))
            return ch - TJS_W('0');
        if(ch >= TJS_W('a') && ch <= TJS_W('f'))
            return ch - TJS_W('a') + 10;
        if(ch >= TJS_W('A') && ch <= TJS_W('F'))
            return ch - TJS_W('A') + 10;
        return -1;
    }

    unsigned readHexEscape(int digits) {
        unsigned value = 0;
        for(int i = 0; i < digits; ++i) {
            const int digit = hexValue(get());
            if(digit < 0)
                fail(TJS_W("JSON parse error: invalid hex escape"));
            value = (value << 4) | static_cast<unsigned>(digit);
        }
        return value;
    }

    void parseString(tjs_char quote, tTJSVariant &value) {
        ttstr text;
        for(;;) {
            const tjs_char ch = get();
            if(ch == 0 || ch == TJS_W('\n') || ch == TJS_W('\r'))
                fail(TJS_W("JSON parse error: unterminated string"));
            if(ch == quote) {
                value = text;
                return;
            }
            if(ch != TJS_W('\\')) {
                text += ch;
                continue;
            }

            const tjs_char escaped = get();
            switch(escaped) {
                case TJS_W('b'):
                    text += TJS_W('\b');
                    break;
                case TJS_W('f'):
                    text += TJS_W('\f');
                    break;
                case TJS_W('n'):
                    text += TJS_W('\n');
                    break;
                case TJS_W('r'):
                    text += TJS_W('\r');
                    break;
                case TJS_W('t'):
                    text += TJS_W('\t');
                    break;
                case TJS_W('u'): {
                    unsigned code = readHexEscape(4);
                    if(code >= 0xd800 && code <= 0xdbff && peek() == TJS_W('\\')) {
                        // Combine a UTF-16 surrogate pair written as two
                        // \u escapes.
                        const tjs_char slash = get();
                        if(slash == TJS_W('\\') && peek() == TJS_W('u')) {
                            ++_pos; // 'u'
                            const unsigned low = readHexEscape(4);
                            if(low >= 0xdc00 && low <= 0xdfff) {
                                const char32_t combined =
                                    0x10000 +
                                    ((code - 0xd800) << 10) + (low - 0xdc00);
                                const tjs_char buf[2] = {
                                    static_cast<tjs_char>(
                                        0xd800 + ((combined - 0x10000) >> 10)),
                                    static_cast<tjs_char>(
                                        0xdc00 + ((combined - 0x10000) & 0x3ff))
                                };
                                text += buf[0];
                                text += buf[1];
                                break;
                            }
                            text += static_cast<tjs_char>(code);
                            text += static_cast<tjs_char>(low);
                            break;
                        }
                        text += static_cast<tjs_char>(code);
                        break;
                    }
                    text += static_cast<tjs_char>(code);
                    break;
                }
                case TJS_W('x'):
                    text += static_cast<tjs_char>(readHexEscape(2));
                    break;
                case 0:
                    fail(TJS_W("JSON parse error: unterminated escape"));
                default:
                    // Original plugin passes unknown escapes through.
                    text += escaped;
                    break;
            }
        }
    }

    void parseNumber(tjs_char ch, tTJSVariant &value) {
        ttstr token;
        bool is_real = false;
        tjs_char c = ch;
        while(c == TJS_W('+') || c == TJS_W('-') || c == TJS_W('.') ||
              c == TJS_W('e') || c == TJS_W('E') ||
              (c >= TJS_W('0') && c <= TJS_W('9'))) {
            if(c == TJS_W('.') || c == TJS_W('e') || c == TJS_W('E'))
                is_real = true;
            token += c;
            c = get();
        }
        unget();

        const std::string narrow = token.AsStdString();
        char *end = nullptr;
        if(is_real) {
            const double parsed = std::strtod(narrow.c_str(), &end);
            if(end == narrow.c_str() || *end != 0)
                fail(TJS_W("JSON parse error: invalid number"));
            value = parsed;
        } else {
            const long long parsed = std::strtoll(narrow.c_str(), &end, 10);
            if(end == narrow.c_str() || *end != 0)
                fail(TJS_W("JSON parse error: invalid number"));
            value = static_cast<tTVInteger>(parsed);
        }
    }

    ttstr _text;
    tjs_uint _pos;
};

//----------------------------------------------------------------------------
// Writer
//----------------------------------------------------------------------------

class JsonWriter {
public:
    explicit JsonWriter(tjs_int newline) : _newline(newline == 1 ? TJS_W("\n")
                                                                : TJS_W("\r\n")) {}

    ttstr stringify(const tTJSVariant &value) {
        writeValue(value);
        return _output;
    }

private:
    void write(const tjs_char *text) { _output += text; }

    void write(tjs_char ch) { _output += ch; }

    void newline() {
        _output += _newline;
        for(tjs_int i = 0; i < _indent; ++i)
            _output += TJS_W(' ');
    }

    void writeString(const tjs_char *text) {
        if(!text) {
            write(TJS_W("\"\""));
            return;
        }
        write(TJS_W('"'));
        for(const tjs_char *p = text; *p; ++p) {
            const tjs_char ch = *p;
            switch(ch) {
                case TJS_W('"'):
                    write(TJS_W("\\\""));
                    break;
                case TJS_W('\\'):
                    write(TJS_W("\\\\"));
                    break;
                case TJS_W('\b'):
                    write(TJS_W("\\b"));
                    break;
                case TJS_W('\f'):
                    write(TJS_W("\\f"));
                    break;
                case TJS_W('\n'):
                    write(TJS_W("\\n"));
                    break;
                case TJS_W('\r'):
                    write(TJS_W("\\r"));
                    break;
                case TJS_W('\t'):
                    write(TJS_W("\\t"));
                    break;
                default:
                    if(ch < 0x20) {
                        static const tjs_char digits[] =
                            TJS_W("0123456789abcdef");
                        write(TJS_W("\\u"));
                        for(int shift = 12; shift >= 0; shift -= 4)
                            write(digits[(ch >> shift) & 0xf]);
                    } else {
                        write(ch);
                    }
                    break;
            }
        }
        write(TJS_W('"'));
    }

    void writeObject(iTJSDispatch2 *object) {
        write(TJS_W('{'));
        ++_indent;
        const bool parent_first = _first_member;
        _first_member = true;
        MemberWriter *writer = new MemberWriter(this);
        tTJSVariantClosure closure(writer);
        object->EnumMembers(TJS_IGNOREPROP, &closure, object);
        writer->Release();
        const bool empty = _first_member;
        --_indent;
        if(!empty)
            newline();
        _first_member = parent_first;
        write(TJS_W('}'));
    }

    void writeArray(iTJSDispatch2 *array) {
        write(TJS_W('['));
        ++_indent;

        tjs_int count = 0;
        tTJSVariant count_value;
        if(TJS_SUCCEEDED(objectProperty(array, TJS_W("count"), count_value)))
            count = static_cast<tjs_int>(count_value.AsInteger());

        if(count > 0)
            newline();
        for(tjs_int i = 0; i < count; ++i) {
            if(i != 0) {
                write(TJS_W(','));
                newline();
            }
            tTJSVariant element;
            if(TJS_FAILED(array->PropGetByNum(TJS_IGNOREPROP, i, &element,
                                              array)))
                element.Clear();
            writeValue(element);
        }

        --_indent;
        if(count > 0)
            newline();
        write(TJS_W(']'));
    }

    static tjs_error objectProperty(iTJSDispatch2 *object, const tjs_char *name,
                                    tTJSVariant &value) {
        return object->PropGet(0, name, nullptr, &value, object);
    }

    void writeValue(const tTJSVariant &value) {
        switch(value.Type()) {
            case tvtVoid:
                write(TJS_W("null"));
                break;
            case tvtObject: {
                iTJSDispatch2 *object = value.AsObjectNoAddRef();
                if(!object) {
                    write(TJS_W("null"));
                } else if(object->IsInstanceOf(TJS_IGNOREPROP, nullptr,
                                               nullptr, TJS_W("Array"),
                                               object) == TJS_S_TRUE) {
                    writeArray(object);
                } else {
                    writeObject(object);
                }
                break;
            }
            case tvtString:
                writeString(value.GetString());
                break;
            case tvtInteger: {
                tTJSVariantString *str =
                    TJSIntegerToString(value.AsInteger());
                write(static_cast<const tjs_char *>(*str));
                str->Release();
                break;
            }
            case tvtReal: {
                tTJSVariantString *str = TJSRealToString(value.AsReal());
                const tjs_char *text = static_cast<const tjs_char *>(*str);
                if(text && text[0] == TJS_W('+'))
                    ++text;
                write(text);
                str->Release();
                break;
            }
            default:
                write(TJS_W("null"));
                break;
        }
    }

    class MemberWriter : public tTJSDispatch {
    public:
        explicit MemberWriter(JsonWriter *owner) : _owner(owner) {}

        tjs_error FuncCall(tjs_uint32, const tjs_char *, tjs_uint32 *,
                           tTJSVariant *result, tjs_int numparams,
                           tTJSVariant **param, iTJSDispatch2 *) override {
            if(numparams > 2) {
                const tTVInteger flags = param[1]->AsInteger();
                if(!(flags & TJS_HIDDENMEMBER)) {
                    if(!_owner->_first_member) {
                        _owner->write(TJS_W(','));
                        _owner->newline();
                    } else {
                        _owner->newline();
                    }
                    _owner->_first_member = false;
                    _owner->writeString(param[0]->GetString());
                    _owner->write(TJS_W(':'));
                    _owner->writeValue(*param[2]);
                }
            }
            if(result)
                *result = true;
            return TJS_S_OK;
        }

    private:
        JsonWriter *_owner;
    };

    ttstr _output;
    const tjs_char *_newline;
    tjs_int _indent = 0;
    bool _first_member = true;
};

//----------------------------------------------------------------------------
// File helpers
//----------------------------------------------------------------------------

ttstr loadText(const ttstr &filename, bool utf8) {
    std::unique_ptr<iTJSTextReadStream> stream(TVPCreateTextStreamForRead(
        filename, utf8 ? TJS_W("utf-8") : TJS_W("")));
    if(!stream)
        TVPThrowExceptionMessage(TJS_W("json.dll: cannot open file"));
    ttstr text;
    stream->Read(text, 0);
    return text;
}

// Encode UTF-16 to UTF-8 without a BOM.  The core helper handles only BMP
// code units, so surrogate pairs are combined here.
std::string encodeUtf8(const tjs_char *p, tjs_uint length) {
    std::string out;
    for(tjs_uint i = 0; i < length; ++i) {
        char32_t code = p[i];
        if(code >= 0xd800 && code <= 0xdbff && i + 1 < length) {
            const char32_t low = p[i + 1];
            if(low >= 0xdc00 && low <= 0xdfff) {
                code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                ++i;
            }
        }

        if(code < 0x80) {
            out += static_cast<char>(code);
        } else if(code < 0x800) {
            out += static_cast<char>(0xc0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3f));
        } else if(code < 0x10000) {
            out += static_cast<char>(0xe0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (code & 0x3f));
        } else {
            out += static_cast<char>(0xf0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3f));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (code & 0x3f));
        }
    }
    return out;
}

void saveText(const ttstr &filename, const ttstr &text, bool utf8) {
    std::unique_ptr<tTJSBinaryStream> stream(
        TVPCreateBinaryStreamForWrite(filename, TJS_W("")));
    if(!stream)
        TVPThrowExceptionMessage(TJS_W("json.dll: cannot open file"));

    if(utf8) {
        const std::string bytes = encodeUtf8(text.c_str(), text.GetLen());
        if(!bytes.empty())
            stream->Write(bytes.data(), bytes.size());
        return;
    }

    // Legacy behavior: UTF-16LE without a BOM.
    const tjs_uint length = text.GetLen();
    if(length) {
        std::unique_ptr<tjs_uint16[]> buffer(new tjs_uint16[length]);
        const tjs_char *src = text.c_str();
        for(tjs_uint i = 0; i < length; ++i)
            buffer[i] = static_cast<tjs_uint16>(src[i]);
        stream->Write(buffer.get(), length * sizeof(tjs_uint16));
    }
}

//----------------------------------------------------------------------------
// Scripts members
//----------------------------------------------------------------------------

struct ScriptsJSON {
    static tjs_error evalJSON(tTJSVariant *result, tjs_int numparams,
                              tTJSVariant **param, iTJSDispatch2 *) {
        if(numparams < 1)
            return TJS_E_BADPARAMCOUNT;
        JsonReader reader(param[0]->GetString());
        if(result)
            *result = reader.parse();
        else
            reader.parse();
        return TJS_S_OK;
    }

    static tjs_error evalJSONStorage(tTJSVariant *result, tjs_int numparams,
                                     tTJSVariant **param, iTJSDispatch2 *) {
        if(numparams < 1)
            return TJS_E_BADPARAMCOUNT;
        const bool utf8 =
            numparams > 1 && static_cast<tjs_int>(*param[1]) != 0;
        JsonReader reader(loadText(param[0]->GetString(), utf8));
        if(result)
            *result = reader.parse();
        else
            reader.parse();
        return TJS_S_OK;
    }

    static tjs_error saveJSON(tTJSVariant *result, tjs_int numparams,
                              tTJSVariant **param, iTJSDispatch2 *) {
        if(numparams < 2)
            return TJS_E_BADPARAMCOUNT;
        const bool utf8 =
            numparams > 2 && static_cast<tjs_int>(*param[2]) != 0;
        const tjs_int newline =
            numparams > 3 ? static_cast<tjs_int>(*param[3]) : 0;
        JsonWriter writer(newline);
        saveText(param[0]->GetString(), writer.stringify(*param[1]), utf8);
        if(result)
            *result = true;
        return TJS_S_OK;
    }

    static tjs_error toJSONString(tTJSVariant *result, tjs_int numparams,
                                  tTJSVariant **param, iTJSDispatch2 *) {
        if(numparams < 1)
            return TJS_E_BADPARAMCOUNT;
        const tjs_int newline =
            numparams > 1 ? static_cast<tjs_int>(*param[1]) : 0;
        JsonWriter writer(newline);
        if(result)
            *result = writer.stringify(*param[0]);
        return TJS_S_OK;
    }
};

} // namespace

NCB_ATTACH_CLASS(ScriptsJSON, Scripts) {
    RawCallback(TJS_W("evalJSON"), &ScriptsJSON::evalJSON, TJS_STATICMEMBER);
    RawCallback(TJS_W("evalJSONStorage"), &ScriptsJSON::evalJSONStorage,
                TJS_STATICMEMBER);
    RawCallback(TJS_W("saveJSON"), &ScriptsJSON::saveJSON, TJS_STATICMEMBER);
    RawCallback(TJS_W("toJSONString"), &ScriptsJSON::toJSONString,
                TJS_STATICMEMBER);
}

// Registration-only translation units in static libraries need an explicit
// reference from PluginImpl.cpp, otherwise the linker drops this object.
extern "C" void TVPJsonPluginAnchor() {}
