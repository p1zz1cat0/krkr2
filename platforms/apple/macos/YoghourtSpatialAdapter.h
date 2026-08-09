#pragma once

#include <EGL/egl.h>

extern "C" void YoghourtKrKrSpatialRegisterSourceTexture(
    unsigned int texture,
    int width,
    int height,
    float maxS,
    float maxT,
    bool flippedY);

extern "C" bool YoghourtKrKrSpatialPresent(
    EGLDisplay display,
    EGLSurface windowSurface,
    EGLConfig config,
    EGLContext context,
    void *nativeWindow);

extern "C" void YoghourtKrKrSpatialShutdown();
