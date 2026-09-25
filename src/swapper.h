#pragma once

#include <windows.h>

#include <string>
#include <vector>

struct MonitorRec
{
    HMONITOR     handle  = nullptr;
    RECT         bounds  = {};      // MONITORINFO::rcMonitor
    RECT         work    = {};      // MONITORINFO::rcWork (taskbar excluded)
    bool         primary = false;
    std::wstring device;            // e.g. \.\DISPLAY5
};

enum class WinState { Normal, Maximized, Minimized };

struct WindowRec
{
    HWND            hwnd    = nullptr;
    std::wstring    title;
    std::wstring    cls;
    RECT            rect    = {};   // GetWindowRect, screen coords
    WINDOWPLACEMENT wp      = {};
    WinState        state   = WinState::Normal;
    int             srcSlot = -1;   // index into the swapped monitor pair (0 or 1)
    int             dstSlot = -1;

    RECT targetRect   = {};         // remapped rect in screen coords (logging + normal moves)
    RECT targetNormal = {};         // remapped rcNormalPosition in workspace coords
};

struct SwapResult
{
    int monitors   = 0;
    int considered = 0;
    int moved      = 0;
    int failed     = 0;
};

std::vector<MonitorRec> EnumerateMonitors();

// rcNormalPosition lives in "workspace" coordinates, which differ from screen
// coordinates by the primary monitor's work-area origin (non-zero only when the
// taskbar sits at the top or left edge).
POINT WorkspaceOffset();
RECT  WorkspaceToScreen(const RECT& r, POINT off);
RECT  ScreenToWorkspace(const RECT& r, POINT off);

// Maps a rect from one monitor's work area onto another's, preserving relative
// position and size. When the two work areas are the same size this is an exact
// integer translation, so swapping twice is a perfect round trip.
RECT RemapRect(const RECT& r, const RECT& srcWork, const RECT& dstWork, const RECT& dstBounds);

bool ApplyOne(const WindowRec& w);

SwapResult PerformSwap(HWND self, bool dryRun);
void       ListAll(HWND self);
int        SelfTest();
