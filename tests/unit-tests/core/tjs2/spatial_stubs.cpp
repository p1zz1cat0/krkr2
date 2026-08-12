// macOS 测试链接桩：core_environ 的 TVPWindow 等对象引用了 Yoghourt 的
// Spatial Presenter 符号，而测试可执行文件不链接主程序的 YoghourtSpatialPresenter.mm。
// TVPGetLangNamePluginAnchor 由 krkr2plugin 的 getLangName.cpp（PRIVATE）提供，
// core 测试不链接插件库，因此一并补桩。
#if defined(__APPLE__)
extern "C" void YoghourtApplyWindowPresentation(void *) {}
extern "C" void YoghourtKrKrSpatialRegisterSourceTexture(unsigned int, int, int,
                                                         float, float, bool) {}
extern "C" bool YoghourtKrKrSpatialPresent(void *, void *, void *, void *,
                                           void *) {
    return false;
}
extern "C" void YoghourtKrKrSpatialShutdown() {}
#endif
// 链接锚点桩：注册型插件是 krkr2plugin 聚合库的 PRIVATE source，core 测试
// 不链接插件库，PluginImpl.cpp 引用这些锚点时由本文件补全。
extern "C" void TVPGetLangNamePluginAnchor() {}
extern "C" void TVPShrinkCopyPluginAnchor() {}
extern "C" void TVPLayerExRasterPluginAnchor() {}
extern "C" void TVPFstatPluginAnchor() {}
extern "C" void TVPLayerExBtoAPluginAnchor() {}
extern "C" void TVPPackinOnePluginAnchor() {}
