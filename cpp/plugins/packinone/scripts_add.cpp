// scripts_add.cpp — ScriptsAdd 兼容确认与 PackinOne 加载入口。
//
// PackinOne.dll 的脚本扩展（官方逆向文档 M12-04-01）：
//   ncbAttachTJS2ClassAutoRegister<ScriptsAdd>
//   ncbAttachTJS2ClassAutoRegister<ScriptsAddForSaveStruct>
//   ncbAttachTJS2ClassAutoRegister<ArrayAdd> / <DictAdd>
//
// 这些接口在 KrKr2 本体已由独立插件提供，方法面与 PackinOne 一致：
//   scriptsEx.cpp  -> ScriptsAdd（getObjectKeys/getObjectCount/equalStruct/
//                     clone/safeEvalStorage 等）
//   saveStruct.cpp -> ArrayAdd.save2/saveStruct2/toStructString、
//                     DictAdd.saveStruct2/toStructString
//   fstat main.cpp -> StoragesFstat（官方 KrKr2 接口）、TemporaryFiles
//
// 本文件负责在 packinone 模块加载时确保这些模块已注册（游戏只 link
// PackinOne.dll 时也能拿到 ScriptsAdd 等方法），并注册 PackinOne 特有的
// Plugins/Script 辅助函数。

#include "packinone.h"

#define NCB_MODULE_NAME TJS_W("packinone.dll")

namespace {

bool HasGlobalMember(const tjs_char *name) {
    tTJS *engine = TVPGetScriptEngine();
    if(!engine)
        return false;
    iTJSDispatch2 *global = engine->GetGlobalNoAddRef();
    if(!global)
        return false;
    tTJSVariant value;
    return TJS_SUCCEEDED(global->PropGet(0, name, nullptr, &value, global)) &&
        value.Type() != tvtVoid;
}

// Plugins.CanLoadPlugin: 部分游戏在 link 前探测插件可用性。stub 始终
// 报告可加载——内部模块满足 link。实测星光咖啡馆启动路径不调用它，
// 保留给其他移植游戏。
tjs_error CanLoadPlugin(tTJSVariant *result, tjs_int numparams,
                        tTJSVariant **, iTJSDispatch2 *) {
    if(numparams < 1)
        return TJS_E_BADPARAMCOUNT;
    if(result)
        *result = (tjs_int)1;
    return TJS_S_OK;
}

} // namespace

NCB_ATTACH_FUNCTION(CanLoadPlugin, Plugins, CanLoadPlugin);

static void InitPlugin_PackinOneScripts() {
    // PackinOne 捆绑的辅助模块，全部由 KrKr2 本体插件提供；确保已注册。
    ncbAutoRegister::LoadModule(TJS_W("fstat.dll"));
    ncbAutoRegister::LoadModule(TJS_W("saveStruct.dll"));
    ncbAutoRegister::LoadModule(TJS_W("ScriptsEx.dll"));
    ncbAutoRegister::LoadModule(TJS_W("csvParser.dll"));
    ncbAutoRegister::LoadModule(TJS_W("layerExMovie.dll"));
    ncbAutoRegister::LoadModule(TJS_W("addFont.dll"));
    ncbAutoRegister::LoadModule(TJS_W("dirlist.dll"));

    // AffineSourceMovie：PackinOne 捆绑的影片 affine 源。KrKr2 有
    // AffineSource 但没有 Movie 子类，这里用 TJS 脚本补一个。
    if(HasGlobalMember(TJS_W("AffineSource")) &&
       !HasGlobalMember(TJS_W("AffineSourceMovie"))) {
        try {
            TVPExecuteScript(TJS_W(
                "class AffineSourceMovie extends AffineSource {"
                "  var _movie;"
                "  var _width = 0;"
                "  var _height = 0;"
                "  var _lastOwner = true;"
                "  function AffineSourceMovie(window) {"
                "    super.AffineSource(window);"
                "  }"
                "  function createLayer(orig=void) {"
                "    var src = new global.Layer(_window, _pool);"
                "    if (orig != void) {"
                "      src.assignImages(orig);"
                "      src.width = orig.width;"
                "      src.height = orig.height;"
                "      src.scale = orig.scale;"
                "    } else {"
                "      src.scale = 1.0;"
                "    }"
                "    return src;"
                "  }"
                "  function finalize() {"
                "    if (_lastOwner) {"
                "      clear();"
                "      invalidate _movie;"
                "    }"
                "  }"
                "  function clear() {"
                "    notifyOwner(\"onMotionStop\");"
                "    onMovieStop();"
                "    if (typeof kag != \"undefined\" && kag !== void)"
                "      kag.conductor.trigger(\"movie_world_foremovie\");"
                "  }"
                "  function clone(newwindow, instance) {"
                "    if (newwindow == void) newwindow = _window;"
                "    if (instance == void)"
                "      instance = new global.AffineSourceMovie(newwindow);"
                "    instance._movie = _movie;"
                "    instance._width = _width;"
                "    instance._height = _height;"
                "    _lastOwner = false;"
                "    super.clone(newwindow, instance);"
                "    return instance;"
                "  }"
                "  function canWaitMovie() {"
                "    return _movie.isPlayingMovie();"
                "  }"
                "  function isFlip() {"
                "    if (_movie.isPlayingMovie()) return true;"
                "    clear();"
                "    return false;"
                "  }"
                "  function stopMovie() {"
                "    _movie.stopMovie();"
                "  }"
                "  function drawAffine(target, mtx, src) {"
                "    (global.Layer.copyRect incontextof target)("
                "      0, 0, _movie, 0, 0, _width, _height);"
                "  }"
                "  function loadImages(storage, colorKey=clNone, options=void) {"
                "    _movie = createLayer();"
                "    _movie.openMovie(storage, false);"
                "    _movie.setSizeToImageSize();"
                "    _width = _movie.width;"
                "    _height = _movie.height;"
                "    _movie.startMovie(false);"
                "  }"
                "};"
            ));
        } catch(...) {
        }
    }

    if(!HasGlobalMember(TJS_W("AffineSourceMovie")))
        return;

    try {
        TVPExecuteScript(TJS_W(
            "if (global.extSourceMap === void) {"
            "  global.extSourceMap = %[];"
            "}"
            "extSourceMap[\".WMV\"] = AffineSourceMovie;"
            "extSourceMap[\".MPG\"] = AffineSourceMovie;"
            "extSourceMap[\".MPEG\"] = AffineSourceMovie;"
        ));
    } catch(...) {
    }
}

NCB_PRE_REGIST_CALLBACK(InitPlugin_PackinOneScripts);
