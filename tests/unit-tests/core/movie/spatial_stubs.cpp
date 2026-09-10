// macOS 测试链接桩：与 tests/unit-tests/core/tjs2/spatial_stubs.cpp 一致，
// core 测试不链接 krkr2plugin，因此补齐插件锚点。
#if defined(__APPLE__)
extern "C" void YoghourtApplyWindowPresentation(void *) {}
extern "C" bool YoghourtGameWindowIsFullscreen(void) { return false; }
extern "C" void YoghourtKrKrSpatialRegisterSourceTexture(unsigned int, int, int,
                                                         float, float, bool) {}
extern "C" bool YoghourtKrKrSpatialPresent(void *, void *, void *, void *,
                                           void *) {
    return false;
}
extern "C" void YoghourtKrKrSpatialShutdown() {}
extern "C" void YoghourtKrKrTelemetryRecordFrameStages(double, double, double) {}
#endif
// 链接锚点桩：与 tests/unit-tests/core/tjs2/spatial_stubs.cpp 一致
extern "C" void TVPGetLangNamePluginAnchor() {}
extern "C" void TVPShrinkCopyPluginAnchor() {}
extern "C" void TVPLayerExRasterPluginAnchor() {}
extern "C" void TVPFstatPluginAnchor() {}
extern "C" void TVPLayerExBtoAPluginAnchor() {}
extern "C" void TVPPackinOnePluginAnchor() {}
extern "C" void TVPExtransPluginAnchor() {}
extern "C" void TVPAlphaMoviePluginAnchor() {}
extern "C" void TVPWuVorbisPluginAnchor() {}
extern "C" void TVPExtNaganoPluginAnchor() {}
extern "C" void TVPMotionPlayerPluginAnchor() {}

// 以下锚点随后续注册型插件加入 PluginImpl.cpp，但桩文件当时未同步——
// core 测试既不被质量门构建也不被运行，链接失败因此一直没有暴露。
extern "C" void TVPAddFontPluginAnchor() {}
extern "C" void TVPCsvParserPluginAnchor() {}
extern "C" void TVPDirListPluginAnchor() {}
extern "C" void TVPFftGraphPluginAnchor() {}
extern "C" void TVPGetAboutPluginAnchor() {}
extern "C" void TVPGetSamplePluginAnchor() {}
extern "C" void TVPKAGParserExPluginAnchor() {}
extern "C" void TVPLayerExMoviePluginAnchor() {}
extern "C" void TVPLayerExPerspectivePluginAnchor() {}
extern "C" void TVPSaveStructPluginAnchor() {}
extern "C" void TVPScriptsExPluginAnchor() {}
extern "C" void TVPTextRenderPluginAnchor() {}
extern "C" void TVPVarFilePluginAnchor() {}
extern "C" void TVPWin32DialogPluginAnchor() {}
extern "C" void TVPWindowExPluginAnchor() {}
extern "C" void TVPWutcwfPluginAnchor() {}
