#import <AppKit/AppKit.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "YoghourtSpatialAdapter.h"
#include "YoghourtSpatialPresenter.h"
#include "YoghourtTelemetryCollector.h"
#include "IOSurfaceRing.h"

#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <limits>

namespace {

using yoghourt_spatial::MetalTextureFrame;
using yoghourt_spatial::Options;
using yoghourt_spatial::PixelFormat;
using yoghourt_spatial::Presenter;
using yoghourt_spatial::ScalerMode;
using yoghourt_surface_relay::EGLSharedEventAPI;
using yoghourt_surface_relay::IOSurfaceRing;
using yoghourt_surface_relay::IOSurfaceRingConfig;

struct SourceTexture {
    GLuint name = 0;
    int width = 0;
    int height = 0;
    float maxS = 1.0f;
    float maxT = 1.0f;
    bool flippedY = false;
};

struct GLState {
    struct Attribute {
        GLint index = -1;
        GLint enabled = GL_FALSE;
        GLint size = 4;
        GLint type = GL_FLOAT;
        GLint normalized = GL_FALSE;
        GLint stride = 0;
        GLint buffer = 0;
        void *pointer = nullptr;
    } attributes[2];
    GLint framebuffer = 0;
    GLint program = 0;
    GLint arrayBuffer = 0;
    GLint activeTexture = GL_TEXTURE0;
    GLint texture = 0;
    GLint viewport[4] = {};
    GLboolean blend = GL_FALSE;
    GLboolean depth = GL_FALSE;
    GLboolean stencil = GL_FALSE;
    GLboolean scissor = GL_FALSE;
    GLboolean colorMask[4] = {};
};

SourceTexture gSource;
EGLDisplay gDisplay = EGL_NO_DISPLAY;
EGLContext gContext = EGL_NO_CONTEXT;
id<MTLDevice> gDevice = nil;
CAMetalLayer *gOverlayLayer = nil;
std::unique_ptr<Presenter> gPresenter;
std::unique_ptr<IOSurfaceRing> gRing;

// Session performance telemetry (YOGHOURT_TELEMETRY=1 only). The collector is
// intentionally never deleted: Metal completion handlers keep a raw pointer
// to it and may run until process exit, the same ownership shape as the
// presenter's async-failure signal. shutdown() below stops its worker and
// joins it safely. The collector is created on the first swap hook; without a
// spatial scaler it still records frame cadence while leaving native
// presentation untouched.
yoghourt_telemetry::TelemetryCollector *gTelemetry = nullptr;
uint64_t gSwapTelemetryFrameIndex = 0;

// Bridges the engine-agnostic presenter hook to the KrKr2 collector. Lives
// for the whole process; gTelemetry may be null while disabled.
class KrKrTelemetryBridge final : public yoghourt_spatial::FrameTelemetryObserver {
public:
    uint64_t onFrameSubmitted() override {
        gSwapTelemetryFrameIndex = gTelemetry ? gTelemetry->onFrameSubmitted() : 0;
        return gSwapTelemetryFrameIndex;
    }
    void onFramePresented(uint64_t frameIndex) override {
        if (gTelemetry) gTelemetry->onFramePresented(frameIndex);
    }
    void onFrameCompleted(uint64_t frameIndex, uint64_t gpuDurationNs, bool succeeded) override {
        if (gTelemetry) gTelemetry->onFrameCompleted(frameIndex, gpuDurationNs, succeeded);
    }
    void onOutputRebuild(uint32_t oldWidth, uint32_t oldHeight, uint32_t newWidth, uint32_t newHeight) override {
        if (gTelemetry) gTelemetry->onOutputRebuild(oldWidth, oldHeight, newWidth, newHeight);
    }
    void onPipelineFailure(const char *reason) override {
        if (gTelemetry) gTelemetry->onPipelineFailure(reason);
    }
};
KrKrTelemetryBridge gTelemetryBridge;

void EnsureTelemetryCollector() {
    if (gTelemetry) return;
    const char *flag = std::getenv("YOGHOURT_TELEMETRY");
    if (!flag || std::strcmp(flag, "1") != 0) return;
    const char *sessionID = std::getenv("YOGHOURT_SESSION_ID");
    gTelemetry = new yoghourt_telemetry::TelemetryCollector(
        sessionID ? sessionID : "",
        std::make_unique<yoghourt_telemetry::StdoutOutputSink>());
}

int gPresenterWidth = 0;
int gPresenterHeight = 0;
GLuint gProgram = 0;
GLint gPosition = -1;
GLint gTexCoord = -1;
bool gLogged = false;
bool gGenerationDisabled = false;
EGLDisplay gDisabledDisplay = EGL_NO_DISPLAY;
EGLContext gDisabledContext = EGL_NO_CONTEXT;
int gDisabledWidth = 0;
int gDisabledHeight = 0;
std::atomic_bool gAsyncFailure{false};
std::atomic_bool gTestHoldCompletions{false};
uint64_t gSubmittedFrames = 0;
std::atomic<uint64_t> gCompletedFrames{0};
uint64_t gDroppedScalingFrames = 0;
uint64_t gRingSaturations = 0;
uint64_t gFallbacks = 0;
uint64_t gMaxInFlight = 0;
std::array<double, 256> gSubmitSamples{};
size_t gSubmitSampleCount = 0;
size_t gSubmitSampleCursor = 0;
std::chrono::steady_clock::time_point gMetricsEpoch;
uint64_t gMetricsSubmittedBase = 0;
uint64_t gMetricsCompletedBase = 0;

bool HasExtension(const char *extensions, const char *name) {
    if (!extensions || !name || !name[0]) return false;
    const size_t length = std::strlen(name);
    for (const char *found = std::strstr(extensions, name); found;
         found = std::strstr(found + length, name)) {
        const bool startsToken = found == extensions || found[-1] == ' ';
        const bool endsToken = found[length] == '\0' || found[length] == ' ';
        if (startsToken && endsToken) return true;
    }
    return false;
}

ScalerMode ScalerFromEnvironment() {
    const char *value = std::getenv("YOGHOURT_SPATIAL_SCALER");
    if (value && std::strcmp(value, "cunny") == 0) return ScalerMode::cuNNy;
    if (value && std::strcmp(value, "cunny+metalfx") == 0) return ScalerMode::cuNNyPlusMetalFX;
    return ScalerMode::metalFX;
}

const char *ScalerName() {
    const char *value = std::getenv("YOGHOURT_SPATIAL_SCALER");
    if (value && (std::strcmp(value, "cunny") == 0 ||
                  std::strcmp(value, "cunny+metalfx") == 0 ||
                  std::strcmp(value, "metalfx") == 0)) {
        return value;
    }
    return "metalfx";
}

bool IsEnabled() {
    const char *value = std::getenv("YOGHOURT_SPATIAL_SCALER");
    return value && value[0];
}

void HideOverlay() {
    if (gOverlayLayer) gOverlayLayer.hidden = YES;
}

void MarkFallback(const char *reason) {
    if (!gGenerationDisabled) {
        ++gFallbacks;
        if (gTelemetry) gTelemetry->onPipelineFailure(reason);
        std::fprintf(stderr, "[Yoghourt] KrKr spatial disabled: %s\n", reason);
    }
    gGenerationDisabled = true;
    gDisabledDisplay = gDisplay;
    gDisabledContext = gContext;
    gDisabledWidth = gSource.width;
    gDisabledHeight = gSource.height;
    HideOverlay();
}

void ReleaseResources() {
    if (gPresenter) {
        gPresenter->drain();
    }
    gPresenter.reset();
    if (gRing) {
        gRing->reset();
    }
    gRing.reset();
    gDisplay = EGL_NO_DISPLAY;
    gContext = EGL_NO_CONTEXT;
    gDevice = nil;
    gPresenterWidth = 0;
    gPresenterHeight = 0;
}

GLuint CompileShader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE) return shader;
    char log[1024] = {};
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    std::fprintf(stderr, "[Yoghourt] ERROR KrKr spatial shader compile failed: %s\n", log);
    glDeleteShader(shader);
    return 0;
}

