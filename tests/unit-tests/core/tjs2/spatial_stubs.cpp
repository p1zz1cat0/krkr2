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
extern "C" void TVPGetLangNamePluginAnchor() {}
