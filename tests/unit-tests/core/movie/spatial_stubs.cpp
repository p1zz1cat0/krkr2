// macOS 测试链接桩：与 tests/unit-tests/core/tjs2/spatial_stubs.cpp 一致，
// core 测试不链接 krkr2plugin，因此补齐插件锚点。
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