bool EnsureProgram() {
    if (gProgram) return true;
    static const char *vertexSource =
        "attribute vec2 a_position;"
        "attribute vec2 a_texCoord;"
        "varying vec2 v_texCoord;"
        "void main(){v_texCoord=a_texCoord;gl_Position=vec4(a_position,0.0,1.0);}";
    static const char *fragmentSource =
        "precision mediump float;"
        "varying vec2 v_texCoord;"
        "uniform sampler2D u_texture;"
        "void main(){gl_FragColor=texture2D(u_texture,v_texCoord);}";
    const GLuint vertex = CompileShader(GL_VERTEX_SHADER, vertexSource);
    const GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, fragmentSource);
    if (!vertex || !fragment) {
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        return false;
    }
    gProgram = glCreateProgram();
    glAttachShader(gProgram, vertex);
    glAttachShader(gProgram, fragment);
    glLinkProgram(gProgram);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked = GL_FALSE;
    glGetProgramiv(gProgram, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        char log[1024] = {};
        glGetProgramInfoLog(gProgram, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[Yoghourt] ERROR KrKr spatial shader link failed: %s\n", log);
        glDeleteProgram(gProgram);
        gProgram = 0;
        return false;
    }
    gPosition = glGetAttribLocation(gProgram, "a_position");
    gTexCoord = glGetAttribLocation(gProgram, "a_texCoord");
    return gPosition >= 0 && gTexCoord >= 0;
}

