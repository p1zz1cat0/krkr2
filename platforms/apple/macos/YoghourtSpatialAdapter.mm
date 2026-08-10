#import <AppKit/AppKit.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "YoghourtSpatialAdapter.h"
#include "YoghourtSpatialPresenter.h"

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

struct SourceTexture {
    GLuint name = 0;
    int width = 0;
    int height = 0;
    float maxS = 1.0f;
    float maxT = 1.0f;
    bool flippedY = false;
};

enum class SlotOwnership : uint8_t {
    free = 0,
    angleQueued = 1,
    metalQueued = 2,
};

struct SharedSlot {
    IOSurfaceRef ioSurface = nullptr;
    EGLSurface pbuffer = EGL_NO_SURFACE;
    id<MTLTexture> texture = nil;
    std::atomic<uint8_t> ownership{static_cast<uint8_t>(SlotOwnership::free)};
    std::atomic_bool completionHeld{false};
    uint64_t angleReadyValue = 0;
    uint64_t metalDoneValue = 0;
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
id<MTLSharedEvent> gAngleReadyEvent = nil;
id<MTLSharedEvent> gMetalDoneEvent = nil;
CAMetalLayer *gOverlayLayer = nil;
std::unique_ptr<Presenter> gPresenter;
std::array<SharedSlot, 3> gSlots;
size_t gNextSlot = 0;
uint64_t gNextAngleReadyValue = 1;
uint64_t gNextMetalDoneValue = 1;
PFNEGLCREATESYNCPROC gCreateSync = nullptr;
PFNEGLDESTROYSYNCPROC gDestroySync = nullptr;
PFNEGLWAITSYNCPROC gWaitSync = nullptr;
PFNEGLCOPYMETALSHAREDEVENTANGLEPROC gCopyMetalSharedEvent = nullptr;
GLuint gProgram = 0;
GLint gPosition = -1;
GLint gTexCoord = -1;
int gResourceWidth = 0;
int gResourceHeight = 0;
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
    for (auto &slot : gSlots) {
        slot.texture = nil;
        if (slot.pbuffer != EGL_NO_SURFACE && gDisplay != EGL_NO_DISPLAY) {
            eglDestroySurface(gDisplay, slot.pbuffer);
        }
        slot.pbuffer = EGL_NO_SURFACE;
        if (slot.ioSurface) {
            CFRelease(slot.ioSurface);
            slot.ioSurface = nullptr;
        }
        slot.angleReadyValue = 0;
        slot.metalDoneValue = 0;
        slot.ownership.store(static_cast<uint8_t>(SlotOwnership::free), std::memory_order_release);
        slot.completionHeld.store(false, std::memory_order_release);
    }
    gAngleReadyEvent = nil;
    gMetalDoneEvent = nil;
    gDisplay = EGL_NO_DISPLAY;
    gContext = EGL_NO_CONTEXT;
    gDevice = nil;
    gCreateSync = nullptr;
    gDestroySync = nullptr;
    gWaitSync = nullptr;
    gCopyMetalSharedEvent = nullptr;
    gNextSlot = 0;
    gNextAngleReadyValue = 1;
    gNextMetalDoneValue = 1;
    gResourceWidth = 0;
    gResourceHeight = 0;
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

EGLAttrib ValueLow(uint64_t value) {
    return static_cast<EGLAttrib>(static_cast<uint32_t>(value));
}

EGLAttrib ValueHigh(uint64_t value) {
    return static_cast<EGLAttrib>(static_cast<uint32_t>(value >> 32));
}

bool LoadSharedEventSync(EGLDisplay display) {
    const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
    if (!HasExtension(extensions, "EGL_ANGLE_metal_shared_event_sync")) return false;
    gCreateSync = reinterpret_cast<PFNEGLCREATESYNCPROC>(eglGetProcAddress("eglCreateSync"));
    gDestroySync = reinterpret_cast<PFNEGLDESTROYSYNCPROC>(eglGetProcAddress("eglDestroySync"));
    gWaitSync = reinterpret_cast<PFNEGLWAITSYNCPROC>(eglGetProcAddress("eglWaitSync"));
    gCopyMetalSharedEvent = reinterpret_cast<PFNEGLCOPYMETALSHAREDEVENTANGLEPROC>(
        eglGetProcAddress("eglCopyMetalSharedEventANGLE"));
    return gCreateSync && gDestroySync && gWaitSync && gCopyMetalSharedEvent;
}

bool CreateGenerationEvents(EGLDisplay display, id<MTLDevice> device) {
    const EGLAttrib attributes[] = {
        EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_LO_ANGLE, 0,
        EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_HI_ANGLE, 0,
        EGL_NONE,
    };
    EGLSync sync = gCreateSync(display, EGL_SYNC_METAL_SHARED_EVENT_ANGLE, attributes);
    if (sync == EGL_NO_SYNC) return false;
    void *event = gCopyMetalSharedEvent(display, sync);
    const bool destroyed = gDestroySync(display, sync) == EGL_TRUE;
    if (!event || !destroyed) {
        if (event) CFRelease(event);
        return false;
    }
    gAngleReadyEvent = (__bridge_transfer id<MTLSharedEvent>)event;
    gMetalDoneEvent = [device newSharedEvent];
    const auto eventCompatible = [](id<MTLSharedEvent> event, id<MTLDevice> rhs) {
        id<MTLDevice> lhs = event.device;
        if (!lhs) return true;
        return lhs && rhs && (lhs == rhs || lhs.registryID == rhs.registryID);
    };
    return gAngleReadyEvent && gMetalDoneEvent &&
           eventCompatible(gAngleReadyEvent, device) &&
           eventCompatible(gMetalDoneEvent, device);
}

bool EnqueueMetalDoneWait(SharedSlot &slot) {
    if (slot.metalDoneValue == 0) return true;
    const EGLAttrib attributes[] = {
        EGL_SYNC_METAL_SHARED_EVENT_OBJECT_ANGLE,
        reinterpret_cast<EGLAttrib>((__bridge void *)gMetalDoneEvent),
        EGL_SYNC_CONDITION, EGL_SYNC_METAL_SHARED_EVENT_SIGNALED_ANGLE,
        EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_LO_ANGLE, ValueLow(slot.metalDoneValue),
        EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_HI_ANGLE, ValueHigh(slot.metalDoneValue),
        EGL_NONE,
    };
    EGLSync sync = gCreateSync(gDisplay, EGL_SYNC_METAL_SHARED_EVENT_ANGLE, attributes);
    if (sync == EGL_NO_SYNC) return false;
    const bool waited = gWaitSync(gDisplay, sync, 0) == EGL_TRUE;
    const bool destroyed = gDestroySync(gDisplay, sync) == EGL_TRUE;
    return waited && destroyed;
}

bool EnqueueAngleReadySignal(SharedSlot &slot) {
    if (gNextAngleReadyValue == 0 ||
        gNextAngleReadyValue == std::numeric_limits<uint64_t>::max()) {
        return false;
    }
    const uint64_t value = gNextAngleReadyValue++;
    const EGLAttrib attributes[] = {
        EGL_SYNC_METAL_SHARED_EVENT_OBJECT_ANGLE,
        reinterpret_cast<EGLAttrib>((__bridge void *)gAngleReadyEvent),
        EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_LO_ANGLE, ValueLow(value),
        EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_HI_ANGLE, ValueHigh(value),
        EGL_NONE,
    };
    EGLSync sync = gCreateSync(gDisplay, EGL_SYNC_METAL_SHARED_EVENT_ANGLE, attributes);
    if (sync == EGL_NO_SYNC) return false;
    const bool destroyed = gDestroySync(gDisplay, sync) == EGL_TRUE;
    if (!destroyed) return false;
    glFlush();
    slot.angleReadyValue = value;
    return true;
}

bool CreateSharedResources(EGLDisplay display,
                           EGLConfig config,
                           EGLContext context,
                           void *nativeWindow) {
    if (gSource.width <= 0 || gSource.height <= 0) return false;
    id<MTLDevice> device = QueryMetalDevice(display);
    if (!device || !EnsureOverlay(nativeWindow, device)) return false;

    if (gSlots[0].pbuffer != EGL_NO_SURFACE && gDisplay == display && gContext == context &&
        gResourceWidth == gSource.width && gResourceHeight == gSource.height &&
        gDevice == device && gPresenter && !gGenerationDisabled) {
        return true;
    }
    ReleaseResources();
    if (!EnsureOverlay(nativeWindow, device)) return false;
    gDisplay = display;
    gContext = context;
    gDevice = device;
    gGenerationDisabled = false;

    if (!LoadSharedEventSync(display) || !CreateGenerationEvents(display, device)) {
        MarkFallback("EGL_ANGLE_metal_shared_event_sync unavailable");
        ReleaseResources();
        return false;
    }

    NSDictionary *properties = @{
        (NSString *)kIOSurfaceWidth: @(gSource.width),
        (NSString *)kIOSurfaceHeight: @(gSource.height),
        (NSString *)kIOSurfaceBytesPerElement: @4,
        (NSString *)kIOSurfacePixelFormat: @(static_cast<uint32_t>('BGRA')),
    };
    const EGLint attributes[] = {
        EGL_WIDTH, gSource.width,
        EGL_HEIGHT, gSource.height,
        EGL_IOSURFACE_PLANE_ANGLE, 0,
        EGL_TEXTURE_TARGET, EGL_TEXTURE_2D,
        EGL_TEXTURE_INTERNAL_FORMAT_ANGLE, GL_BGRA_EXT,
        EGL_TEXTURE_FORMAT, EGL_TEXTURE_RGBA,
        EGL_TEXTURE_TYPE_ANGLE, GL_UNSIGNED_BYTE,
        EGL_NONE,
    };
    MTLTextureDescriptor *descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:gSource.width
                                                          height:gSource.height
                                                       mipmapped:NO];
    // IOSurface-backed textures use shared storage. The frame still remains
    // GPU-only: neither the adapter nor SpatialPresenter maps this storage.
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite |
                       MTLTextureUsageRenderTarget;
    for (auto &slot : gSlots) {
        slot.ioSurface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
        if (!slot.ioSurface) {
            MarkFallback("IOSurface allocation failed");
            ReleaseResources();
            return false;
        }
        slot.pbuffer = eglCreatePbufferFromClientBuffer(
            display, EGL_IOSURFACE_ANGLE, slot.ioSurface, config, attributes);
        if (slot.pbuffer == EGL_NO_SURFACE) {
            MarkFallback("IOSurface EGL pbuffer creation failed");
            ReleaseResources();
            return false;
        }
        slot.texture = [device newTextureWithDescriptor:descriptor iosurface:slot.ioSurface plane:0];
        if (!slot.texture) {
            MarkFallback("IOSurface Metal texture creation failed");
            ReleaseResources();
            return false;
        }
    }
    Options options;
    options.scaler = ScalerFromEnvironment();
    options.enableOverlayMask = false;
    gPresenter = std::make_unique<Presenter>((__bridge void *)gOverlayLayer,
                                             gSource.width,
                                             gSource.height,
                                             options);
    if (!gPresenter->isActive()) {
        MarkFallback("SpatialPresenter initialization failed");
        ReleaseResources();
        return false;
    }
    gResourceWidth = gSource.width;
    gResourceHeight = gSource.height;
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

bool CopySourceToIOSurface(SharedSlot &slot,
                           EGLDisplay display,
                           EGLSurface windowSurface,
                           EGLContext context) {
    if (!EnsureProgram()) {
        std::fprintf(stderr, "[Yoghourt] KrKr spatial copy failed: GL program\n");
        return false;
    }
    GLState state;
    CaptureGLState(state);
    if (!EnqueueMetalDoneWait(slot)) {
        std::fprintf(stderr, "[Yoghourt] KrKr spatial copy failed: metalDone wait egl=0x%x\n", eglGetError());
        return false;
    }
    if (!eglMakeCurrent(display, slot.pbuffer, slot.pbuffer, context)) {
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
    const bool signaled = drawError == GL_NO_ERROR && EnqueueAngleReadySignal(slot);
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

SharedSlot *AcquireFreeSlot() {
    for (size_t offset = 0; offset < gSlots.size(); ++offset) {
        const size_t index = (gNextSlot + offset) % gSlots.size();
        auto &slot = gSlots[index];
        uint8_t expected = static_cast<uint8_t>(SlotOwnership::free);
        if (slot.ownership.compare_exchange_strong(
                expected,
                static_cast<uint8_t>(SlotOwnership::angleQueued),
                std::memory_order_acq_rel)) {
            gNextSlot = (index + 1) % gSlots.size();
            uint64_t inFlight = 0;
            for (const auto &candidate : gSlots) {
                if (candidate.ownership.load(std::memory_order_acquire) !=
                    static_cast<uint8_t>(SlotOwnership::free)) {
                    ++inFlight;
                }
            }
            gMaxInFlight = std::max(gMaxInFlight, inFlight);
            return &slot;
        }
    }
    return nullptr;
}

void SpatialCompletion(void *context, bool succeeded) {
    auto *slot = static_cast<SharedSlot *>(context);
    if (!succeeded) gAsyncFailure.store(true, std::memory_order_release);
    gCompletedFrames.fetch_add(1, std::memory_order_relaxed);
    if (gTestHoldCompletions.load(std::memory_order_acquire)) {
        slot->completionHeld.store(true, std::memory_order_release);
        return;
    }
    slot->ownership.store(static_cast<uint8_t>(SlotOwnership::free), std::memory_order_release);
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
    uint64_t inFlight = 0;
    for (const auto &slot : gSlots) {
        if (slot.ownership.load(std::memory_order_acquire) !=
            static_cast<uint8_t>(SlotOwnership::free)) ++inFlight;
    }
    const uint64_t completed = gCompletedFrames.load(std::memory_order_relaxed);
    std::fprintf(stdout,
                 "[Yoghourt] KrKr spatial metrics submitCPU.p50=%.3fms submitCPU.p95=%.3fms submittedFPS=%.1f completedFPS=%.1f droppedScalingFrames=%llu ringSaturation=%llu fallback=%llu inFlight=%llu maxInFlight=%llu\n",
                 percentile(0.50), percentile(0.95),
                 (gSubmittedFrames - gMetricsSubmittedBase) / seconds,
                 (completed - gMetricsCompletedBase) / seconds,
                 (unsigned long long)gDroppedScalingFrames,
                 (unsigned long long)gRingSaturations,
                 (unsigned long long)gFallbacks,
                 (unsigned long long)inFlight,
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

extern "C" bool YoghourtKrKrSpatialPresent(
    EGLDisplay display,
    EGLSurface windowSurface,
    EGLConfig config,
    EGLContext context,
    void *nativeWindow) {
    if (!IsEnabled() || gSource.name == 0 || gSource.width <= 0 || gSource.height <= 0 ||
        display == EGL_NO_DISPLAY || windowSurface == EGL_NO_SURFACE ||
        context == EGL_NO_CONTEXT || !nativeWindow) {
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
    if ((gNextAngleReadyValue == std::numeric_limits<uint64_t>::max() ||
         gNextMetalDoneValue == std::numeric_limits<uint64_t>::max()) &&
        gSlots[0].pbuffer != EGL_NO_SURFACE) {
        // Counter rollover is a generation boundary, never a frame-path wait.
        ReleaseResources();
    }
    if (!CreateSharedResources(display, config, context, nativeWindow)) {
        if (!gGenerationDisabled) MarkFallback("shared resource creation failed");
        return false;
    }
    SharedSlot *slot = AcquireFreeSlot();
    if (!slot) {
        ++gDroppedScalingFrames;
        ++gRingSaturations;
        LogMetricsIfDue();
        return true;
    }
    const auto submitStart = std::chrono::steady_clock::now();
    if (!CopySourceToIOSurface(*slot, display, windowSurface, context)) {
        slot->ownership.store(static_cast<uint8_t>(SlotOwnership::free), std::memory_order_release);
        MarkFallback("ANGLE shared-texture copy or synchronization failed");
        return false;
    }
    if (gNextMetalDoneValue == 0 ||
        gNextMetalDoneValue == std::numeric_limits<uint64_t>::max()) {
        slot->ownership.store(static_cast<uint8_t>(SlotOwnership::free), std::memory_order_release);
        MarkFallback("shared-event counter exhausted; resource generation rebuild required");
        return false;
    }
    slot->metalDoneValue = gNextMetalDoneValue++;
    slot->ownership.store(static_cast<uint8_t>(SlotOwnership::metalQueued),
                          std::memory_order_release);
    MetalTextureFrame frame((__bridge void *)slot->texture,
                            gSource.width,
                            gSource.height,
                            PixelFormat::bgra8Unorm,
                            (__bridge void *)gAngleReadyEvent,
                            slot->angleReadyValue,
                            (__bridge void *)gMetalDoneEvent,
                            slot->metalDoneValue,
                            SpatialCompletion,
                            slot);
    gOverlayLayer.hidden = NO;
    if (!gPresenter->present(frame)) {
        slot->ownership.store(static_cast<uint8_t>(SlotOwnership::free), std::memory_order_release);
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
    gSource = {};
    gLogged = false;
    gGenerationDisabled = false;
    gDisabledDisplay = EGL_NO_DISPLAY;
    gDisabledContext = EGL_NO_CONTEXT;
    gDisabledWidth = 0;
    gDisabledHeight = 0;
    gAsyncFailure.store(false, std::memory_order_release);
    gTestHoldCompletions.store(false, std::memory_order_release);
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
    if (!hold) {
        for (auto &slot : gSlots) {
            if (slot.completionHeld.exchange(false, std::memory_order_acq_rel)) {
                slot.ownership.store(static_cast<uint8_t>(SlotOwnership::free),
                                     std::memory_order_release);
            }
        }
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
