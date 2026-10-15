//---------------------------------------------------------------------------
/*
        TVP2 ( T Visual Presenter 2 )  A script authoring tool
        Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

        See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Windows mouse cursor resources (.cur / .ani)
//---------------------------------------------------------------------------
#ifndef CursorFileH
#define CursorFileH

#include <cstddef>
#include <vector>

#include "tjs.h"

/*[*/
//---------------------------------------------------------------------------
// Values a script can assign to Layer.cursor, see the "mouse cursor
// constants" in ScriptMgnIntf.cpp.  Kept in C++ because the window layer and
// the macOS cursor host have to translate them.
//---------------------------------------------------------------------------
enum tTVPCursorId {
    crDefault = 0x0,
    crNone = -1,
    crArrow = -2,
    crCross = -3,
    crIBeam = -4,
    crSize = -5,
    crSizeNESW = -6,
    crSizeNS = -7,
    crSizeNWSE = -8,
    crSizeWE = -9,
    crUpArrow = -10,
    crHourGlass = -11,
    crDrag = -12,
    crNoDrop = -13,
    crHSplit = -14,
    crVSplit = -15,
    crMultiDrag = -16,
    crSQLWait = -17,
    crNo = -18,
    crAppStart = -19,
    crHelp = -20,
    crHandPoint = -21,
    crSizeAll = -22,
    crHBeam = 1,
};

//---------------------------------------------------------------------------
// Loaded cursors
//---------------------------------------------------------------------------
//! Handle base of the loaded-cursor registry.  Scripts only pass the handles
//! returned by TVPGetCursor() around, but they must stay clear of the cr*
//! values above (crHBeam is positive).
constexpr tjs_int TVPCursorHandleBase = 0x1000;

//! One decoded cursor image.
struct tTVPCursorImage {
    int Width = 0;
    int Height = 0;
    //! RGBA8888, top row first, Width * Height * 4 bytes.
    std::vector<tjs_uint8> Pixels;
};

//! One step of a cursor animation.  A still cursor holds exactly one step.
struct tTVPCursorStep {
    tTVPCursorImage Image;
    //! Time the step stays on screen; 0 means "no animation".
    int DurationMS = 0;
};

struct tTVPCursorResource {
    std::vector<tTVPCursorStep> Steps;
    int HotSpotX = 0;
    int HotSpotY = 0;
};

//---------------------------------------------------------------------------
//! Decodes a .cur or .ani image.  Returns false for unsupported or damaged
//! files; the caller decides whether that is an error.
bool TVPDecodeCursorFile(const void *data, size_t size, tTVPCursorResource &out);

//! Stores a decoded cursor and returns its script visible handle.
tjs_int TVPCursorRegister(tTVPCursorResource resource);

//! Returns the cursor TVPCursorRegister() stored, or nullptr for a handle that
//! is not a loaded cursor.
const tTVPCursorResource *TVPCursorFind(tjs_int handle);

/*]*/
#endif