id<MTLDevice> QueryMetalDevice(EGLDisplay display) {
    const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
    if (!HasExtension(extensions, "EGL_ANGLE_iosurface_client_buffer")) {
        std::fprintf(stderr, "[Yoghourt] KrKr spatial fallback: required ANGLE IOSurface extensions unavailable\n");
        return nil;
    }
    auto queryDisplay = reinterpret_cast<PFNEGLQUERYDISPLAYATTRIBEXTPROC>(
        eglGetProcAddress("eglQueryDisplayAttribEXT"));
    auto queryDevice = reinterpret_cast<PFNEGLQUERYDEVICEATTRIBEXTPROC>(
        eglGetProcAddress("eglQueryDeviceAttribEXT"));
    if (!queryDisplay || !queryDevice) return nil;
    EGLAttrib deviceAttribute = 0;
    if (!queryDisplay(display, EGL_DEVICE_EXT, &deviceAttribute)) return nil;
    auto queryDeviceString = reinterpret_cast<PFNEGLQUERYDEVICESTRINGEXTPROC>(
        eglGetProcAddress("eglQueryDeviceStringEXT"));
    const auto device = reinterpret_cast<EGLDeviceEXT>(deviceAttribute);
    if (queryDeviceString &&
        !HasExtension(queryDeviceString(device, EGL_EXTENSIONS), "EGL_ANGLE_device_metal")) {
        return nil;
    }
    EGLAttrib metalAttribute = 0;
    if (!queryDevice(device,
                     EGL_METAL_DEVICE_ANGLE,
                     &metalAttribute)) {
        return nil;
    }
    return (__bridge id<MTLDevice>)(reinterpret_cast<void *>(metalAttribute));
}

bool EnsureOverlay(void *nativeWindow, id<MTLDevice> device) {
    NSWindow *window = (__bridge NSWindow *)nativeWindow;
    NSView *view = window.contentView;
    if (!view) return false;
    view.wantsLayer = YES;
    if (!gOverlayLayer) {
        gOverlayLayer = [CAMetalLayer layer];
        gOverlayLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        gOverlayLayer.framebufferOnly = NO;
        gOverlayLayer.maximumDrawableCount = 3;
        gOverlayLayer.opaque = YES;
        gOverlayLayer.hidden = YES;
        [view.layer addSublayer:gOverlayLayer];
    }
    if (gOverlayLayer.superlayer != view.layer) {
        [gOverlayLayer removeFromSuperlayer];
        [view.layer addSublayer:gOverlayLayer];
    }
    gOverlayLayer.device = device;
    gOverlayLayer.frame = view.bounds;
    const CGFloat scale = std::max<CGFloat>(1.0, window.backingScaleFactor);
    gOverlayLayer.contentsScale = scale;
    gOverlayLayer.drawableSize = CGSizeMake(
        std::max<CGFloat>(1.0, view.bounds.size.width * scale),
        std::max<CGFloat>(1.0, view.bounds.size.height * scale));
    return true;
}

