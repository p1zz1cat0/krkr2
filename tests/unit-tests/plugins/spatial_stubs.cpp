// macOS 测试链接桩：core_environ 的 TVPWindow 等对象引用了 Yoghourt 的
// Spatial Presenter 符号，而测试可执行文件不链接主程序的 YoghourtSpatialPresenter.mm。
// 与 tests/unit-tests/core/tjs2/tjs.cpp 中的桩保持同一签名。
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
