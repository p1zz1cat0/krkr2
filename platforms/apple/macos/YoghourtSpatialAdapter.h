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

// Renderer self-test hooks. They retain completed slot ownership so the
// fourth submission deterministically exercises ring saturation.
extern "C" void YoghourtKrKrSpatialTestingHoldCompletions(bool hold);
extern "C" unsigned long long YoghourtKrKrSpatialTestingDroppedFrames();
extern "C" unsigned long long YoghourtKrKrSpatialTestingMaxInFlight();
extern "C" unsigned long long YoghourtKrKrSpatialTestingSubmittedFrames();
