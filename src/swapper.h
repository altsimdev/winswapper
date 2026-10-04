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

// Which way windows travel between displays. Left is the original behaviour; Right
// is its exact inverse as a mapping between displays, so one of each returns every
// window to its own display - and to its exact pixel when the displays share a size.
enum class Rotation { Left, Right };

struct WindowRec
{
    HWND            hwnd    = nullptr;
    std::wstring    title;
    std::wstring    cls;
    RECT            rect    = {};   // GetWindowRect, screen coords
    WINDOWPLACEMENT wp      = {};
    WinState        state   = WinState::Normal;
    int             srcSlot = -1;   // index into the left-to-right display list
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

// Where the windows on display `slot` are headed. Displays are ordered
// left-to-right and everything rotates one place, with the end display wrapping
// around. With two displays either direction degenerates to the same straight
// swap, which is why the two-display behaviour is unchanged.
int DestSlot(int slot, int count, Rotation dir);

// rcNormalPosition lives in "workspace" coordinates, which differ from screen
// coordinates by the primary monitor's work-area origin (non-zero only when the
// taskbar sits at the top or left edge).
POINT WorkspaceOffset();
RECT  WorkspaceToScreen(const RECT& r, POINT off);
RECT  ScreenToWorkspace(const RECT& r, POINT off);

// Maps a rect from one monitor's work area onto another's, preserving relative
// position and size. When the two work areas are the same size this is an exact
// integer translation, so a full cycle of N rotations is a perfect round trip.
RECT RemapRect(const RECT& r, const RECT& srcWork, const RECT& dstWork, const RECT& dstBounds);

bool ApplyOne(const WindowRec& w);

// Moves every window in plan, then puts back the stacking order and the focused
// window that re-maximizing disturbs. plan must still be in its original
// top-to-bottom order - the order EnumWindows produced it in. Returns which
// windows were actually moved; each failure is logged.
std::vector<bool> ApplyPlan(const std::vector<WindowRec>& plan, HWND self);

SwapResult PerformSwap(HWND self, bool dryRun, Rotation dir);
void       ListAll(HWND self);
int        SelfTest();