EGLSharedEventAPI LoadSharedEventSync(EGLDisplay display) {
    EGLSharedEventAPI api;
    const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
    if (!HasExtension(extensions, "EGL_ANGLE_metal_shared_event_sync")) return api;
    api.createSync = reinterpret_cast<PFNEGLCREATESYNCPROC>(eglGetProcAddress("eglCreateSync"));
    api.destroySync = reinterpret_cast<PFNEGLDESTROYSYNCPROC>(eglGetProcAddress("eglDestroySync"));
    api.waitSync = reinterpret_cast<PFNEGLWAITSYNCPROC>(eglGetProcAddress("eglWaitSync"));
    api.copyMetalSharedEvent = reinterpret_cast<PFNEGLCOPYMETALSHAREDEVENTANGLEPROC>(
        eglGetProcAddress("eglCopyMetalSharedEventANGLE"));
    return api;
}

bool CreateSharedResources(EGLDisplay display,
                           EGLConfig config,
                           EGLContext context,
                           void *nativeWindow) {
    if (gSource.width <= 0 || gSource.height <= 0) return false;
    id<MTLDevice> device = QueryMetalDevice(display);
    if (!device || !EnsureOverlay(nativeWindow, device)) return false;

    EGLSharedEventAPI syncAPI = LoadSharedEventSync(display);
    if (!syncAPI.valid()) {
        ReleaseResources();
        return false;
    }

    // Ring generation change detection lives inside the ring: config match
    // is a no-op, otherwise it drains and recreates all slots. The Metal
    // presenter (the only in-flight consumer) must be drained BEFORE the
    // rebuild releases the old IOSurface resources.
    IOSurfaceRingConfig ringConfig;
    ringConfig.display = display;
    ringConfig.config = config;
    ringConfig.metalDevice = (__bridge void *)device;
    ringConfig.width = gSource.width;
    ringConfig.height = gSource.height;
    ringConfig.syncAPI = syncAPI;
    if (gRing && !gRing->matches(ringConfig)) {
        if (gPresenter) {
            gPresenter->drain();
        }
        gPresenter.reset();
        gPresenterWidth = 0;
        gPresenterHeight = 0;
    }
    if (!gRing) {
        gRing = std::make_unique<IOSurfaceRing>();
    }
    if (!gRing->rebuild(ringConfig)) {
        if (!gGenerationDisabled) MarkFallback("shared resource creation failed");
        return false;
    }
    gRing->setDeferredFree(gTestHoldCompletions.load(std::memory_order_acquire));

    gDisplay = display;
    gContext = context;
    gDevice = device;
    gGenerationDisabled = false;

    if (gPresenter && gPresenterWidth == gSource.width &&
        gPresenterHeight == gSource.height) {
        return true;
    }
    if (gPresenter) {
        gPresenter->drain();
        gPresenter.reset();
    }
    Options options;
    options.scaler = ScalerFromEnvironment();
    options.enableOverlayMask = false;
    options.telemetryObserver = gTelemetry ? &gTelemetryBridge : nullptr;
    gPresenter = std::make_unique<Presenter>((__bridge void *)gOverlayLayer,
                                             gSource.width,
                                             gSource.height,
                                             options);
    if (!gPresenter->isActive()) {
        MarkFallback("SpatialPresenter initialization failed");
        ReleaseResources();
        return false;
    }
    gPresenterWidth = gSource.width;
    gPresenterHeight = gSource.height;
    return true;
}

