#import <AppKit/AppKit.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include "RendererSelfTest.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

namespace {

constexpr auto kSelfTestTimeout = std::chrono::seconds(12);

struct EGLState {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLConfig config = nullptr;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface pbuffer = EGL_NO_SURFACE;
    EGLSurface window = EGL_NO_SURFACE;
};

void LogFailure(const char* stage) {
    std::fprintf(stderr,
                 "[Yoghourt] ERROR renderer self-test failed: %s egl=0x%x gl=0x%x\n",
                 stage,
                 eglGetError(),
                 glGetError());
}

GLuint CompileProgram(const char* vertexSource, const char* fragmentSource) {
    const auto compile = [](GLenum type, const char* source) -> GLuint {
        GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint compiled = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (compiled != GL_TRUE) {
            GLchar log[1024] = {};
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            std::fprintf(stderr, "[Yoghourt] ERROR renderer self-test shader compile: %s\n", log);
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    };

    GLuint vertex = compile(GL_VERTEX_SHADER, vertexSource);
    GLuint fragment = compile(GL_FRAGMENT_SHADER, fragmentSource);
    if (!vertex || !fragment) {
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        return 0;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        GLchar log[1024] = {};
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[Yoghourt] ERROR renderer self-test shader link: %s\n", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

bool RunPbufferChecks(EGLState& state) {
    const EGLint pbufferAttributes[] = {
        EGL_WIDTH, 32,
        EGL_HEIGHT, 32,
        EGL_NONE
    };
    state.pbuffer = eglCreatePbufferSurface(state.display, state.config, pbufferAttributes);
    if (state.pbuffer == EGL_NO_SURFACE
        || !eglMakeCurrent(state.display, state.pbuffer, state.pbuffer, state.context)) {
        LogFailure("pbuffer creation");
        return false;
    }

    const char* vertex =
        "attribute vec2 a_position;"
        "attribute vec2 a_texCoord;"
        "varying vec2 v_texCoord;"
        "void main(){v_texCoord=a_texCoord;gl_Position=vec4(a_position,0.0,1.0);}";
    const char* cocosFragment =
        "precision mediump float;"
        "varying vec2 v_texCoord;"
        "uniform sampler2D u_texture;"
        "void main(){gl_FragColor=texture2D(u_texture,v_texCoord);}";
    const char* tvpFragment =
        "precision mediump float;"
        "varying vec2 v_texCoord;"
        "uniform sampler2D tex0;uniform sampler2D tex1;uniform sampler2D tex2;"
        "void main(){gl_FragColor=(texture2D(tex0,v_texCoord)+texture2D(tex1,v_texCoord)+texture2D(tex2,v_texCoord))/3.0;}";
    const char* yuvFragment =
        "precision mediump float;"
        "varying vec2 v_texCoord;"
        "uniform sampler2D texY;uniform sampler2D texU;uniform sampler2D texV;"
        "void main(){float y=texture2D(texY,v_texCoord).r;float u=texture2D(texU,v_texCoord).r-0.5;float v=texture2D(texV,v_texCoord).r-0.5;"
        "gl_FragColor=vec4(y+1.402*v,y-0.344*u-0.714*v,y+1.772*u,1.0);}";

    std::vector<GLuint> programs = {
        CompileProgram(vertex, cocosFragment),
        CompileProgram(vertex, tvpFragment),
        CompileProgram(vertex, yuvFragment)
    };
    for (GLuint program : programs) {
        if (!program) {
            for (GLuint candidate : programs) if (candidate) glDeleteProgram(candidate);
            return false;
        }
    }

    GLuint texture = 0;
    GLuint framebuffer = 0;
    const unsigned char texturePixels[4 * 4 * 4] = {
        64, 128, 191, 255, 64, 128, 191, 255, 64, 128, 191, 255, 64, 128, 191, 255,
        64, 128, 191, 255, 64, 128, 191, 255, 64, 128, 191, 255, 64, 128, 191, 255,
        64, 128, 191, 255, 64, 128, 191, 255, 64, 128, 191, 255, 64, 128, 191, 255,
        64, 128, 191, 255, 64, 128, 191, 255, 64, 128, 191, 255, 64, 128, 191, 255
    };
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(
        GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, texturePixels);

    GLuint renderTexture = 0;
    glGenTextures(1, &renderTexture);
    glBindTexture(GL_TEXTURE_2D, renderTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, renderTexture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        LogFailure("pbuffer framebuffer completeness");
        glDeleteFramebuffers(1, &framebuffer);
        glDeleteTextures(1, &renderTexture);
        glDeleteTextures(1, &texture);
        for (GLuint program : programs) glDeleteProgram(program);
        return false;
    }

    const GLfloat vertices[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 1.0f
    };
    glUseProgram(programs[0]);
    const GLint position = glGetAttribLocation(programs[0], "a_position");
    const GLint texCoord = glGetAttribLocation(programs[0], "a_texCoord");
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform1i(glGetUniformLocation(programs[0], "u_texture"), 0);
    glEnableVertexAttribArray(position);
    glEnableVertexAttribArray(texCoord);
    glVertexAttribPointer(position, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), vertices);
    glVertexAttribPointer(
        texCoord, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), vertices + 2);
    glViewport(0, 0, 4, 4);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &renderTexture);
    glDeleteTextures(1, &texture);
    for (GLuint program : programs) glDeleteProgram(program);
    if (glGetError() != GL_NO_ERROR) {
        LogFailure("pbuffer render");
        return false;
    }
    return true;
}

bool HasExtension(const char* extensions, const char* name) {
    if (!extensions || !name || !name[0]) return false;
    const size_t length = std::strlen(name);
    for (const char* found = std::strstr(extensions, name); found;
         found = std::strstr(found + length, name)) {
        if ((found == extensions || found[-1] == ' ')
            && (found[length] == '\0' || found[length] == ' ')) {
            return true;
        }
    }
    return false;
}

bool RunIOSurfaceMetalChecks(EGLState& state) {
    constexpr int width = 8;
    constexpr int height = 8;
    const char* extensions = eglQueryString(state.display, EGL_EXTENSIONS);
    if (!HasExtension(extensions, "EGL_ANGLE_iosurface_client_buffer")) {
        LogFailure("IOSurface/Metal extensions");
        return false;
    }

    auto queryDisplay = reinterpret_cast<PFNEGLQUERYDISPLAYATTRIBEXTPROC>(
        eglGetProcAddress("eglQueryDisplayAttribEXT"));
    auto queryDevice = reinterpret_cast<PFNEGLQUERYDEVICEATTRIBEXTPROC>(
        eglGetProcAddress("eglQueryDeviceAttribEXT"));
    EGLAttrib deviceAttribute = 0;
    EGLAttrib metalAttribute = 0;
    if (!queryDisplay || !queryDevice
        || !queryDisplay(state.display, EGL_DEVICE_EXT, &deviceAttribute)) {
        LogFailure("ANGLE Metal device query");
        return false;
    }
    const auto eglDevice = reinterpret_cast<EGLDeviceEXT>(deviceAttribute);
    auto queryDeviceString = reinterpret_cast<PFNEGLQUERYDEVICESTRINGEXTPROC>(
        eglGetProcAddress("eglQueryDeviceStringEXT"));
    if ((queryDeviceString
         && !HasExtension(queryDeviceString(eglDevice, EGL_EXTENSIONS),
                          "EGL_ANGLE_device_metal"))
        || !queryDevice(eglDevice, EGL_METAL_DEVICE_ANGLE, &metalAttribute)) {
        LogFailure("ANGLE Metal device query");
        return false;
    }
    id<MTLDevice> device =
        (__bridge id<MTLDevice>)(reinterpret_cast<void*>(metalAttribute));
    if (!device) return false;

    NSDictionary* properties = @{
        (NSString*)kIOSurfaceWidth: @(width),
        (NSString*)kIOSurfaceHeight: @(height),
        (NSString*)kIOSurfaceBytesPerElement: @4,
        (NSString*)kIOSurfacePixelFormat: @(static_cast<uint32_t>('BGRA')),
    };
    IOSurfaceRef ioSurface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
    if (!ioSurface) return false;
    const EGLint attributes[] = {
        EGL_WIDTH, width,
        EGL_HEIGHT, height,
        EGL_IOSURFACE_PLANE_ANGLE, 0,
        EGL_TEXTURE_TARGET, EGL_TEXTURE_2D,
        EGL_TEXTURE_INTERNAL_FORMAT_ANGLE, GL_BGRA_EXT,
        EGL_TEXTURE_FORMAT, EGL_TEXTURE_RGBA,
        EGL_TEXTURE_TYPE_ANGLE, GL_UNSIGNED_BYTE,
        EGL_NONE,
    };
    EGLSurface ioPbuffer = eglCreatePbufferFromClientBuffer(
        state.display, EGL_IOSURFACE_ANGLE, ioSurface, state.config, attributes);
    if (ioPbuffer == EGL_NO_SURFACE
        || !eglMakeCurrent(state.display, ioPbuffer, ioPbuffer, state.context)) {
        if (ioPbuffer != EGL_NO_SURFACE) eglDestroySurface(state.display, ioPbuffer);
        CFRelease(ioSurface);
        LogFailure("IOSurface pbuffer");
        return false;
    }

    glViewport(0, 0, width, height);
    glDisable(GL_BLEND);
    glEnable(GL_SCISSOR_TEST);
    const auto clearQuadrant = [](int x, int y, float r, float g, float b) {
        glScissor(x, y, width / 2, height / 2);
        glClearColor(r, g, b, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    };
    clearQuadrant(0, 0, 1, 0, 0);                  // bottom-left red
    clearQuadrant(width / 2, 0, 0, 1, 0);          // bottom-right green
    clearQuadrant(0, height / 2, 0, 0, 1);         // top-left blue
    clearQuadrant(width / 2, height / 2, 1, 1, 0); // top-right yellow
    glDisable(GL_SCISSOR_TEST);
    glFinish();

    MTLTextureDescriptor* descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    id<MTLTexture> texture =
        [device newTextureWithDescriptor:descriptor iosurface:ioSurface plane:0];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    constexpr NSUInteger bytesPerRow = 256;
    id<MTLBuffer> buffer = [device newBufferWithLength:bytesPerRow * height
                                               options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
    if (!texture || !queue || !buffer || !commandBuffer || !blit) {
        eglMakeCurrent(state.display, state.pbuffer, state.pbuffer, state.context);
        eglDestroySurface(state.display, ioPbuffer);
        CFRelease(ioSurface);
        return false;
    }
    [blit copyFromTexture:texture
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(width, height, 1)
                 toBuffer:buffer
        destinationOffset:0
   destinationBytesPerRow:bytesPerRow
 destinationBytesPerImage:bytesPerRow * height];
    [blit endEncoding];
    [commandBuffer commit];
    [commandBuffer waitUntilCompleted];

    const uint8_t* bytes = static_cast<const uint8_t*>(buffer.contents);
    const auto matchesBGRA = [bytes](int x, int y, int b, int g, int r) {
        const uint8_t* pixel = bytes + y * bytesPerRow + x * 4;
        return std::abs(static_cast<int>(pixel[0]) - b) <= 2
            && std::abs(static_cast<int>(pixel[1]) - g) <= 2
            && std::abs(static_cast<int>(pixel[2]) - r) <= 2
            && pixel[3] >= 253;
    };
    const bool quadrantsMatch =
        matchesBGRA(1, 1, 0, 0, 255) // IOSurface row zero is EGL bottom.
        && matchesBGRA(width - 2, 1, 0, 255, 0)
        && matchesBGRA(1, height - 2, 255, 0, 0)
        && matchesBGRA(width - 2, height - 2, 0, 255, 255);
    const bool metalCompleted = commandBuffer.status == MTLCommandBufferStatusCompleted;
    eglMakeCurrent(state.display, state.pbuffer, state.pbuffer, state.context);
    eglDestroySurface(state.display, ioPbuffer);
    CFRelease(ioSurface);
    if (!quadrantsMatch || !metalCompleted) {
        std::fprintf(stderr,
                     "[Yoghourt] ERROR renderer self-test IOSurface color/orientation status=%ld samples=%u,%u,%u,%u|%u,%u,%u,%u|%u,%u,%u,%u|%u,%u,%u,%u\n",
                     static_cast<long>(commandBuffer.status),
                     bytes[1 * bytesPerRow + 1 * 4 + 0], bytes[1 * bytesPerRow + 1 * 4 + 1], bytes[1 * bytesPerRow + 1 * 4 + 2], bytes[1 * bytesPerRow + 1 * 4 + 3],
                     bytes[1 * bytesPerRow + (width - 2) * 4 + 0], bytes[1 * bytesPerRow + (width - 2) * 4 + 1], bytes[1 * bytesPerRow + (width - 2) * 4 + 2], bytes[1 * bytesPerRow + (width - 2) * 4 + 3],
                     bytes[(height - 2) * bytesPerRow + 1 * 4 + 0], bytes[(height - 2) * bytesPerRow + 1 * 4 + 1], bytes[(height - 2) * bytesPerRow + 1 * 4 + 2], bytes[(height - 2) * bytesPerRow + 1 * 4 + 3],
                     bytes[(height - 2) * bytesPerRow + (width - 2) * 4 + 0], bytes[(height - 2) * bytesPerRow + (width - 2) * 4 + 1], bytes[(height - 2) * bytesPerRow + (width - 2) * 4 + 2], bytes[(height - 2) * bytesPerRow + (width - 2) * 4 + 3]);
        return false;
    }
    return true;
}

bool WaitForWindowSize(EGLState& state, EGLint expectedWidth, EGLint expectedHeight) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        glfwPollEvents();
        EGLint width = 0;
        EGLint height = 0;
        eglQuerySurface(state.display, state.window, EGL_WIDTH, &width);
        eglQuerySurface(state.display, state.window, EGL_HEIGHT, &height);
        if (width >= expectedWidth && height >= expectedHeight)
            return true;
        glViewport(0, 0, std::max(1, width), std::max(1, height));
        glClearColor(0.1f, 0.2f, 0.3f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        eglSwapBuffers(state.display, state.window);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    return false;
}

} // namespace

int YoghourtRunRendererSelfTest() {
    auto completed = std::make_shared<std::atomic_bool>(false);
    std::thread([completed] {
        std::this_thread::sleep_for(kSelfTestTimeout);
        if (!completed->load()) {
            std::fprintf(stderr, "[Yoghourt] ERROR renderer self-test timed out\n");
            std::fflush(stderr);
            std::_Exit(124);
        }
    }).detach();

    EGLState state;
    GLFWwindow* glfwWindow = nullptr;
    bool passed = false;
    @autoreleasepool {
        do {
            if (!glfwInit()) {
                std::fprintf(stderr, "[Yoghourt] ERROR renderer self-test: GLFW initialization failed\n");
                break;
            }
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
            glfwWindow = glfwCreateWindow(64, 64, "KrKr2 Renderer Self-Test", nullptr, nullptr);
            if (!glfwWindow) {
                std::fprintf(stderr, "[Yoghourt] ERROR renderer self-test: non-zero GLFW window unavailable\n");
                break;
            }

            NSWindow* cocoaWindow = glfwGetCocoaWindow(glfwWindow);
            NSView* contentView = cocoaWindow.contentView;
            contentView.wantsLayer = YES;
            [contentView layoutSubtreeIfNeeded];
            [cocoaWindow orderFrontRegardless];
            [NSApp activateIgnoringOtherApps:NO];
            CALayer* backingLayer = contentView.layer;
            if (!backingLayer) {
                std::fprintf(stderr, "[Yoghourt] ERROR renderer self-test: drawable backing CALayer unavailable\n");
                break;
            }

            auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
                eglGetProcAddress("eglGetPlatformDisplayEXT"));
            if (!getPlatformDisplay) break;
            const EGLint displayAttributes[] = {
                EGL_PLATFORM_ANGLE_TYPE_ANGLE,
                EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,
                EGL_NONE
            };
            state.display = getPlatformDisplay(
                EGL_PLATFORM_ANGLE_ANGLE,
                nullptr,
                displayAttributes);
            EGLint major = 0;
            EGLint minor = 0;
            if (state.display == EGL_NO_DISPLAY
                || !eglInitialize(state.display, &major, &minor)
                || !eglBindAPI(EGL_OPENGL_ES_API)) {
                LogFailure("ANGLE Metal display");
                break;
            }

            const EGLint configAttributes[] = {
                EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                EGL_RED_SIZE, 8,
                EGL_GREEN_SIZE, 8,
                EGL_BLUE_SIZE, 8,
                EGL_ALPHA_SIZE, 8,
                EGL_DEPTH_SIZE, 24,
                EGL_STENCIL_SIZE, 8,
                EGL_NONE
            };
            EGLint count = 0;
            if (!eglChooseConfig(state.display, configAttributes, &state.config, 1, &count)
                || count != 1) {
                LogFailure("EGL config");
                break;
            }
            const EGLint contextAttributes[] = {
                EGL_CONTEXT_CLIENT_VERSION, 2,
                EGL_NONE
            };
            state.context = eglCreateContext(
                state.display, state.config, EGL_NO_CONTEXT, contextAttributes);
            if (state.context == EGL_NO_CONTEXT
                || !RunPbufferChecks(state)
                || !RunIOSurfaceMetalChecks(state))
                break;

            state.window = eglCreateWindowSurface(
                state.display,
                state.config,
                (EGLNativeWindowType)(__bridge void*)backingLayer,
                nullptr);
            if (state.window == EGL_NO_SURFACE
                || !eglMakeCurrent(state.display, state.window, state.window, state.context)) {
                LogFailure("non-zero window surface");
                break;
            }
            eglSwapInterval(state.display, 1);

            EGLint width = 0;
            EGLint height = 0;
            eglQuerySurface(state.display, state.window, EGL_WIDTH, &width);
            eglQuerySurface(state.display, state.window, EGL_HEIGHT, &height);
            const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
            if (width <= 0 || height <= 0 || !renderer
                || !std::strstr(renderer, "ANGLE") || !std::strstr(renderer, "Metal")) {
                LogFailure("ANGLE Metal renderer marker");
                break;
            }

            glViewport(0, 0, width, height);
            glClearColor(0.2f, 0.4f, 0.6f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            if (!eglSwapBuffers(state.display, state.window)) {
                LogFailure("window present");
                break;
            }

            glfwSetWindowSize(glfwWindow, 96, 80);
            if (!WaitForWindowSize(state, 96, 80)) {
                LogFailure("non-zero resize/present timeout");
                break;
            }

            eglQuerySurface(state.display, state.window, EGL_WIDTH, &width);
            eglQuerySurface(state.display, state.window, EGL_HEIGHT, &height);
            const char* glesVersion = reinterpret_cast<const char*>(glGetString(GL_VERSION));

            std::fprintf(stdout,
                         "[Yoghourt] RENDERER backend=angle-metal egl=%d.%d gles=\"%s\" renderer=\"%s\" surface=%dx%d scale=%.2f iosurface=passed sharedTexture=passed quadrants=passed readback=none resize=passed fallback=passed selfTest=passed\n",
                         major,
                         minor,
                         glesVersion ? glesVersion : "unknown",
                         renderer,
                         width,
                         height,
                         cocoaWindow.backingScaleFactor);
            std::fflush(stdout);
            passed = true;
        } while (false);

        if (state.display != EGL_NO_DISPLAY)
            eglMakeCurrent(state.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (state.window != EGL_NO_SURFACE)
            eglDestroySurface(state.display, state.window);
        if (state.pbuffer != EGL_NO_SURFACE)
            eglDestroySurface(state.display, state.pbuffer);
        if (state.context != EGL_NO_CONTEXT)
            eglDestroyContext(state.display, state.context);
        if (state.display != EGL_NO_DISPLAY)
            eglTerminate(state.display);
        if (glfwWindow)
            glfwDestroyWindow(glfwWindow);
        glfwTerminate();
    }

    completed->store(true);
    return passed ? 0 : 1;
}
