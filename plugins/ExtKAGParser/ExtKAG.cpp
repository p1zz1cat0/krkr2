// ExtKAGParser.dll
//
// Ported from krkrsdl3 plugins/ExtKAGParser/ExtKAG.cpp (Kirikiri Z license).
// The parser implementation derives from the KiriKiri KAG parser by W.Dee and
// contributors, modified by KAICHO.

#include "tjsCommHead.h"
#include "ncbind.hpp"

#define NCB_MODULE_NAME TJS_W("ExtKAGParser.dll")
#define TVP_KAGPARSER_EX_CLASSNAME TJS_W("KAGParser")

extern tTJSNativeClass* TVPCreateNativeClass_ExtKAGParser();
static iTJSDispatch2* origKAGParser = NULL;
static bool extkagparser_installed = false;

void extkagparser_init()
{
    if (extkagparser_installed)
        return;

    iTJSDispatch2* global = TVPGetScriptDispatch();
    if (global)
    {
        tTJSVariant val;
        if (TJS_SUCCEEDED(global->PropGet(0, TVP_KAGPARSER_EX_CLASSNAME, NULL, &val, global)))
        {
            origKAGParser = val.AsObject();
            val.Clear();
        }
        iTJSDispatch2* tjsclass = TVPCreateNativeClass_ExtKAGParser();
        val = tTJSVariant(tjsclass);
        tjsclass->Release();
        global->PropSet(TJS_MEMBERENSURE, TVP_KAGPARSER_EX_CLASSNAME, NULL, &val, global);
        global->Release();
        extkagparser_installed = true;
    }
}

void extkagparser_done()
{
    if (!extkagparser_installed)
        return;

    iTJSDispatch2* global = TVPGetScriptDispatch();
    if (global)
    {
        global->DeleteMember(0, TVP_KAGPARSER_EX_CLASSNAME, NULL, global);
        if (origKAGParser)
        {
            tTJSVariant val(origKAGParser);
            origKAGParser->Release();
            origKAGParser = NULL;
            global->PropSet(TJS_MEMBERENSURE, TVP_KAGPARSER_EX_CLASSNAME, NULL, &val, global);
        }
        global->Release();
    }
    extkagparser_installed = false;
}

NCB_PRE_REGIST_CALLBACK(extkagparser_init);
NCB_POST_UNREGIST_CALLBACK(extkagparser_done);

extern "C" void TVPExtKAGParserPluginAnchor() {}