void CaptureGLState(GLState &state) {
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &state.framebuffer);
    glGetIntegerv(GL_CURRENT_PROGRAM, &state.program);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &state.arrayBuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &state.activeTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &state.texture);
    glGetIntegerv(GL_VIEWPORT, state.viewport);
    state.blend = glIsEnabled(GL_BLEND);
    state.depth = glIsEnabled(GL_DEPTH_TEST);
    state.stencil = glIsEnabled(GL_STENCIL_TEST);
    state.scissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetBooleanv(GL_COLOR_WRITEMASK, state.colorMask);
    const GLint indices[] = {gPosition, gTexCoord};
    for (int i = 0; i < 2; ++i) {
        auto &attribute = state.attributes[i];
        attribute.index = indices[i];
        glGetVertexAttribiv(indices[i], GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attribute.enabled);
        glGetVertexAttribiv(indices[i], GL_VERTEX_ATTRIB_ARRAY_SIZE, &attribute.size);
        glGetVertexAttribiv(indices[i], GL_VERTEX_ATTRIB_ARRAY_TYPE, &attribute.type);
        glGetVertexAttribiv(indices[i], GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &attribute.normalized);
        glGetVertexAttribiv(indices[i], GL_VERTEX_ATTRIB_ARRAY_STRIDE, &attribute.stride);
        glGetVertexAttribiv(indices[i], GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &attribute.buffer);
        glGetVertexAttribPointerv(indices[i], GL_VERTEX_ATTRIB_ARRAY_POINTER, &attribute.pointer);
    }
}

void RestoreGLState(const GLState &state) {
    glBindFramebuffer(GL_FRAMEBUFFER, state.framebuffer);
    glUseProgram(state.program);
    glBindBuffer(GL_ARRAY_BUFFER, state.arrayBuffer);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, state.texture);
    glActiveTexture(state.activeTexture);
    glViewport(state.viewport[0], state.viewport[1], state.viewport[2], state.viewport[3]);
    state.blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
    state.depth ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
    state.stencil ? glEnable(GL_STENCIL_TEST) : glDisable(GL_STENCIL_TEST);
    state.scissor ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
    glColorMask(state.colorMask[0], state.colorMask[1], state.colorMask[2], state.colorMask[3]);
    for (const auto &attribute : state.attributes) {
        glBindBuffer(GL_ARRAY_BUFFER, attribute.buffer);
        // A disabled attribute's default null client pointer is not a valid
        // glVertexAttribPointer input on ANGLE. Its pointer is irrelevant
        // while disabled, so only restore concrete attribute bindings.
        if (attribute.enabled || attribute.buffer != 0 || attribute.pointer != nullptr) {
            glVertexAttribPointer(attribute.index,
                                  attribute.size,
                                  attribute.type,
                                  attribute.normalized,
                                  attribute.stride,
                                  attribute.pointer);
        }
        attribute.enabled ? glEnableVertexAttribArray(attribute.index)
                          : glDisableVertexAttribArray(attribute.index);
    }
    glBindBuffer(GL_ARRAY_BUFFER, state.arrayBuffer);
}

