//
//  macOS: the game window's native mouse cursor.
//
//  Cursors are installed through GLFW: the game window is a GLFW window and
//  GLFW overwrites the cursor from its own state on every AppKit cursor
//  update/enter, so a plain [NSCursor set] or cursor rect would be reverted.
//  Game cursors arrive decoded as RGBA frames (CursorFile.h) and become GLFW
//  cursors here; animated ones are advanced from the window's per-frame tick.
//
#include "YoghourtCursor.h"

#import <AppKit/AppKit.h>
#include <GLFW/glfw3.h>
#include <cocos2d.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <new>
#include <set>
#include <string>
#include <vector>

#include "CursorFile.h"

//---------------------------------------------------------------------------
// Game window
//---------------------------------------------------------------------------
//! cocos2d-x keeps the GLFW window handle protected, so the runtime builds the
//! game view itself instead of calling GLViewImpl::create*().
class YoghourtGLView : public cocos2d::GLViewImpl {
public:
    static YoghourtGLView *createWithRect(const std::string &viewName,
                                          const cocos2d::Rect &rect) {
        auto *view = new (std::nothrow) YoghourtGLView();
        // Same arguments as GLViewImpl::createWithRect() keeps; the window is
        // made resizable later by the window presentation layer.
        if(view && view->initWithRect(viewName, rect, 1.0f, false)) {
            view->autorelease();
            return view;
        }
        CC_SAFE_DELETE(view);
        return nullptr;
    }

    GLFWwindow *getGLFWWindow() const { return _mainWindow; }
};

//---------------------------------------------------------------------------
// Cursor state
//---------------------------------------------------------------------------
namespace {

GLFWwindow *gWindow = nullptr;

//! Cursor the layer under the mouse asked for, see CursorFile.h.
int gHandle = crDefault;
//! mcsHidden: keep the pointer invisible over the game window.
bool gHidden = false;

//! Native cursors of one decoded cursor resource.
struct tAnimatedCursor {
    std::vector<GLFWcursor *> Cursors;
    std::vector<int> DurationsMS;
};

std::map<int, tAnimatedCursor> gLoadedCursors;
std::map<int, GLFWcursor *> gStandardCursors;
//! Loaded-cursor handles that could not become native cursors.
std::set<int> gFailedCursors;

int gFrameIndex = 0;
int gFrameElapsedMS = 0;
long long gLastTick = 0;
bool gTicking = false;

//! 单调时钟毫秒数，动画步进只关心差值。
long long NowMilliseconds() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
        .count();
}

//! Cursor shown when the game hides the pointer.  NSCursor's hide/unhide would
//! also hide the pointer outside the game window and needs balanced calls; a
//! fully transparent cursor only covers our window and survives app switches.
GLFWcursor *HiddenCursor() {
    static GLFWcursor *cursor = nullptr;
    if(!cursor) {
        unsigned char pixel[4] = { 0, 0, 0, 0 };
        GLFWimage image = { 1, 1, pixel };
        cursor = glfwCreateCursor(&image, 0, 0);
    }
    return cursor;
}

//! Maps a cr* id onto a system cursor.  Returns nullptr for "plain arrow",
//! which is also the fallback for shapes macOS has no counterpart for.
GLFWcursor *StandardCursor(int handle) {
    int shape;
    switch(handle) {
        case crIBeam:
        case crHBeam:
            shape = GLFW_IBEAM_CURSOR;
            break;
        case crCross:
            shape = GLFW_CROSSHAIR_CURSOR;
            break;
        case crHandPoint:
            shape = GLFW_POINTING_HAND_CURSOR;
            break;
        case crSize:
        case crSizeAll:
        case crDrag:
        case crMultiDrag:
            shape = GLFW_RESIZE_ALL_CURSOR;
            break;
        case crSizeNESW:
            shape = GLFW_RESIZE_NESW_CURSOR;
            break;
        case crSizeNS:
        case crVSplit:
            shape = GLFW_RESIZE_NS_CURSOR;
            break;
        case crSizeNWSE:
            shape = GLFW_RESIZE_NWSE_CURSOR;
            break;
        case crSizeWE:
        case crHSplit:
            shape = GLFW_RESIZE_EW_CURSOR;
            break;
        case crNoDrop:
        case crNo:
            shape = GLFW_NOT_ALLOWED_CURSOR;
            break;
        default:
            // crDefault, crArrow, crUpArrow, crHourGlass, crSQLWait,
            // crAppStart and crHelp have no macOS counterpart.
            return nullptr;
    }

    auto it = gStandardCursors.find(shape);
    if(it != gStandardCursors.end())
        return it->second;
    GLFWcursor *cursor = glfwCreateStandardCursor(shape);
    if(cursor)
        gStandardCursors.emplace(shape, cursor);
    return cursor;
}

