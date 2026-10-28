/**
 * @file      textrender.cc
 * @author    Hikaru Terazono (3c1u) <3c1u@vulpesgames.tokyo>
 * @brief     Drop-in replacement for TextRender.dll.
 * @date      2020-08-16
 *
 * @copyright Copyright (c) 2020 Hikaru Terazono. All rights reserved.
 *
 */

// Source: https://github.com/3c1u/TextRender (commit 2fe2957), dual-licensed
// MIT or Apache-2.0. Yoghourt uses it under the MIT License; the full text is
// in docs/TEXTRENDER_LICENSE.txt. The upstream is a work in progress, not the
// original TextRender.dll. Yoghourt changes: original lowercase setDefault
// keys, align/valign, and the %; reset code. Known gaps: see the Sbectrum
// feature for TextRender.

#include "ncbind.hpp"
#include "common/PluginSafety.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <limits>

#define NCB_MODULE_NAME TJS_W("TextRender.dll")

extern "C" void TVPTextRenderPluginAnchor() {}

// #define dbg_print TVPAddLog
#define dbg_print

// use Kirikiri-Z rasterizer for layouting
#include "FontRasterizer.h"
#include "LayerBitmapIntf.h"

#define LOGGER spdlog::get("plugin")

extern FontRasterizer *GetCurrentRasterizer();

using RgbColor = uint32_t;