bool CopySourceToIOSurface(IOSurfaceRing::Slot &slot,
                           EGLDisplay display,
                           EGLSurface windowSurface,
                           EGLContext context) {
    if (!EnsureProgram()) {
        std::fprintf(stderr, "[Yoghourt] KrKr spatial copy failed: GL program\n");
        return false;
    }
    GLState state;
    CaptureGLState(state);
    if (!gRing->waitPreviousMetalDone(&slot)) {
        std::fprintf(stderr, "[Yoghourt] KrKr spatial copy failed: metalDone wait egl=0x%x\n", eglGetError());
        return false;
    }
    if (!eglMakeCurrent(display, gRing->pbufferOf(&slot), gRing->pbufferOf(&slot), context)) {
        std::fprintf(stderr, "[Yoghourt] KrKr spatial copy failed: pbuffer makeCurrent egl=0x%x\n", eglGetError());
        return false;
    }

    // ANGLE's IOSurface bridge exposes EGL row zero (the bottom row) to
    // Metal as texture y=0. The shared Metal presenter samples uv.y=0 at
    // the top of its drawable, so the copy pass must put the source's top
    // row into the IOSurface bottom row. This is the opposite of the
    // source sprite's normal GL presentation mapping. Keep the explicit
    // sprite flip in the equation so textures which are intentionally
    // flipped still preserve their logical orientation.
    const float bottomT = gSource.flippedY ? gSource.maxT : 0.0f;
    const float topT = gSource.flippedY ? 0.0f : gSource.maxT;
    const GLfloat vertices[] = {
        -1.0f, -1.0f, 0.0f,         bottomT,
         1.0f, -1.0f, gSource.maxS, bottomT,
        -1.0f,  1.0f, 0.0f,         topT,
         1.0f,  1.0f, gSource.maxS, topT,
    };
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, gSource.width, gSource.height);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(gProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gSource.name);
    glUniform1i(glGetUniformLocation(gProgram, "u_texture"), 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray(gPosition);
    glEnableVertexAttribArray(gTexCoord);
    while (glGetError() != GL_NO_ERROR) {}
    glVertexAttribPointer(gPosition, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), vertices);
    glVertexAttribPointer(gTexCoord, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), vertices + 2);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(gPosition);
    glDisableVertexAttribArray(gTexCoord);
    const GLenum drawError = glGetError();
    const bool signaled = drawError == GL_NO_ERROR && gRing->signalAngleReady(&slot);
    const bool drew = drawError == GL_NO_ERROR && signaled;
    const bool restored = eglMakeCurrent(display, windowSurface, windowSurface, context) == EGL_TRUE;
    if (!drew || !restored) {
        std::fprintf(stderr,
                     "[Yoghourt] KrKr spatial copy failed: draw=0x%x readySignal=%d restore=%d egl=0x%x\n",
                     drawError, signaled, restored, eglGetError());
    }
    RestoreGLState(state);
    return drew && restored;
}

void SpatialCompletion(void *context, bool succeeded) {
    auto *slot = static_cast<IOSurfaceRing::Slot *>(context);
    if (!succeeded) gAsyncFailure.store(true, std::memory_order_release);
    gCompletedFrames.fetch_add(1, std::memory_order_relaxed);
    gRing->complete(slot, succeeded);
}

void RecordSubmitTime(double milliseconds) {
    gSubmitSamples[gSubmitSampleCursor] = milliseconds;
    gSubmitSampleCursor = (gSubmitSampleCursor + 1) % gSubmitSamples.size();
    gSubmitSampleCount = std::min(gSubmitSampleCount + 1, gSubmitSamples.size());
}

void LogMetricsIfDue() {
    const auto now = std::chrono::steady_clock::now();
    if (gMetricsEpoch.time_since_epoch().count() == 0) {
        gMetricsEpoch = now;
        return;
    }
    const double seconds = std::chrono::duration<double>(now - gMetricsEpoch).count();
    if (seconds < 1.0) return;
    std::array<double, 256> sorted = gSubmitSamples;
    std::sort(sorted.begin(), sorted.begin() + gSubmitSampleCount);
    const auto percentile = [&](double fraction) {
        if (gSubmitSampleCount == 0) return 0.0;
        const size_t index = std::min(gSubmitSampleCount - 1,
                                      static_cast<size_t>((gSubmitSampleCount - 1) * fraction));
        return sorted[index];
    };
    const uint64_t completed = gCompletedFrames.load(std::memory_order_relaxed);
    std::fprintf(stdout,
                 "[Yoghourt] KrKr spatial metrics submitCPU.p50=%.3fms submitCPU.p95=%.3fms submittedFPS=%.1f completedFPS=%.1f droppedScalingFrames=%llu ringSaturation=%llu fallback=%llu inFlight=%llu maxInFlight=%llu\n",
                 percentile(0.50), percentile(0.95),
                 (gSubmittedFrames - gMetricsSubmittedBase) / seconds,
                 (completed - gMetricsCompletedBase) / seconds,
                 (unsigned long long)gDroppedScalingFrames,
                 (unsigned long long)gRingSaturations,
                 (unsigned long long)gFallbacks,
                 (unsigned long long)(gRing ? gRing->inFlight() : 0),
                 (unsigned long long)gMaxInFlight);
    std::fflush(stdout);
    gMetricsEpoch = now;
    gMetricsSubmittedBase = gSubmittedFrames;
    gMetricsCompletedBase = completed;
}

} // namespace

