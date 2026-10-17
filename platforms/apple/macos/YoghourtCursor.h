//
//  macOS: the game window's native mouse cursor.
//
#pragma once

// The game window is a GLFW window created by cocos2d-x.  GLFW re-applies its
// own cursor on every AppKit cursor update, so a cursor has to be installed
// through glfwSetCursor(); that needs the GLFW handle, which cocos2d-x does not
// hand out.  The runtime therefore creates the game view through
// YoghourtCreateRuntimeGLView() and keeps the handle here.
extern "C" {

//! Creates the game window (the cocos2d-x GLView) and attaches it to this
//! module.  Returns the GLView as void * or nullptr when window creation
//! failed.
void *YoghourtCreateRuntimeGLView(const char *title, int width, int height);

//! Attaches an already created GLFW window as the cursor target.  Called by
//! the view factory above and by the cursor component test.
void YoghourtCursorAttachWindow(void *glfwWindow);

//! Applies a layer cursor.  The handle is one of the cr* constants or a value
//! returned by TVPGetCursor(); crDefault keeps the plain arrow.
void YoghourtCursorApply(int handle);

//! tTVPMouseCursorState::mcsHidden: no cursor over the game window until the
//! script asks for a visible one again.
void YoghourtCursorSetHidden(int hidden);

//! mcsTempHidden and Window.hideMouseCursor(): invisible until the mouse moves.
void YoghourtCursorHideUntilMouseMoves(void);

//! Advances an animated cursor.  Call once per frame.
void YoghourtCursorTick(void);

//! Advances an animated cursor at a monotonic timestamp in milliseconds.
//! Component checks supply timestamps here; the runtime uses Tick().
void YoghourtCursorTickAt(long long now);
}
