// layerExSave.dll 模块入口。上游 Main.cpp 的 Window 异步保存扩展未移植，
// 原因见 utils.hpp 顶部说明；Layer/Math 扩展注册在各自源文件中。
#include "ncbind.hpp"
#include "utils.hpp"

namespace {
// NCB_ATTACH_FUNCTION 在目标类尚未存在时静默跳过注册。在预注册阶段解析
// Layer，使 Plugins.link("layerExSave.dll") 在首个 Window/Layer 创建前也生效。
void EnsureLayerClassRegistered() {
    tTJSVariant layerClass;
    TVPExecuteExpression(TJS_W("Layer"), &layerClass);
    if(layerClass.Type() != tvtObject)
        TVPThrowExceptionMessage(
            TJS_W("layerExSave.dll requires the Layer class."));
}
} // namespace

NCB_PRE_REGIST_CALLBACK(EnsureLayerClassRegistered);

// 静态库中只含注册的翻译单元会被链接器丢弃：PluginImpl.cpp 引用此锚点，
// 此锚点再引用每个含 NCB_ATTACH_FUNCTION 的翻译单元。
void LayerExSaveUtilsAnchor();
void LayerExSavePngAnchor();
void LayerExSaveTlg5Anchor();
extern "C" void TVPLayerExSavePluginAnchor() {
    LayerExSaveUtilsAnchor();
    LayerExSavePngAnchor();
    LayerExSaveTlg5Anchor();
}