#define setprop_t(d, p, ty)                                                    \
    {                                                                          \
        tTJSVariant v(ty(p));                                                  \
        d->PropSet(TJS_MEMBERENSURE, TJS_W(#p), nullptr, &v, d);               \
    }

#define setprop_opt_t(d, p, ty)                                                \
    if(NULL != p) {                                                            \
        tTJSVariant v(ty(p));                                                  \
        d->PropSet(TJS_MEMBERENSURE, TJS_W(#p), nullptr, &v, d);               \
    } else {                                                                   \
        tTJSVariant v;                                                         \
        d->PropSet(TJS_MEMBERENSURE, TJS_W(#p), nullptr, &v, d);               \
    }

#define setprop(d, p) setprop_t(d, p, )

#define setprop_opt(d, p) setprop_opt_t(d, p, )

#define getprop_opt(d, p) getprop_opt_t(d, p, )

#define getprop_t(d, p, ty)                                                    \
    {                                                                          \
        tTJSVariant v;                                                         \
        if(TJS_SUCCEEDED(d->PropGet(0, TJS_W(#p), nullptr, &v, d)) &&          \
           v.Type() != tvtVoid) {                                              \
            p = ty(v);                                                         \
        }                                                                      \
    }

#define getprop_opt_t(d, p, ty)                                                \
    {                                                                          \
        tTJSVariant v;                                                         \
        if(TJS_SUCCEEDED(d->PropGet(0, TJS_W(#p), nullptr, &v, d)) &&          \
           v.Type() != tvtVoid) {                                              \
            p = ty(v);                                                         \
        } else {                                                               \
            p = NULL;                                                          \
        }                                                                      \
    }

#define getprop_ensure_opt_deref(d, p, e)                                      \
    {                                                                          \
        tTJSVariant v;                                                         \
        if(TJS_SUCCEEDED(d->PropGet(0, TJS_W(#p), nullptr, &v, d)) &&          \
           v.Type() != tvtVoid) {                                              \
            auto ens = v.e;                                                    \
            if(ens)                                                            \
                p = *ens;                                                      \
            else                                                               \
                p = std::nullopt;                                              \
        } else {                                                               \
            p = std::nullopt;                                                  \
        }                                                                      \
    }

#define getprop(d, p) getprop_t(d, p, )

// Read the dictionary key `key` into `p`. Use this when the original
// TextRender.dll key is not the same as the member name.
#define getprop_key_t(d, key, p, ty)                                           \
    {                                                                          \
        tTJSVariant v;                                                         \
        if(TJS_SUCCEEDED(d->PropGet(0, TJS_W(key), nullptr, &v, d)) &&         \
           v.Type() != tvtVoid) {                                              \
            p = ty(v);                                                         \
        }                                                                      \
    }

#define getprop_ensure_deref(d, p, e)                                          \
    {                                                                          \
        tTJSVariant v;                                                         \
        if(TJS_SUCCEEDED(d->PropGet(0, TJS_W(#p), nullptr, &v, d)) &&          \
           v.Type() != tvtVoid) {                                              \
            auto ens = v.e;                                                    \
            if(ens)                                                            \
                p = *ens;                                                      \
        }                                                                      \
    }

struct TextRenderState {
    bool bold = false;
    bool italic = false;
    ttstr face = TJS_W("user");
    int fontSize = 24;
    double fontScale = 1.0;
    RgbColor chColor = 0xffffff;
    int rubySize = 10;
    int rubyOffset = -2;
    bool shadow = true;
    RgbColor shadowColor = 0x000000;
    bool edge = false;
    RgbColor edgeColor = 0x0080ff;
    int lineSpacing = 6;
    int pitch = 0;
    int lineSize = 0;
    int align = -1;  // -1 left, 0 center, 1 right
    int valign = -1; // -1 top, 0 center, 1 bottom

    bool renderOver = false;
    double renderDelay = 0;
    ttstr renderText = TJS_W("");

    // -------------------------------------------------------------- //

    tTJSVariant serialize() const {
        auto dict = TJSCreateDictionaryObject();

        setprop(dict, bold);
        setprop(dict, italic);
        setprop(dict, fontSize);
        setprop(dict, fontScale);
        setprop(dict, face);
        setprop_t(dict, chColor, static_cast<tjs_int>);
        setprop(dict, rubySize);
        setprop(dict, rubyOffset);
        setprop(dict, shadow);
        setprop_t(dict, shadowColor, static_cast<tjs_int>);
        setprop(dict, edge);
        setprop_t(dict, edgeColor, static_cast<tjs_int>);
        setprop(dict, lineSpacing);
        setprop(dict, pitch);
        setprop(dict, lineSize);

        auto res = tTJSVariant(dict, dict);
        dict->Release();

        return res;
    }

    void deserialize(tTJSVariant t) {
        auto dict = t.AsObjectNoAddRef();
        if(!dict) {
            return;
        }

        getprop_t(dict, bold, static_cast<tjs_int>);
        getprop_t(dict, italic, static_cast<tjs_int>);
        getprop(dict, fontSize);
        getprop(dict, fontScale);
        getprop_ensure_deref(dict, face, AsStringNoAddRef());
        getprop_t(dict, chColor, static_cast<tjs_int>);
        getprop(dict, rubySize);
        getprop(dict, rubyOffset);
        getprop_t(dict, shadow, static_cast<tjs_int>);
        getprop_t(dict, shadowColor, static_cast<tjs_int>);
        getprop_t(dict, edge, static_cast<tjs_int>);
        getprop_t(dict, edgeColor, static_cast<tjs_int>);
        getprop(dict, lineSpacing);
        getprop(dict, pitch);
        getprop(dict, lineSize);
        getprop(dict, align);
        getprop(dict, valign);

        // The original TextRender.dll reads lowercase keys. Games call
        // setDefault(%["fontsize" => ..., "align" => 0, ...]), for example
        // the RIDDLE JOKER choice buttons. Read them after the camelCase
        // keys, so that the original spelling wins.
        getprop_key_t(dict, "fontsize", fontSize, );
        getprop_key_t(dict, "color", chColor, static_cast<tjs_int>);
        getprop_key_t(dict, "rubysize", rubySize, );
        getprop_key_t(dict, "rubyoffset", rubyOffset, );
        getprop_key_t(dict, "shadowcolor", shadowColor, static_cast<tjs_int>);
        getprop_key_t(dict, "edgecolor", edgeColor, static_cast<tjs_int>);
        getprop_key_t(dict, "linespacing", lineSpacing, );
        getprop_key_t(dict, "linesize", lineSize, );
    }

    static TextRenderState from(tTJSVariant t) {
        TextRenderState state{};
        state.deserialize(t);
        return state;
    }
};

struct TextRenderOptions {
    const tjs_char *following = TJS_W(
        "%),:;]}????。，、．：；゛゜ヽヾゝゞ々’”）〕］｝〉》」』】°′″℃￠％‰　!."
        "?=?????????????？！ーぁぃぅぇぉっゃゅょゎァィゥェォッャュョヮヵヶ");
    const tjs_char *leading = TJS_W("\\$([{?‘“（〔［｛〈《「『【￥＄￡");
    bool ignoreDelay = false;
    bool widthTimeScale = false;
    // Auto-indent brackets. A close pairs with the open at the same index.
    ttstr begin = TJS_W("「『（‘“〔［｛〈《");
    ttstr end = TJS_W("」』）’”〕］｝〉》");

    // -------------------------------------------------------------- //

    tTJSVariant serialize() const {
        auto dict = TJSCreateDictionaryObject();

        setprop(dict, following);
        setprop(dict, leading);
        setprop(dict, begin);
        setprop(dict, end);

        auto res = tTJSVariant(dict, dict);
        dict->Release();

        return res;
    }

    void deserialize(tTJSVariant t) {
        auto dict = t.AsObjectNoAddRef();
        if(!dict) {
            return;
        }

        // The original replaces the whole set for each key.
        getprop_ensure_deref(dict, begin, AsStringNoAddRef());
        getprop_ensure_deref(dict, end, AsStringNoAddRef());
        getprop_key_t(dict, "ignore_delay", ignoreDelay, static_cast<tjs_int>);
        getprop_key_t(dict, "width_time_scale", widthTimeScale, static_cast<tjs_int>);
    }

    static TextRenderState from(tTJSVariant t) {
        TextRenderState state{};
        state.deserialize(t);
        return state;
    }
};

struct RubyInfo {
    ttstr text;
    double x = 0;
    double y = 0;
    int size = 0;

    tTJSVariant serialize() const {
        auto dict = TJSCreateDictionaryObject();
        setprop(dict, text);
        setprop(dict, x);
        setprop(dict, y);
        setprop(dict, size);
        tTJSVariant result(dict, dict);
        dict->Release();
        return result;
    }
};

struct CharacterInfo {
    bool bold = false;
    bool italic = false;
    bool graph = false;
    bool vertical = false;
    ttstr face = TJS_W("user");

    int x = 0;
    int y = 0;
    int cw = 0;
    int size = 0;

    RgbColor color = 0xffffff;
    RgbColor edge = NULL; //
    RgbColor shadow = NULL; //

    ttstr text = TJS_W("");

    int align = -1; // layout only; not given to the script

    double delay = 0;
    std::vector<RubyInfo> ruby;
    bool rubyArray = false;

    // Auto-indent actions. flush() does them when it places the character,
    // because the indent is the cursor after the character.
    bool hang = false;        // move back by cw before placing
    bool setIndent = false;   // indent = cursor after this character
    bool resetIndent = false; // indent = 0

    // -------------------------------------------------------------- //

    tTJSVariant serialize() const {
        auto dict = TJSCreateDictionaryObject();

        setprop(dict, bold);
        setprop(dict, italic);
        setprop(dict, graph);
        setprop(dict, vertical);
        setprop(dict, x);
        setprop(dict, y);
        setprop(dict, cw);
        setprop(dict, size);
        setprop(dict, face);

        setprop_t(dict, color, static_cast<tjs_int>);
        setprop_opt_t(dict, edge, static_cast<tjs_int>);
        setprop_opt_t(dict, shadow, static_cast<tjs_int>);

        setprop(dict, text);
        setprop(dict, delay);
        if(!ruby.empty()) {
            tTJSVariant value;
            if(rubyArray) {
                auto array = TJSCreateArrayObject();
                value = tTJSVariant(array, array);
                array->Release();
                for(size_t i = 0; i < ruby.size(); ++i) {
                    auto item = ruby[i].serialize();
                    array->PropSetByNum(TJS_MEMBERENSURE, i, &item, array);
                }
            } else {
                value = ruby.front().serialize();
            }
            dict->PropSet(TJS_MEMBERENSURE, TJS_W("ruby"), nullptr, &value, dict);
        }

        auto res = tTJSVariant(dict, dict);
        dict->Release();

        return res;
    }

    void deserialize(tTJSVariant t) {
        auto dict = t.AsObjectNoAddRef();
        if(!dict) {
            return;
        }

        getprop(dict, bold);
        getprop(dict, italic);
        getprop(dict, graph);
        getprop(dict, vertical);
        getprop(dict, x);
        getprop(dict, y);
        getprop(dict, cw);
        getprop(dict, size);
        getprop_ensure_deref(dict, face, AsStringNoAddRef());

        getprop_t(dict, color, static_cast<tjs_int>);
        getprop_opt_t(dict, edge, static_cast<tjs_int>);
        getprop_opt_t(dict, shadow, static_cast<tjs_int>);

        getprop_ensure_deref(dict, text, AsStringNoAddRef());
    }

    static TextRenderState from(tTJSVariant t) {
        TextRenderState state{};
        state.deserialize(t);
        return state;
    }
};

#define property_accessor(name, type, storage)                                 \
    type get_##name() const { return storage; }                                \
    void set_##name(type v) { storage = v; }

#define property_accessor_cast(name, type, cast, storage)                      \
    cast get_##name() const { return cast(storage); }                          \
    void set_##name(cast v) { storage = type(v); }

#define property_accessor_string(name, storage)                                \
    tTJSVariant get_##name() const { return tTJSVariant(storage); }            \
    void set_##name(tTJSVariant v) {                                           \
        auto s = v.AsStringNoAddRef();                                         \
        storage = s->LongString ? s->LongString : s->ShortString;              \
    }

#define property_delegate(name) NCB_PROPERTY(name, get_##name, set_##name);

class TextRenderBase {
public:
    TextRenderBase();
    virtual ~TextRenderBase();
    bool render(tTJSString text, int autoIndent, int diff, int all,
                bool same);
    void setRenderSize(int width, int height);
    void setDefault(tTJSVariant defaultSettings);
    void setOption(tTJSVariant options);
    tTJSVariant getCharacters(int start, int end);
    void clear();
    void done();

    void newline() { LOGGER->warn("no support newline"); }

    void resetFont();
    void resetStyle();
    tTJSVariant getKeyWait();
    void setKeyWait(tTJSVariant) { throw "no support setKeyWait"; };
    int calcShowCount(double elapsed);
    double get_timeScale() const { return m_timeScale; }
    void set_timeScale(double value) {
        if(!std::isfinite(value) || value < 0) {
            TVPThrowExceptionMessage(TJS_W("TextRender: invalid timeScale"));
        }
        m_timeScale = value;
    }

    // property accessor
    property_accessor(vertical, bool, m_vertical);

    property_accessor(bold, bool, m_state.bold);
    property_accessor(italic, bool, m_state.italic);
    property_accessor_string(face, m_state.face);
    property_accessor(fontSize, int, m_state.fontSize);
    property_accessor(fontScale, double, m_state.fontScale);
    property_accessor_cast(chColor, RgbColor, tjs_int, m_state.chColor);
    property_accessor(rubySize, int, m_state.rubySize);
    property_accessor(rubyOffset, int, m_state.rubyOffset);
    property_accessor(shadow, bool, m_state.shadow);
    property_accessor_cast(shadowColor, RgbColor, tjs_int, m_state.shadowColor);
    property_accessor(edge, bool, m_state.edge);
    property_accessor(lineSpacing, int, m_state.lineSpacing);
    property_accessor(pitch, int, m_state.pitch);
    property_accessor(lineSize, int, m_state.lineSize);
    property_accessor(align, int, m_state.align);

    property_accessor(renderOver, bool, m_state.renderOver);
    double get_renderDelay() const { return m_state.renderDelay * m_timeScale; }
    void set_renderDelay(double value) {
        if(!std::isfinite(value) || value < 0) {
            TVPThrowExceptionMessage(TJS_W("TextRender: invalid renderDelay"));
        }
        m_state.renderDelay = value;
    }
    property_accessor(renderLeft, int, m_x);
    int get_renderRight() const { return m_x + m_boxWidth; }
    void set_renderRight(int v) { m_boxWidth = v - m_x; };
    property_accessor(renderTop, int, m_y);
    int get_renderBottom() const { return m_y + m_boxHeight; }
    void set_renderBottom(int v) { m_boxHeight = v - m_y; };
    property_accessor(renderText, ttstr, m_state.renderText);
    int get_renderCount() const { return m_state.renderText.length(); }
    void set_renderCount(int v) { throw "avoid to set renderCount"; };

    property_accessor(defaultBold, bool, m_default.bold);
    property_accessor(defaultItalic, bool, m_default.italic);
    property_accessor_string(defaultFace, m_default.face);
    property_accessor(defaultFontSize, int, m_default.fontSize);
    property_accessor(defaultFontScale, float, m_default.fontScale);
    property_accessor_cast(defaultChColor, RgbColor, tjs_int,
                           m_default.chColor);
    property_accessor(defaultRubySize, int, m_default.rubySize);
    property_accessor(defaultRubyOffset, int, m_default.rubyOffset);
    property_accessor(defaultShadow, bool, m_default.shadow);
    property_accessor_cast(defaultShadowColor, RgbColor, tjs_int,
                           m_default.shadowColor);
    property_accessor(defaultEdge, bool, m_default.edge);
    property_accessor(defaultLineSpacing, int, m_default.lineSpacing);
    property_accessor(defaultPitch, int, m_default.pitch);
    property_accessor(defaultLineSize, int, m_default.lineSize);
    property_accessor(defaultAlign, int, m_default.align);
    property_accessor(defaultValign, int, m_default.valign);

private:
    int m_boxWidth = 0;
    int m_boxHeight = 0;

    int m_x = 0;
    int m_y = 0;

    // m_indent stays after done(). The other auto-indent state is for one
    // render() call only (textrender.dll keeps it in locals).
    int m_indent = 0;
    int m_autoIndent = 0;
    bool m_renderStart = true; // no character yet in this render()
    bool m_lineStart = true;   // render() start, or after LF
    int m_depth = 0;           // bracket depth; can go below 0
    tjs_char m_opener = 0;     // open bracket that set the indent
    bool m_overflow = false;

    bool m_vertical = false;

    TextRenderOptions m_options{};
    TextRenderState m_default{};
    TextRenderState m_state{};

    std::vector<CharacterInfo> m_characters{};
    std::vector<CharacterInfo> m_buffer{};
    size_t m_alignedCount = 0; // m_characters before this index are aligned
    uint32_t m_mode = 0;
    double m_timeScale = 1;
    double m_delay = 0;
    double m_step = 0;
    double m_baseStep = 0;
    double m_lastDelay = 0;
    size_t m_anchorIndex = 0;
    double m_anchorTime = 0;
    bool m_trackHasCharacters = false;
    ttstr m_rubyText;
    ttstr m_pendingText;
    int m_rubyRemaining = 0;

    void emitCharacter(tjs_char ch, std::vector<RubyInfo> ruby, bool rubyArray);
    void discardRuby();
    int measure(ttstr const &text, int size);
    void scaleSegment(double factor);
    void finishTiming(int all);

    void pushCharacter(tjs_char ch);
    void pushGraphicalCharacter(ttstr const &graph);
    void performLinebreak();
    void flush(bool force = false);
    void updateFont();
    void applyAlignment();
};

enum TextRenderAlignment {
    kTextRenderAlignmentLeft = -1,
    kTextRenderAlignmentCenter = 0,
    kTextRenderAlignmentRight = 1,
};

enum TextRenderMode {
    kTextRenderModeLeading = 0,
    kTextRenderModeNormal,
    kTextRenderModeFollowing,
};

// -------------------------------------------------------------------

TextRenderBase::TextRenderBase() {}

TextRenderBase::~TextRenderBase() {}

static bool readchar(tTJSString const &str, size_t &i, tjs_char &c) {
    auto const len = str.GetLen();

    if(++i >= len) {
        return false;
    }

    c = str[i];
    return true;
}

static void read_integer(tTJSString const &str, size_t &i, int &value) {
    tjs_char ch;
    bool is_negative = false;

    while(true) {
        if(!readchar(str, i, ch)) {
            TVPThrowExceptionMessage(
                TJS_W("TextRenderBase::render() failed to "
                      "parse: expected either integer or ';', found EOF"));
        }

        if('0' <= ch && ch <= '9') {
            if(value > (std::numeric_limits<int>::max() - (ch - '0')) / 10) {
                TVPThrowExceptionMessage(TJS_W("TextRender: integer out of range"));
            }
            value = value * 10 + (ch - '0');
            continue;
        } else if(ch == '-') {
            is_negative = !is_negative;
            continue;
        } else if(ch == ';') {
            if(is_negative) {
                value = -value;
            }
            return;
        }

        TVPThrowExceptionMessage(
            TJS_W("TextRenderBase::render() failed to "
                  "parse: expected either integer or ';', found '%1'"),
            ch);
    }
}

bool TextRenderBase::render(tTJSString text, int autoIndent, int diff, int all,
                            bool same) {

    auto const len = text.GetLen();

    flush();
    if(!same) {
        for(auto &ch : m_characters) ch.delay = 0;
        m_state.renderDelay = 0;
    }
    m_anchorIndex = m_characters.size();
    m_anchorTime = 0;
    m_trackHasCharacters = false;
    m_delay = 0;
    m_lastDelay = 0;
    m_baseStep = std::max(diff, 0);
    m_step = m_baseStep;
    if(all > 0 && m_step == 0) m_step = 1;

    m_autoIndent = autoIndent;
    m_renderStart = true;
    m_lineStart = true;
    m_depth = 0;
    m_opener = 0;

    for(size_t i = 0; i < len; ++i) {
        auto ch = text[i];

        switch(ch) {
            case '\n':
                // Discard an incomplete ruby group before a line break.
                discardRuby();
                flush();
                performLinebreak();
                m_lineStart = true;
                break;
            case '%':
                if(!readchar(text, i, ch)) {
                    TVPThrowExceptionMessage(
                        TJS_W("TextRenderBase::render() failed to "
                              "parse: expected character, found EOF"));
                }

                switch(ch) {
                    case ';':
                        m_state.fontSize = m_default.fontSize;
                        updateFont();
                        break;
                    case 'f': {
                        ttstr faceName{};

                        while(true) {
                            if(!readchar(text, i, ch)) {
                                TVPThrowExceptionMessage(TJS_W(
                                    "TextRenderBase::render() failed to "
                                    "parse: expected character, found EOF"));
                            }

                            if(ch == ';')
                                break;

                            faceName += ch;
                        }

                        dbg_print(TVPFormatMessage(
                            TJS_W("change font face name: %1"), faceName));

                        m_state.face = faceName;

                        break;
                    }
                    case 'b': {
                        if(!readchar(text, i, ch) && (ch == '0' || ch == '1')) {
                            TVPThrowExceptionMessage(
                                TJS_W("TextRenderBase::render() failed to "
                                      "parse %%b: expected either '0' or '1', "
                                      "found EOF"));
                        }
                        bool flag = ch == '1';
                        if(flag)
                            dbg_print(TJS_W("set bold"));
                        else
                            dbg_print(TJS_W("unset bold"));

                        m_state.bold = flag;

                        break;
                    }
                    case 'i': {
                        if(!readchar(text, i, ch) && (ch == '0' || ch == '1')) {
                            TVPThrowExceptionMessage(
                                TJS_W("TextRenderBase::render() failed to "
                                      "parse %%i: expected either '0' or '1', "
                                      "found EOF"));
                        }
                        bool flag = ch == '1';
                        if(flag)
                            dbg_print(TJS_W("set italic (oblique)"));
                        else
                            dbg_print(TJS_W("unset italic (oblique)"));

                        m_state.italic = flag;

                        break;
                    }
                    case 's': {
                        if(!readchar(text, i, ch) && (ch == '0' || ch == '1')) {
                            TVPThrowExceptionMessage(
                                TJS_W("TextRenderBase::render() failed to "
                                      "parse %%s: expected either '0' or '1', "
                                      "found EOF"));
                        }
                        bool flag = ch == '1';
                        if(flag)
                            dbg_print(TJS_W("set shadow"));
                        else
                            dbg_print(TJS_W("unset shadow"));

                        m_state.shadow = flag;

                        break;
                    }
                    case 'e': {
                        if(!readchar(text, i, ch) && (ch == '0' || ch == '1')) {
                            TVPThrowExceptionMessage(
                                TJS_W("TextRenderBase::render() failed to "
                                      "parse %%e: expected either '0' or '1', "
                                      "found '%1'"),
                                ch);
                        }
                        bool flag = ch == '1';
                        if(flag)
                            dbg_print(TJS_W("set edge"));
                        else
                            dbg_print(TJS_W("unset edge"));

                        m_state.edge = flag;

                        break;
                    }
                    case 'n': {
                        // textrender.dll accepts %n, %n;, %n2;, and %n-1;.
                        // The optional signed value is the number of line
                        // breaks; an omitted value defaults to one.  Unlike
                        // read_integer(), the delimiter itself is optional,
                        // and the first non-numeric character must remain in
                        // the input stream.
                        int linebreaks = 1;
                        size_t cursor = i + 1;
                        bool negative = false;
                        bool hasValue = false;

                        if(cursor < len && text[cursor] == '-') {
                            negative = true;
                            hasValue = true;
                            ++cursor;
                        }

                        int parsed = 0;
                        while(cursor < len && '0' <= text[cursor]
                              && text[cursor] <= '9') {
                            hasValue = true;
                            parsed = parsed * 10 + (text[cursor] - '0');
                            ++cursor;
                        }

                        if(hasValue)
                            linebreaks = negative ? -parsed : parsed;

                        if(cursor < len && text[cursor] == ';')
                            i = cursor;
                        else if(cursor > i + 1)
                            i = cursor - 1;

                        if(linebreaks > 0) discardRuby();
                        flush();
                        if(linebreaks > 0) m_lineStart = true;
                        for(int line = 0; line < linebreaks; ++line)
                            performLinebreak();

                        break;
                    }
                    case 'B': // TODO: Big
                        dbg_print(TJS_W("big font"));
                        break;
                    case 'S': // TODO: Small
                        dbg_print(TJS_W("small font"));
                        break;
                    case 'r': // Reset
                        dbg_print(TJS_W("reset"));
                        resetFont();
                        break;
                    case 'C':
                        m_state.align = kTextRenderAlignmentCenter;
                        break;
                    case 'R':
                        m_state.align = kTextRenderAlignmentRight;
                        break;
                    case 'L':
                        m_state.align = kTextRenderAlignmentLeft;
                        break;
                    case 'l': {
                        dbg_print(TJS_W("left"));
                        while(true) {
                            if(!readchar(text, i, ch)) {
                                TVPThrowExceptionMessage(TJS_W(
                                    "TextRenderBase::render() failed to "
                                    "parse: expected character, found EOF"));
                            }
                            if(ch == ';')
                                break;
                        }
                        break;
                    }
                    case 'p': {
                        int value = 0;
                        read_integer(text, i, value);
                        m_state.pitch = value;
                        break;
                    }
                    case 'd':
                    case 'a':
                    case 'w':
                    case 't':
                    case 'D': {
                        auto const command = ch;
                        bool const empty = i + 1 < len && text[i + 1] == ';';
                        int value = 0;
                        read_integer(text, i, value);
                        double const amount = std::max(value, 0);
                        if(m_options.ignoreDelay) break;
                        if(command == 'd') {
                            m_step = m_baseStep * (empty ? 100 : amount) / 100;
                        } else if(command == 'a') {
                            m_step = empty ? m_baseStep : amount;
                        } else if(command == 'w') {
                            m_delay += m_baseStep * amount / 100;
                        } else if(command == 't') {
                            m_delay += amount;
                        } else {
                            double const target = std::max(amount, m_anchorTime);
                            double const span = m_delay - m_anchorTime;
                            if(span > 0 && target < m_delay) {
                                double const factor = (target - m_anchorTime) / span;
                                scaleSegment(factor);
                            }
                            m_delay = target;
                            m_anchorTime = target;
                            m_anchorIndex = m_characters.size() + m_buffer.size();
                        }
                        break;
                    }
                    case '0':
                    case '1':
                    case '2':
                    case '3':
                    case '4':
                    case '5':
                    case '6':
                    case '7':
                    case '8':
                    case '9': {
                        int value = static_cast<int>(ch - '0');
                        read_integer(text, i, value);

                        // font size
                        m_state.fontSize = m_default.fontSize * value / 100;

                        dbg_print(TVPFormatMessage(
                            TJS_W("new font size: %1 px"), m_state.fontSize));

                        updateFont();

                        break;
                    }
                    default:
                        TVPThrowExceptionMessage(
                            TJS_W("TextRenderBase::render() failed to "
                                  "parse: expected any of 'n' or "
                                  "'fbiseBSrCRLpdwD0123456789', found "
                                  "'%1'"),
                            ch);
                        break;
                }
                break;
            case '\\':
                if(!readchar(text, i, ch)) {
                    TVPThrowExceptionMessage(
                        TJS_W("TextRenderBase::render() failed to "
                              "parse: expected character, found EOF"));
                }

                switch(ch) {
                    case 'n':
                        discardRuby();
                        flush();
                        performLinebreak();
                        break;
                    case 't':
                        pushCharacter('\t');
                        break;
                    case 'i':
                        m_indent = m_x;
                        break;
                    case 'r':
                        m_indent = 0;
                        break;
                    case 'w':
                        pushCharacter(' ');
                        break;
                    case 'k':
                        break;
                    case 'x':
                        break;
                    default:
                        goto __draw_normal;
                        break;
                }
                break;
            case '[': {
                // [ .* ]
                // [ .*, [0-9]+ ]
                ttstr ruby{};

                while(true) {
                    if(!readchar(text, i, ch)) {
                        TVPThrowExceptionMessage(
                            TJS_W("TextRenderBase::render() failed to "
                                  "parse: expected character, found EOF"));
                    }

                    if(ch == ']')
                        break;

                    ruby += ch;
                }

                tjs_int comma = -1;
                for(tjs_int j = 0; j < ruby.GetLen(); ++j) {
                    if(ruby[j] == ',') { comma = j; break; }
                }
                if(comma >= 0) {
                    m_rubyText = ttstr(ruby.c_str(), comma);
                    int count = 0;
                    for(tjs_int j = comma + 1; j < ruby.GetLen(); ++j) {
                        if(ruby[j] < '0' || ruby[j] > '9' ||
                           count > (std::numeric_limits<int>::max() - 9) / 10) {
                            TVPThrowExceptionMessage(TJS_W("TextRender: invalid ruby count"));
                        }
                        count = count * 10 + ruby[j] - '0';
                    }
                    m_rubyRemaining = count;
                } else {
                    m_rubyText = ruby;
                    m_rubyRemaining = 0;
                }

                break;
            }
            case '#': {
                // [ .* ]
                // [ .*, [0-9]+ ]
                RgbColor colour = 0x00000000;
                bool isNull = true;

                while(true) {
                    if(!readchar(text, i, ch)) {
                        TVPThrowExceptionMessage(
                            TJS_W("TextRenderBase::render() failed to "
                                  "parse: expected character, found EOF"));
                    }

                    if(ch == ';')
                        break;
                    isNull = false;
                    RgbColor c = 0;
                    if('0' <= ch && ch <= '9') {
                        c = static_cast<RgbColor>(ch - '0');
                    } else if('A' <= ch && ch <= 'F') {
                        c = 0x0a + static_cast<RgbColor>(ch - 'A');
                    } else if('a' <= ch && ch <= 'f') {
                        c = 0x0a + static_cast<RgbColor>(ch - 'a');
                    } else {
                        TVPThrowExceptionMessage(
                            TJS_W("TextRenderBase::render() failed to "
                                  "parse: expected hexadecimal number, found "
                                  "'%1'"),
                            ch);
                    }

                    colour = (colour << 4) | c;
                }
                if(isNull)
                    colour = 0xffffff;
                m_state.chColor = colour;

                break;
            }
            case '&': {
                // '&' .+ ';'
                ttstr graph{};

                while(true) {
                    if(!readchar(text, i, ch)) {
                        TVPThrowExceptionMessage(
                            TJS_W("TextRenderBase::render() failed to "
                                  "parse: expected character, found EOF"));
                    }

                    if(ch == ';')
                        break;

                    graph += ch;
                }

                pushGraphicalCharacter(graph);
                break;
            }
            case '$': {
                // '&' .+ ';'
                ttstr varName{};

                while(true) {
                    if(!readchar(text, i, ch)) {
                        TVPThrowExceptionMessage(
                            TJS_W("TextRenderBase::render() failed to "
                                  "parse: expected character, found EOF"));
                    }

                    if(ch == ';')
                        break;

                    varName += ch;
                }

                // TODO: implement eval

                break;
            }
            default:
            __draw_normal:
                // TODO: character should include format options;
                //       as the font is lazy-evaluated/drawn
                //       (restrictions for line-breaking algorithm)
                pushCharacter(ch);
                break;
        }
    }

    finishTiming(all);

    if(m_y > m_boxHeight) {
        m_overflow = true;
        set_renderOver(true);
    } else {
        m_overflow = false;
        set_renderOver(false);
    }

    return !m_overflow;
}

void TextRenderBase::performLinebreak() {
    auto rasterizer = GetCurrentRasterizer();
    m_x = m_indent;
    m_y += rasterizer->GetAscentHeight() + m_state.lineSpacing;
}

void TextRenderBase::pushGraphicalCharacter(ttstr const &graph) {
    // TODO: implement graphical characters
}

static bool findchInChars(const tjs_char *chz, tjs_char ch) {
    for(int i = 0; chz[i] != L'\0'; i++) {
        if(chz[i] == ch) {
            return true;
        }
    }
    return false;
}

// Return the index of ch in chars, or -1.
static int indexOfChar(ttstr const &chars, tjs_char ch) {
    for(tjs_int i = 0, n = chars.GetLen(); i < n; i++) {
        if(chars[i] == ch) {
            return i;
        }
    }
    return -1;
}

void TextRenderBase::discardRuby() {
    m_rubyText = TJS_W("");
    m_pendingText = TJS_W("");
    m_rubyRemaining = 0;
}

int TextRenderBase::measure(ttstr const &text, int size) {
    auto rasterizer = GetCurrentRasterizer();
    tTVPFont font{size, static_cast<tjs_uint32>(
        (m_state.bold ? TVP_TF_BOLD : 0) | (m_state.italic ? TVP_TF_ITALIC : 0)),
        0, m_state.face};
    rasterizer->ApplyFont(font);
    int width = 0, height = 0;
    for(tjs_int i = 0; i < text.GetLen(); ++i) {
        int cw = 0;
        rasterizer->GetTextExtent(text[i], cw, height);
        if(cw > 0 && width > std::numeric_limits<int>::max() - cw) {
            updateFont();
            TVPThrowExceptionMessage(TJS_W("TextRender: text width overflow"));
        }
        width += cw;
    }
    updateFont();
    return width;
}

void TextRenderBase::pushCharacter(tjs_char ch) {
    if((0xD800 <= ch && ch <= 0xDFFF)) {
        TVPThrowExceptionMessage(TJS_W("unexpected character: surrogate pair"));
    }
    if(m_pendingText.GetLen() == 0 && m_rubyText.GetLen() == 0 && m_rubyRemaining == 0) {
        emitCharacter(ch, {}, false);
        return;
    }
    if(static_cast<size_t>(m_pendingText.GetLen()) >=
       pluginSafety::kDefaultSingleAllocationLimit / sizeof(CharacterInfo)) {
        TVPThrowExceptionMessage(TJS_W("TextRender: ruby group too large"));
    }
    m_pendingText += ch;
    if(m_rubyRemaining > 0) {
        --m_rubyRemaining;
        return;
    }

    auto const count = m_pendingText.GetLen();
    auto const bases = pluginSafety::checkedElementBytes(static_cast<size_t>(count),
        sizeof(CharacterInfo) + sizeof(std::vector<RubyInfo>) + sizeof(double));
    auto const accents = pluginSafety::checkedElementBytes(static_cast<size_t>(m_rubyText.GetLen()),
        sizeof(RubyInfo) + sizeof(double));
    pluginSafety::OperationBudget budget;
    if(!bases || !accents ||
       !pluginSafety::validateAllocationBudget(bases.value, budget) ||
       !pluginSafety::validateAllocationBudget(accents.value, budget)) {
        TVPThrowExceptionMessage(TJS_W("TextRender: ruby group too large"));
    }
    std::vector<std::vector<RubyInfo>> rubies(count);
    if(m_rubyText.GetLen() != 0) {
        if(count == 1) {
            double const width = measure(m_pendingText, m_state.fontSize);
            double const rubyWidth = measure(m_rubyText, m_state.rubySize);
            rubies[0].push_back({m_rubyText, (width - rubyWidth) / 2,
                                -double(m_state.rubySize) - m_state.rubyOffset,
                                m_state.rubySize});
        } else {
            std::vector<double> widths;
            double span = 0;
            for(tjs_int i = 0; i < count; ++i) {
                double const width = measure(ttstr() + m_pendingText[i], m_state.fontSize);
                widths.push_back(width);
                span += width;
            }
            span += double(m_state.pitch) * (count - 1);
            span -= (widths.front() + widths.back()) / 2;
            auto const rubyCount = m_rubyText.GetLen();
            std::vector<double> rubyWidths;
            double rubySpan = 0;
            for(tjs_int i = 0; i < rubyCount; ++i) {
                double const width = measure(ttstr() + m_rubyText[i], m_state.rubySize);
                rubyWidths.push_back(width);
                rubySpan += width;
            }
            rubySpan -= (rubyWidths.front() + rubyWidths.back()) / 2;
            double x = widths.front() / 2 - std::max(0.0, rubySpan - span) / 2;
            double const step = rubyCount > 1 ? std::max(span, rubySpan) / (rubyCount - 1) : 0;
            size_t target = 0;
            for(tjs_int i = 0; i < rubyCount; ++i) {
                rubies[target].push_back({ttstr() + m_rubyText[i], x - rubyWidths[i] / 2,
                    -double(m_state.rubySize) - m_state.rubyOffset, m_state.rubySize});
                x += step;
                double const width = widths[target] + m_state.pitch;
                if(target + 1 < widths.size() && x > width) {
                    ++target;
                    x -= width;
                }
            }
        }
    }
    auto text = m_pendingText;
    discardRuby();
    for(tjs_int i = 0; i < count; ++i) {
        emitCharacter(text[i], std::move(rubies[i]), count > 1);
    }
}

void TextRenderBase::emitCharacter(tjs_char ch, std::vector<RubyInfo> ruby, bool rubyArray) {
    if((0xD800 <= ch && ch <= 0xDBFF) /* upper surrogate-pair */
       || (0xDC00 <= ch && ch <= 0xDFFF) /* lower surrogate-pair */) {
        TVPThrowExceptionMessage(TJS_W("unexpected character: surrogate pair"));
    }

    auto isLeadingChar = findchInChars(m_options.leading, ch);
    auto isFollowingChar = findchInChars(m_options.following, ch);
    uint32_t current;

    if(isLeadingChar) {
        current = kTextRenderModeLeading;
    } else if(isFollowingChar) {
        current = kTextRenderModeFollowing;
    } else {
        current = kTextRenderModeNormal;
    }

    if(m_mode == kTextRenderModeFollowing || m_mode != kTextRenderModeLeading) {
        flush();
    }

    auto rasterizer = GetCurrentRasterizer();
    int advance_width = 0, advance_height = 0;

    rasterizer->GetTextExtent(ch, advance_width, advance_height);

    CharacterInfo info{
        m_state.bold,
        m_state.italic,
        false,
        false,
        m_state.face,
        0,
        0,
        advance_width,
        // The script renderer assigns CharacterInfo.size back to
        // layer.font.height before drawing. Keep it identical to the height
        // used to measure advance_width; an ascent is a baseline metric, not
        // a font size, and using it here makes glyphs overlap their layout.
        m_state.fontSize,
        m_state.chColor,
        (m_state.edge ? m_state.edgeColor : 0),
        (m_state.shadow ? m_state.shadowColor : 0),
        (ttstr() + ch),
        m_state.align,
    };

    info.delay = m_delay;
    info.ruby = std::move(ruby);
    info.rubyArray = rubyArray;
    m_lastDelay = m_delay;
    double const widthScale = m_options.widthTimeScale ?
        double(std::max(advance_width, 0)) / std::max(m_state.fontSize, 1) : 1;
    m_delay += m_step * widthScale;
    m_trackHasCharacters = true;

    // Auto-indent, as textrender.dll does it (feature 0015, REVERSE 10):
    // - autoIndent < 0: the first character of render() hangs left when it
    //   is an open bracket.
    // - An open bracket at line start with depth 0 sets the indent.
    // - A close bracket at depth 1 that pairs with that open clears it.
    // - Each open adds 1 to depth and each close removes 1.
    if(m_autoIndent != 0) {
        int const open = indexOfChar(m_options.begin, ch);
        if(open >= 0) {
            info.hang = m_autoIndent < 0 && m_renderStart;
            if(m_lineStart && m_depth == 0) {
                info.setIndent = true;
                m_opener = ch;
            }
            ++m_depth;
        } else {
            int const close = indexOfChar(m_options.end, ch);
            if(close >= 0) {
                if(m_depth == 1 && m_opener != 0 &&
                   indexOfChar(m_options.begin, m_opener) == close) {
                    info.resetIndent = true;
                }
                --m_depth;
            }
        }
    }

    m_buffer.push_back(std::move(info));

    m_mode = current;
    m_renderStart = false;
    m_lineStart = false;
}

void TextRenderBase::flush(bool force) {
    if(m_buffer.empty()) {
        return;
    }

    // try place all characters in the same line

    auto x = m_x;
    // Keep indent changes local until the characters are placed. A retry
    // (flush(true) below) must start the new line at the old indent.
    auto indent = m_indent;

    for(auto &ch : m_buffer) {
        auto advance_width = ch.cw;
        if(ch.hang) {
            x -= advance_width;
        }
        auto new_x = advance_width + x + m_state.pitch;

        if(m_boxWidth < new_x) {
            if(force) {
                m_indent = indent;
                performLinebreak();
                x = m_x;
                new_x = advance_width + x + m_state.pitch;
            } else {
                performLinebreak();
                flush(true);
                return;
            }
        }

        ch.x = x;
        ch.y = m_y;

        x = new_x;
        if(ch.setIndent) {
            indent = x;
        }
        if(ch.resetIndent) {
            indent = 0;
        }
    }

    m_x = x;
    m_indent = indent;
    m_characters.insert(m_characters.end(), m_buffer.begin(), m_buffer.end());
    for(size_t i = 0; i < m_buffer.size(); i++) {
        m_state.renderText += m_buffer.at(i).text;
    }
    m_buffer.clear();
}

void TextRenderBase::setRenderSize(int width, int height) {
    m_boxWidth = width;
    m_boxHeight = height;

    dbg_print(
        TVPFormatMessage(TJS_W("set render size: (%1, %2)"), width, height));

    clear();
}

void TextRenderBase::setDefault(tTJSVariant defaultSettings) {
    dbg_print(TJS_W("set default format"));
    m_default.deserialize(defaultSettings);
}

void TextRenderBase::setOption(tTJSVariant options) {
    dbg_print(TJS_W("set option"));
    m_options.deserialize(options);
}

tTJSVariant TextRenderBase::getCharacters(int start, int end) {
    auto array = TJSCreateArrayObject();
    tTJSVariant result(array, array);
    array->Release();
    size_t const first = static_cast<size_t>(std::max(start, 0));
    size_t const available = first < m_characters.size() ? m_characters.size() - first : 0;
    size_t const count = end > 0 ? std::min(available, static_cast<size_t>(end)) : available;
    for(size_t i = 0; i < count; ++i) {
        auto ch = m_characters[first + i].serialize();
        array->PropSetByNum(TJS_MEMBERENSURE, i, &ch, array);
    }
    return result;
}

void TextRenderBase::clear() {
    dbg_print(TJS_W("clear character buffer and format"));

    m_characters.clear();
    m_buffer.clear();
    discardRuby();
    m_delay = m_step = m_baseStep = m_lastDelay = 0;
    m_trackHasCharacters = false;
    m_anchorIndex = 0;
    m_anchorTime = 0;
    m_mode = 0;
    m_alignedCount = 0;

    m_state = m_default;
    m_state.renderDelay = 0;
    m_overflow = false;

    m_x = 0;
    m_y = 0;
    m_indent = 0;

    m_renderStart = true;
    m_lineStart = true;

    updateFont();
}

void TextRenderBase::updateFont() {
    auto rasterizer = GetCurrentRasterizer();
    auto font = tTVPFont{
        m_state.fontSize, // height of text
        static_cast<tjs_uint32>((m_state.bold ? TVP_TF_BOLD : 0) |
                                (m_state.italic ? TVP_TF_ITALIC : 0)),
        0,
        m_state.face, // TODO: this may fuck up the font settings by
                      // forcing the fallback font (in most cases)
    };

    rasterizer->ApplyFont(font);
}

void TextRenderBase::done() {
    dbg_print(TJS_W("flush character buffer"));
    flush();
    applyAlignment();
    std::stable_sort(m_characters.begin(), m_characters.end(),
                     [](CharacterInfo const &a, CharacterInfo const &b) {
                         return a.delay < b.delay;
                     });
}

// Move each line by its align value inside m_boxWidth, and move all lines by
// m_default.valign inside m_boxHeight. Left/top (-1) does not move anything.
// A line is a run of characters with the same y. Only the characters added
// after the last call are moved, so a second done() does not move them again.
void TextRenderBase::applyAlignment() {
    size_t const first = m_alignedCount;
    m_alignedCount = m_characters.size();
    if(first >= m_characters.size()) {
        return;
    }

    int textBottom = 0;
    for(size_t begin = first; begin < m_characters.size();) {
        size_t end = begin;
        int right = 0;
        while(end < m_characters.size() &&
              m_characters[end].y == m_characters[begin].y) {
            right = std::max(right, m_characters[end].x + m_characters[end].cw);
            textBottom = std::max(textBottom,
                                  m_characters[end].y + m_characters[end].size);
            ++end;
        }

        int const align = m_characters[end - 1].align;
        if(align != kTextRenderAlignmentLeft) {
            int const space = m_boxWidth - right;
            int const dx = align == kTextRenderAlignmentCenter ? space / 2 : space;
            for(size_t i = begin; i < end; ++i) {
                m_characters[i].x += dx;
            }
        }
        begin = end;
    }

    if(m_default.valign != kTextRenderAlignmentLeft) {
        int const space = m_boxHeight - textBottom;
        int const dy =
            m_default.valign == kTextRenderAlignmentCenter ? space / 2 : space;
        for(size_t i = first; i < m_characters.size(); ++i) {
            m_characters[i].y += dy;
        }
    }
}

void TextRenderBase::resetFont() { resetStyle(); }

void TextRenderBase::resetStyle() {
    auto text = m_state.renderText;
    double const duration = m_state.renderDelay;
    bool const over = m_state.renderOver;
    m_state = m_default;
    m_state.renderText = text;
    m_state.renderDelay = duration;
    m_state.renderOver = over;
}

tTJSVariant TextRenderBase::getKeyWait() {
    iTJSDispatch2 *dsp = TJSCreateArrayObject();
    tTJSVariant ret(dsp, dsp);
    dsp->Release();
    return ret;
}

void TextRenderBase::scaleSegment(double factor) {
    size_t index = 0;
    auto retime = [&](CharacterInfo &info) {
        if(index++ >= m_anchorIndex) {
            info.delay = m_anchorTime + (info.delay - m_anchorTime) * factor;
        }
    };
    for(auto &info : m_characters) retime(info);
    for(auto &info : m_buffer) retime(info);
    m_lastDelay = m_anchorTime + (m_lastDelay - m_anchorTime) * factor;
}

void TextRenderBase::finishTiming(int all) {
    if(!m_trackHasCharacters) return;
    double duration = m_lastDelay + m_baseStep;
    if(all > m_anchorTime) {
        // Keep previous synchronization segments fixed.
        double const source = m_lastDelay + (m_baseStep > 0 ? m_baseStep : 1) - m_anchorTime;
        if(source > 0) {
            scaleSegment((all - m_anchorTime) / source);
            duration = all;
        }
    }
    m_state.renderDelay = std::max(m_state.renderDelay, duration);
}

int TextRenderBase::calcShowCount(double elapsed) {
    for(size_t i = m_characters.size(); i > 0; --i) {
        if(elapsed >= m_characters[i - 1].delay * m_timeScale) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

// register the class
NCB_REGISTER_CLASS(TextRenderBase) {
    Constructor();

    NCB_METHOD(render);
    NCB_METHOD(setRenderSize);
    NCB_METHOD(setDefault);
    NCB_METHOD(setOption);
    NCB_METHOD(getCharacters);
    NCB_METHOD(clear);
    NCB_METHOD(done);
    NCB_METHOD(newline);

    NCB_METHOD(resetFont);
    NCB_METHOD(resetStyle);
    NCB_METHOD(getKeyWait);
    NCB_METHOD(calcShowCount);
    Property(TJS_W("keyWait"), &Class::getKeyWait, &Class::setKeyWait);
    property_delegate(renderCount);
    property_delegate(renderOver);
    property_delegate(renderDelay);
    property_delegate(timeScale);
    property_delegate(renderText);
    property_delegate(renderLeft);
    property_delegate(renderRight);
    property_delegate(renderTop);
    property_delegate(renderBottom);

    property_delegate(vertical);
    property_delegate(bold);
    property_delegate(italic);
    property_delegate(face);
    property_delegate(fontSize);
    property_delegate(fontScale);
    property_delegate(chColor);
    property_delegate(rubySize);
    property_delegate(rubyOffset);
    property_delegate(shadow);
    property_delegate(edge);
    property_delegate(lineSpacing);
    property_delegate(pitch);
    property_delegate(lineSize);
    property_delegate(align);

    property_delegate(defaultBold);
    property_delegate(defaultItalic);
    property_delegate(defaultFace);
    property_delegate(defaultFontSize);
    property_delegate(defaultFontScale);
    property_delegate(defaultChColor);
    property_delegate(defaultRubySize);
    property_delegate(defaultRubyOffset);
    property_delegate(defaultShadow);
    property_delegate(defaultEdge);
    property_delegate(defaultLineSpacing);
    property_delegate(defaultPitch);
    property_delegate(defaultLineSize);
    property_delegate(defaultAlign);
    property_delegate(defaultValign);
};
