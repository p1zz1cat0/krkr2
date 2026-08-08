#import <AppKit/AppKit.h>
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
    unsigned char pixel[4] = {};
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    const bool pixelMatches =
        std::abs(static_cast<int>(pixel[0]) - 64) <= 2
        && std::abs(static_cast<int>(pixel[1]) - 128) <= 2
        && std::abs(static_cast<int>(pixel[2]) - 191) <= 2
        && pixel[3] >= 253;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &renderTexture);
    glDeleteTextures(1, &texture);
    for (GLuint program : programs) glDeleteProgram(program);
    if (!pixelMatches || glGetError() != GL_NO_ERROR) {
        std::fprintf(stderr,
                     "[Yoghourt] ERROR renderer self-test pbuffer readback pixel=%u,%u,%u,%u\n",
                     pixel[0], pixel[1], pixel[2], pixel[3]);
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
            if (state.context == EGL_NO_CONTEXT || !RunPbufferChecks(state))
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
                         "[Yoghourt] RENDERER backend=angle-metal egl=%d.%d gles=\"%s\" renderer=\"%s\" surface=%dx%d scale=%.2f selfTest=passed\n",
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
