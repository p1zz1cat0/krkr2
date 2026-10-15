// 光标宿主的组件测试：把解码出来的游戏光标装到真实 GLFW 窗口上，再读回
// AppKit 的当前光标，确认映射、像素和动画都对。
//
// 光标只有落在窗口内容区里 GLFW 才会立刻套用，所以这里把小窗口放到鼠标
// 当前位置下面；跑测试时不要移动鼠标。
#include "YoghourtCursor.h"

#include "CursorFile.h"

#include <GLFW/glfw3.h>

#import <AppKit/AppKit.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

static int gFailures = 0;

// 链接桩：krkr2core 与打过补丁的 cocos2d-x 都引用 Spatial Presenter，而本测试
// 不链接主程序那份实现。签名与 tests/unit-tests/core/tjs2/spatial_stubs.cpp 一致。
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

static void Expect(bool condition, const char *what) {
    if(!condition) {
        std::printf("FAIL: %s\n", what);
        ++gFailures;
    } else {
        std::printf("ok: %s\n", what);
    }
}

static std::vector<unsigned char> ReadFile(const std::string &path) {
    std::vector<unsigned char> bytes;
    FILE *handle = std::fopen(path.c_str(), "rb");
    if(!handle)
        return bytes;
    unsigned char buffer[4096];
    size_t read = 0;
    while((read = std::fread(buffer, 1, sizeof(buffer), handle)) > 0)
        bytes.insert(bytes.end(), buffer, buffer + read);
    std::fclose(handle);
    return bytes;
}

//! 载入光标文件的路径和脚本层 TVPGetCursor() 完全一致：读文件、解码、占句柄。
static int LoadCursor(const std::string &path) {
    std::vector<unsigned char> bytes = ReadFile(path);
    if(bytes.empty())
        return 0;
    tTVPCursorResource resource;
    if(!TVPDecodeCursorFile(bytes.data(), bytes.size(), resource))
        return 0;
    return (int)TVPCursorRegister(std::move(resource));
}

//! 光标位图的原始 RGBA 像素。AppKit 的颜色空间转换会挪动饱和色（校准 RGB →
//! sRGB 时洋红会带出绿色分量），像素校验因此直接看位图字节。
struct CursorPixel {
    unsigned char R = 0, G = 0, B = 0, A = 0;
};

static bool PixelAt(NSCursor *cursor, int x, int y, CursorPixel &out) {
    if(!cursor.image)
        return false;
    NSBitmapImageRep *rep =
        [NSBitmapImageRep imageRepWithData:[cursor.image TIFFRepresentation]];
    if(!rep)
        return false;
    const unsigned char *data = [rep bitmapData];
    if(!data)
        return false;
    const size_t stride = (size_t)[rep bytesPerRow];
    const unsigned char *src = data + (size_t)y * stride + (size_t)x * 4;
    out = { src[0], src[1], src[2], src[3] };
    return true;
}

static void ExpectColor(NSCursor *cursor, unsigned char r, unsigned char g,
                        unsigned char b, const char *what) {
    CursorPixel pixel;
    if(!PixelAt(cursor, 0, 0, pixel)) {
        std::printf("FAIL: %s (no bitmap)\n", what);
        ++gFailures;
        return;
    }
    if(pixel.R != r || pixel.G != g || pixel.B != b || pixel.A != 0xff) {
        std::printf("FAIL: %s (got %u %u %u alpha %u)\n", what, pixel.R,
                    pixel.G, pixel.B, pixel.A);
        ++gFailures;
        return;
    }
    std::printf("ok: %s\n", what);
}

static bool IsTransparent(NSCursor *cursor) {
    CursorPixel pixel;
    if(!PixelAt(cursor, 0, 0, pixel))
        return false;
    return pixel.A == 0 && [cursor.image size].width <= 1.0 &&
        [cursor.image size].height <= 1.0;
}

