// macOS 测试链接桩：core_environ 的 TVPWindow 等对象引用了 Yoghourt 的
// Spatial Presenter 符号，而测试可执行文件不链接主程序的 YoghourtSpatialPresenter.mm。
// 与 tests/unit-tests/core/tjs2/tjs.cpp 中的桩保持同一签名。
#if defined(__APPLE__)
extern "C" void YoghourtApplyWindowPresentation(void *) {}
extern "C" void YoghourtSetGameWindowContentSize(int, int) {}
extern "C" bool YoghourtGameWindowIsFullscreen(void) { return false; }
extern "C" void YoghourtKrKrSpatialRegisterSourceTexture(unsigned int, int, int,
                                                         float, float, bool) {}
extern "C" bool YoghourtKrKrSpatialPresent(void *, void *, void *, void *,
                                           void *) {
    return false;
}
extern "C" void YoghourtKrKrSpatialShutdown() {}
extern "C" void YoghourtKrKrSetFrameLimit(int) {}
extern "C" void YoghourtKrKrTelemetryRecordFrameStages(double, double, double) {}
// 光标宿主（platforms/apple/macos/YoghourtCursor.mm）不链接进测试，补桩。
extern "C" void *YoghourtCreateRuntimeGLView(const char *, int, int) {
    return nullptr;
}
extern "C" void YoghourtCursorApply(int) {}
extern "C" void YoghourtCursorSetHidden(int) {}
extern "C" void YoghourtCursorHideUntilMouseMoves() {}
extern "C" void YoghourtCursorTick() {}
#endif