extern "C" void YoghourtKrKrSpatialRegisterSourceTexture(
    unsigned int texture,
    int width,
    int height,
    float maxS,
    float maxT,
    bool flippedY) {
    gSource.name = texture;
    gSource.width = width;
    gSource.height = height;
    gSource.maxS = std::clamp(maxS, 0.0f, 1.0f);
    gSource.maxT = std::clamp(maxT, 0.0f, 1.0f);
    gSource.flippedY = flippedY;
}

// core 侧定义（cpp/core/visual/RenderManager.h）。这里前向声明以避免把
// visual 的头文件引入平台适配层。
const char *TVPGetActiveRenderManagerName();
bool TVPGetOglAccurateRender();

extern "C" void YoghourtKrKrTelemetryRecordFrameStages(
    double tickMs,
    double renderMs,
    double swapMs) {
    if (gTelemetry && gSwapTelemetryFrameIndex != 0) {
        // render manager 是懒选择的，可能晚于 collector 构造，因此在帧边界
        // 上报而不是在 collector 启动时；collector 内部保证只记一次。
        const char *rendererName = TVPGetActiveRenderManagerName();
        if (rendererName && *rendererName) {
            gTelemetry->onRendererSelected(rendererName, TVPGetOglAccurateRender());
        }
        gTelemetry->onFrameStages(gSwapTelemetryFrameIndex, tickMs, renderMs, swapMs);
    }
    gSwapTelemetryFrameIndex = 0;
}

extern "C" bool YoghourtKrKrSpatialPresent(
    EGLDisplay display,
    EGLSurface windowSurface,
    EGLConfig config,
    EGLContext context,
    void *nativeWindow) {
    gSwapTelemetryFrameIndex = 0;
    if (gSource.name == 0 || gSource.width <= 0 || gSource.height <= 0 ||
        display == EGL_NO_DISPLAY || windowSurface == EGL_NO_SURFACE ||
        context == EGL_NO_CONTEXT || !nativeWindow) {
        HideOverlay();
        return false;
    }
    EnsureTelemetryCollector();

    // The swap hook is also the only common frame boundary when spatial
    // scaling is disabled. Keep interval telemetry without enabling or
    // changing the native renderer: there is no GPU completion to report on
    // this branch, so the sample is completed with an unavailable GPU value.
    if (!IsEnabled()) {
        if (gTelemetry) {
            const uint64_t frameIndex = gTelemetry->onFrameSubmitted();
            gSwapTelemetryFrameIndex = frameIndex;
            if (frameIndex != 0) {
                gTelemetry->onFrameCompleted(frameIndex, 0, false);
            }
        }
        HideOverlay();
        return false;
    }
    if (gGenerationDisabled) {
        const bool generationChanged = display != gDisabledDisplay ||
            context != gDisabledContext || gSource.width != gDisabledWidth ||
            gSource.height != gDisabledHeight;
        if (!generationChanged) return false;
        ReleaseResources();
        gGenerationDisabled = false;
    }
    if (gAsyncFailure.exchange(false, std::memory_order_acq_rel)) {
        MarkFallback("asynchronous Metal submission failed");
        return false;
    }
    if (gRing && gRing->countersExhausted()) {
        // Counter rollover is a generation boundary, never a frame-path wait.
        ReleaseResources();
    }
    if (!CreateSharedResources(display, config, context, nativeWindow)) {
        if (!gGenerationDisabled) MarkFallback("shared resource creation failed");
        return false;
    }
    IOSurfaceRing::Slot *slot = gRing->acquire();
    if (!slot) {
        ++gDroppedScalingFrames;
        ++gRingSaturations;
        if (gTelemetry) gTelemetry->onRingBusyDrop();
        LogMetricsIfDue();
        return true;
    }
    gMaxInFlight = std::max(gMaxInFlight, (uint64_t)gRing->inFlight());
    const auto submitStart = std::chrono::steady_clock::now();
    if (!CopySourceToIOSurface(*slot, display, windowSurface, context)) {
        gRing->forceFree(slot);
        MarkFallback("ANGLE shared-texture copy or synchronization failed");
        return false;
    }
    if (!gRing->assignMetalDone(slot)) {
        gRing->forceFree(slot);
        MarkFallback("shared-event counter exhausted; resource generation rebuild required");
        return false;
    }
    MetalTextureFrame frame(gRing->textureOf(slot),
                            gRing->width(),
                            gRing->height(),
                            PixelFormat::bgra8Unorm,
                            gRing->angleReadyEvent(),
                            gRing->angleReadyValueOf(slot),
                            gRing->metalDoneEvent(),
                            gRing->metalDoneValueOf(slot),
                            SpatialCompletion,
                            slot);
    gOverlayLayer.hidden = NO;
    if (!gPresenter->present(frame)) {
        gRing->forceFree(slot);
        MarkFallback("SpatialPresenter external submission rejected");
        return false;
    }
    ++gSubmittedFrames;
    RecordSubmitTime(std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - submitStart).count());
    LogMetricsIfDue();
    if (!gLogged) {
        std::fprintf(stdout,
                     "[Yoghourt] SPATIAL engine=kirikiri bridge=iosurface-metal source=%dx%d sync=metal-shared-event buffers=3 glFinish=none externalWait=none readback=none scaler=%s\n",
                     gSource.width,
                     gSource.height,
                     ScalerName());
        std::fflush(stdout);
        gLogged = true;
    }
    return true;
}