int main(int argc, char **argv) {
    const std::string fixture =
        argc > 1 ? argv[1] : std::string("tests/test_files/cursor");
    const int magenta = LoadCursor(fixture + "/pointer.cur");
    const int animation = LoadCursor(fixture + "/pointer.ani");
    Expect(magenta >= TVPCursorHandleBase,
           "pointer.cur decodes into a loaded-cursor handle");
    Expect(animation >= TVPCursorHandleBase,
           "pointer.ani decodes into a loaded-cursor handle");
    if(magenta < TVPCursorHandleBase || animation < TVPCursorHandleBase)
        return 1;

    // 小窗口盖住鼠标当前位置，光标才算落在内容区里。
    const NSPoint mouse = [NSEvent mouseLocation]; // 原点在左下
    const CGFloat screenHeight = [[NSScreen mainScreen] frame].size.height;
    const int side = 160;
    Expect(glfwInit() == GLFW_TRUE, "GLFW initialised");
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    GLFWwindow *window =
        glfwCreateWindow(side, side, "cursor-host", nullptr, nullptr);
    Expect(window != nullptr, "GLFW window created");
    if(!window) {
        glfwTerminate();
        return 1;
    }
    glfwSetWindowPos(window, (int)mouse.x - side / 2,
                     (int)(screenHeight - mouse.y) - side / 2);
    [NSApp activateIgnoringOtherApps:YES];
    glfwPollEvents();
    YoghourtCursorAttachWindow(window);

    // 系统光标常量映射到 AppKit 的对应光标。
    YoghourtCursorApply(crIBeam);
    Expect([NSCursor currentCursor] == [NSCursor IBeamCursor],
           "crIBeam maps to the I-beam cursor");
    YoghourtCursorApply(crHandPoint);
    Expect([NSCursor currentCursor] == [NSCursor pointingHandCursor],
           "crHandPoint maps to the pointing hand");
    YoghourtCursorApply(crNo);
    Expect([NSCursor currentCursor] == [NSCursor operationNotAllowedCursor],
           "crNo maps to the operation-not-allowed cursor");
    YoghourtCursorApply(crDefault);
    Expect([NSCursor currentCursor] == [NSCursor arrowCursor],
           "crDefault maps back to the arrow");

    // 游戏光标：尺寸、热点、像素颜色都要对，通道顺序错了这里就会红。
    YoghourtCursorApply(magenta);
    NSCursor *cursor = [NSCursor currentCursor];
    Expect(cursor != [NSCursor arrowCursor],
           "loaded cursor replaces the window cursor");
    Expect(std::abs([cursor.image size].width - 32.0) <= 0.01 &&
               std::abs([cursor.image size].height - 32.0) <= 0.01,
           "loaded cursor keeps the 32x32 image size");
    Expect(std::abs([cursor hotSpot].x - 4.0) <= 0.01 &&
               std::abs([cursor hotSpot].y - 4.0) <= 0.01,
           "loaded cursor keeps the file hot spot");
    ExpectColor(cursor, 255, 0, 255,
                "loaded cursor pixels are magenta (channel order)");

    // 隐藏状态用全透明光标，脚本再次要求可见时回到原来的光标。
    YoghourtCursorSetHidden(1);
    Expect(IsTransparent([NSCursor currentCursor]),
           "mcsHidden shows a transparent cursor");
    YoghourtCursorSetHidden(0);
    ExpectColor([NSCursor currentCursor], 255, 0, 255,
                "mcsVisible restores the loaded cursor");

    // 动画光标：按 rate 从洋红切到青色（每帧 200 ms）。
    YoghourtCursorApply(animation);
    ExpectColor([NSCursor currentCursor], 255, 0, 255,
                "animated cursor starts on its first frame");
    YoghourtCursorTick();
    usleep(250 * 1000);
    YoghourtCursorTick();
    ExpectColor([NSCursor currentCursor], 0, 255, 255,
                "animated cursor advances to the next frame");

    glfwDestroyWindow(window);
    glfwTerminate();

    if(gFailures) {
        std::printf("%d cursor host check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("CURSOR_HOST_COMPONENT_PASS\n");
    return 0;
}
