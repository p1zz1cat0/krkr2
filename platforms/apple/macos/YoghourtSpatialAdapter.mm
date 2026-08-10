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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

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
EGLSurface gPbuffer = EGL_NO_SURFACE;
EGLContext gContext = EGL_NO_CONTEXT;
IOSurfaceRef gIOSurface = nullptr;
id<MTLDevice> gDevice = nil;
id<MTLTexture> gMetalTexture = nil;
CAMetalLayer *gOverlayLayer = nil;
std::unique_ptr<Presenter> gPresenter;
GLuint gProgram = 0;
GLint gPosition = -1;
GLint gTexCoord = -1;
int gResourceWidth = 0;
int gResourceHeight = 0;
bool gLogged = false;

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

void ReleaseResources() {
    gPresenter.reset();
    gMetalTexture = nil;
    if (gIOSurface) {
        CFRelease(gIOSurface);
        gIOSurface = nullptr;
    }
    if (gPbuffer != EGL_NO_SURFACE && gDisplay != EGL_NO_DISPLAY) {
        eglDestroySurface(gDisplay, gPbuffer);
    }
    gPbuffer = EGL_NO_SURFACE;
    gDisplay = EGL_NO_DISPLAY;
    gContext = EGL_NO_CONTEXT;
    gDevice = nil;
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

bool CreateSharedResources(EGLDisplay display,
                           EGLConfig config,
                           EGLContext context,
                           void *nativeWindow) {
    if (gSource.width <= 0 || gSource.height <= 0) return false;
    id<MTLDevice> device = QueryMetalDevice(display);
    if (!device || !EnsureOverlay(nativeWindow, device)) return false;

    if (gPbuffer != EGL_NO_SURFACE && gDisplay == display && gContext == context &&
        gResourceWidth == gSource.width && gResourceHeight == gSource.height &&
        gDevice == device) {
        return true;
    }
    ReleaseResources();
    if (!EnsureOverlay(nativeWindow, device)) return false;
    gDisplay = display;
    gContext = context;
    gDevice = device;

    NSDictionary *properties = @{
        (NSString *)kIOSurfaceWidth: @(gSource.width),
        (NSString *)kIOSurfaceHeight: @(gSource.height),
        (NSString *)kIOSurfaceBytesPerElement: @4,
        (NSString *)kIOSurfacePixelFormat: @(static_cast<uint32_t>('BGRA')),
    };
    gIOSurface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
    if (!gIOSurface) return false;

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
    gPbuffer = eglCreatePbufferFromClientBuffer(
        display, EGL_IOSURFACE_ANGLE, gIOSurface, config, attributes);
    if (gPbuffer == EGL_NO_SURFACE) {
        std::fprintf(stderr, "[Yoghourt] KrKr spatial fallback: IOSurface pbuffer creation failed egl=0x%x\n", eglGetError());
        ReleaseResources();
        return false;
    }

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
    gMetalTexture = [device newTextureWithDescriptor:descriptor iosurface:gIOSurface plane:0];
    if (!gMetalTexture) {
        ReleaseResources();
        return false;
    }
    Options options;
    options.scaler = ScalerFromEnvironment();
    options.enableOverlayMask = false;
    gPresenter = std::make_unique<Presenter>((__bridge void *)gOverlayLayer,
                                             gSource.width,
                                             gSource.height,
                                             options);
    if (!gPresenter->isActive()) {
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
        glVertexAttribPointer(attribute.index,
                              attribute.size,
                              attribute.type,
                              attribute.normalized,
                              attribute.stride,
                              attribute.pointer);
        attribute.enabled ? glEnableVertexAttribArray(attribute.index)
                          : glDisableVertexAttribArray(attribute.index);
    }
    glBindBuffer(GL_ARRAY_BUFFER, state.arrayBuffer);
}

bool CopySourceToIOSurface(EGLDisplay display,
                           EGLSurface windowSurface,
                           EGLContext context) {
    if (!EnsureProgram()) return false;
    GLState state;
    CaptureGLState(state);
    if (!eglMakeCurrent(display, gPbuffer, gPbuffer, context)) return false;

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
    glVertexAttribPointer(gPosition, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), vertices);
    glVertexAttribPointer(gTexCoord, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), vertices + 2);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();
    glDisableVertexAttribArray(gPosition);
    glDisableVertexAttribArray(gTexCoord);
    const bool drew = glGetError() == GL_NO_ERROR;
    const bool restored = eglMakeCurrent(display, windowSurface, windowSurface, context) == EGL_TRUE;
    RestoreGLState(state);
    return drew && restored;
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
    if (!CreateSharedResources(display, config, context, nativeWindow) ||
        !CopySourceToIOSurface(display, windowSurface, context)) {
        HideOverlay();
        return false;
    }
    MetalTextureFrame frame((__bridge void *)gMetalTexture,
                            gSource.width,
                            gSource.height,
                            PixelFormat::bgra8Unorm);
    gOverlayLayer.hidden = NO;
    if (!gPresenter->present(frame)) {
        HideOverlay();
        return false;
    }
    if (!gLogged) {
        std::fprintf(stdout,
                     "[Yoghourt] SPATIAL engine=kirikiri bridge=iosurface-metal source=%dx%d readback=none sync=glFinish scaler=%s\n",
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
}