//! Builds the native cursors of a handle handed out by TVPGetCursor(), once
//! per handle.
const tAnimatedCursor *LoadedCursor(int handle) {
    auto it = gLoadedCursors.find(handle);
    if(it != gLoadedCursors.end())
        return &it->second;
    // A handle that failed once fails again; do not rebuild on every mouse
    // enter or animation tick.
    if(gFailedCursors.count(handle))
        return nullptr;

    const tTVPCursorResource *resource = TVPCursorFind(handle);
    if(!resource || resource->Steps.empty()) {
        gFailedCursors.insert(handle);
        return nullptr;
    }

    tAnimatedCursor animated;
    for(const tTVPCursorStep &step : resource->Steps) {
        GLFWimage image;
        image.width = step.Image.Width;
        image.height = step.Image.Height;
        image.pixels = const_cast<unsigned char *>(step.Image.Pixels.data());
        GLFWcursor *cursor =
            glfwCreateCursor(&image, resource->HotSpotX, resource->HotSpotY);
        if(!cursor)
            break;
        animated.Cursors.push_back(cursor);
        animated.DurationsMS.push_back(std::max(step.DurationMS, 1));
    }
    if(animated.Cursors.size() != resource->Steps.size()) {
        for(GLFWcursor *cursor : animated.Cursors)
            glfwDestroyCursor(cursor);
        gFailedCursors.insert(handle);
        return nullptr;
    }

    return &gLoadedCursors.emplace(handle, std::move(animated)).first->second;
}

//! Pushes the current layer cursor and visibility state to the window.
void ApplyCursor() {
    gFrameIndex = 0;
    gFrameElapsedMS = 0;
    gTicking = false;

    if(!gWindow)
        return;

    spdlog::debug("yoghourt-cursor: apply handle={} hidden={}", gHandle,
                  gHidden ? 1 : 0);

    if(gHidden || gHandle == crNone) {
        glfwSetCursor(gWindow, HiddenCursor());
        return;
    }

    if(gHandle >= TVPCursorHandleBase) {
        const tAnimatedCursor *animated = LoadedCursor(gHandle);
        if(animated) {
            glfwSetCursor(gWindow, animated->Cursors.front());
            return;
        }
        // Unknown handle: fall through to the arrow rather than to nothing.
    }

    glfwSetCursor(gWindow, StandardCursor(gHandle));
}

void AttachWindow(GLFWwindow *window) {
    gWindow = window;
    ApplyCursor();
}

} // namespace

//---------------------------------------------------------------------------
// Entry points
//---------------------------------------------------------------------------
extern "C" void *YoghourtCreateRuntimeGLView(const char *title, int width,
                                             int height) {
    const std::string viewName = title && title[0] ? title : "krkr2";
    YoghourtGLView *view = YoghourtGLView::createWithRect(
        viewName, cocos2d::Rect(0, 0, (float)width, (float)height));
    if(view)
        AttachWindow(view->getGLFWWindow());
    return view;
}

extern "C" void YoghourtCursorAttachWindow(void *glfwWindow) {
    AttachWindow((GLFWwindow *)glfwWindow);
}

extern "C" void YoghourtCursorApply(int handle) {
    gHandle = handle;
    ApplyCursor();
}

extern "C" void YoghourtCursorSetHidden(int hidden) {
    const bool value = hidden != 0;
    if(gHidden == value)
        return;
    gHidden = value;
    ApplyCursor();
}

extern "C" void YoghourtCursorHideUntilMouseMoves(void) {
    [NSCursor setHiddenUntilMouseMoves:YES];
}

extern "C" void YoghourtCursorTick(void) {
    if(!gWindow || gHidden || gHandle < TVPCursorHandleBase)
        return;

    const tAnimatedCursor *animated = LoadedCursor(gHandle);
    if(!animated || animated->Cursors.size() < 2)
        return;

    const long long now = NowMilliseconds();
    if(!gTicking) {
        gTicking = true;
        gLastTick = now;
        return;
    }

    const int elapsed = (int)(now - gLastTick);
    gLastTick = now;
    if(elapsed <= 0)
        return;

    gFrameElapsedMS += elapsed;
    while(gFrameElapsedMS >= animated->DurationsMS[gFrameIndex]) {
        gFrameElapsedMS -= animated->DurationsMS[gFrameIndex];
        gFrameIndex = (gFrameIndex + 1) % (int)animated->Cursors.size();
        glfwSetCursor(gWindow, animated->Cursors[gFrameIndex]);
    }
}