extern "C" void YoghourtKrKrSpatialShutdown() {
    HideOverlay();
    if (gOverlayLayer) {
        [gOverlayLayer removeFromSuperlayer];
        gOverlayLayer = nil;
    }
    if (gProgram) {
        glDeleteProgram(gProgram);
        gProgram = 0;
    }
    ReleaseResources();
    // Drain the telemetry worker after the presenter drain so in-flight
    // completions are accounted for. The collector object itself is never
    // deleted (see the ownership note at its declaration).
    if (gTelemetry) {
        gTelemetry->shutdown(yoghourt_telemetry::TelemetryCollector::kShutdownDrainTimeoutMs);
    }
    gSwapTelemetryFrameIndex = 0;
    gSource = {};
    gLogged = false;
    gGenerationDisabled = false;
    gDisabledDisplay = EGL_NO_DISPLAY;
    gDisabledContext = EGL_NO_CONTEXT;
    gDisabledWidth = 0;
    gDisabledHeight = 0;
    gAsyncFailure.store(false, std::memory_order_release);
    gTestHoldCompletions.store(false, std::memory_order_release);
    if (gRing) gRing->setDeferredFree(false);
    gSubmittedFrames = 0;
    gCompletedFrames.store(0, std::memory_order_release);
    gDroppedScalingFrames = 0;
    gRingSaturations = 0;
    gFallbacks = 0;
    gMaxInFlight = 0;
    gSubmitSampleCount = 0;
    gSubmitSampleCursor = 0;
    gMetricsEpoch = {};
    gMetricsSubmittedBase = 0;
    gMetricsCompletedBase = 0;
}

extern "C" void YoghourtKrKrSpatialTestingHoldCompletions(bool hold) {
    gTestHoldCompletions.store(hold, std::memory_order_release);
    if (gRing) {
        gRing->setDeferredFree(hold);
        if (!hold) gRing->flushDeferredFree();
    }
}

extern "C" unsigned long long YoghourtKrKrSpatialTestingDroppedFrames() {
    return gDroppedScalingFrames;
}

extern "C" unsigned long long YoghourtKrKrSpatialTestingMaxInFlight() {
    return gMaxInFlight;
}

extern "C" unsigned long long YoghourtKrKrSpatialTestingSubmittedFrames() {
    return gSubmittedFrames;
}
