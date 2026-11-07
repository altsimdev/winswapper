#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

// Moving and resizing a window by holding a modifier - Alt, unless the settings say
// otherwise - and dragging anywhere in it: with the left mouse button to move it, with
// the right to resize it, as many Linux desktops do. A middle-click with the keys held
// sends the window to the back, as KDE does.
//
// A low-level mouse hook, on a thread of its own, watches for a button going down while
// exactly those modifiers are held. Over a window it may change, it takes the press, so
// the program under the pointer never sees it. If the pointer then moves further than
// a click can wander, the window comes to the front and either follows the pointer or,
// for a resize, has its corner or edge nearest the pointer follow it, until the button
// is released. A maximized window first returns to its normal size, under the pointer;
// to be resized, it becomes an ordinary window filling the same work area. If the
// pointer stays put, the press and release are replayed when the button comes up, so a
// plain Alt+click or Alt+right-click still reaches the program. A middle-click does its
// work as the button goes down, and the program sees neither half of it.
//
// While a window is dragged, an edge that comes near the edge of a display's work area
// snaps flush against it - its visible edge, that is: Windows 10 and 11 draw most
// windows with invisible borders several pixels wide, which would otherwise leave a gap.

// Posted to DragOptions::notify when the drag thread has lines for the log, which
// DragTakeLog() then hands over, oldest first. The message carries nothing itself, so
// a stray one from elsewhere is harmless.
const UINT kDragLogMessage = WM_APP + 2;
std::vector<std::wstring> DragTakeLog();

struct DragOptions
{
    UINT                      moveMods   = 0;         // MOD_* bits for the left button; 0: no moving
    UINT                      resizeMods = 0;         // the same for resizing, with the right button
    UINT                      lowerMods  = 0;         // and for sending to the back, with a middle-click
    int                       snap       = 0;         // how near an edge snaps, in pixels at 100% scaling; 0: never
    std::vector<std::wstring> skip;                   // programs left alone, as Settings::dragSkip
    HWND                      notify     = nullptr;   // gets kDragLogMessage
};

// Starts watching the mouse with these options, ending any earlier watch first. With
// all three kinds of mods 0 it only ends it. False if the hook could not be installed,
// with the Windows error in `error`. Both are for the thread that runs the tray app's
// message loop.
bool DragStart(const DragOptions& opts, DWORD& error);
void DragStop();

// ---------------------------------------------- the parts the self-test drives ---

// Where a maximized window goes when a drag un-maximizes it: at its normal size,
// placed so the pointer keeps the same relative position within it.
RECT RestoredUnderPointer(const RECT& maximized, SIZE normal, POINT pt);

// The edges a resize moves.
enum : UINT { kEdgeLeft = 1, kEdgeRight = 2, kEdgeTop = 4, kEdgeBottom = 8 };

// Which edges a resize grabbed at pt moves. The window is divided into thirds each
// way: grabbed in a corner third, it is resized by that corner; in the middle third
// of a side, by that side alone; in the very middle, by the nearest corner.
UINT EdgesAt(const RECT& r, POINT pt);

// r with `edges` moved by `delta`, then made at least minSize - but only by moving
// those same edges back, so the others stay where they are. Never less than 1x1.
RECT ResizedRect(const RECT& r, UINT edges, POINT delta, SIZE minSize);

// Snapping. A window at r shows its frame `insets` inside r on each side - the invisible
// borders - and `areas` are the work areas of all the displays. Each visible edge within
// `distance` of the same edge of a work area beside it - one it overlaps the other way -
// is pulled flush against that edge, the nearest one if there are several.
//
// SnappedMove() keeps the window's size, so of its left and right edges only the nearer
// one snaps, and the same for top and bottom. SnappedResize() snaps only `edges`, each
// on its own. A distance of 0 or less leaves r as it is.
RECT SnappedMove(const RECT& r, const RECT& insets, const std::vector<RECT>& areas, LONG distance);
RECT SnappedResize(const RECT& r, UINT edges, const RECT& insets, const std::vector<RECT>& areas,
                   LONG distance);

// What a press asks for.
enum class MouseAction { Move, Resize, Lower };

// What dragging needs from Windows, kept behind an interface so that the self-test can
// stand in for any of it.
class DragEnv
{
public:
    virtual ~DragEnv() = default;

    virtual HWND              WindowAt(POINT pt) = 0;                 // the top-level window there, or nullptr
    virtual const wchar_t*    Refusal(HWND h, MouseAction a) = 0;     // why h may not be, or nullptr
    virtual bool              Maximized(HWND h) = 0;
    virtual RECT              Rect(HWND h) = 0;                       // GetWindowRect
    virtual SIZE              NormalSize(HWND h) = 0;                 // its size when not maximized
    virtual RECT              WorkArea(HWND h) = 0;                   // of the display it is on
    virtual std::vector<RECT> WorkAreas() = 0;                        // of every display
    virtual RECT              FrameInsets(HWND h) = 0;                // how far inside Rect(h) its visible frame is

    virtual void Begin(HWND h) = 0;                     // a drag starts: bring h to the front
    virtual void Restore(HWND h, const RECT& to) = 0;   // un-maximize h, landing at `to`
    virtual void Move(HWND h, POINT topLeft) = 0;
    virtual void Resize(HWND h, const RECT& to, UINT edges) = 0;   // edges: the ones being dragged
    virtual void End(HWND h, const RECT& from, const RECT& to, bool resized, bool restored) = 0;
    virtual void Lower(HWND h, POINT pt) = 0;           // to the back; pt: where it was clicked
    virtual void Refused(HWND h, const wchar_t* why, MouseAction a) = 0;

    virtual void MaskModifier() = 0;           // so releasing Alt or Win opens no menu
    virtual void ReplayClick(bool right) = 0;  // a press that never became a drag
};

// The MOD_* bits that must be held, exactly, for each kind of press; 0 turns it off.
struct DragKeys
{
    UINT move   = 0;   // dragging with the left button
    UINT resize = 0;   // dragging with the right
    UINT lower  = 0;   // a middle-click
};

// The decisions: which mouse events to take, when a press becomes a drag, and where
// the window goes. It is fed the hook's events and moves nothing itself.
class DragTracker
{
public:
    // threshold: how far the pointer may wander, either way, before a press counts as a
    // drag. minSize: the smallest a resize makes a window, unless it was smaller still
    // when the drag began - Windows' own minimum; a program may insist on more, which
    // the Windows side allows for. snap: how near, in pixels, a dragged edge has to come
    // to the edge of a display to snap to it; 0 turns snapping off.
    DragTracker(DragEnv& env, const DragKeys& keys, SIZE threshold, SIZE minSize, LONG snap = 0);

    // msg is the hook's WM_LBUTTONDOWN, WM_MOUSEMOVE and so on, held the MOD_* bits of
    // the modifiers down at that moment, and replay marks a click ReplayClick() put
    // back, which always goes through. True when the event is taken: nothing else
    // sees it.
    bool OnMouse(UINT msg, POINT pt, UINT held, bool replay);

    // Ends a drag in progress where it is, and forgets a press not yet a drag. The
    // release of a press already taken is still taken when it comes.
    void Cancel();

    bool Dragging() const { return state_ == State::Dragging; }

private:
    enum class State { Idle, Pressed, Dragging };
    enum : UINT { kLeft = 1, kRight = 2, kMiddle = 4 };

    bool Press(UINT button, POINT pt, UINT held);
    bool Release(UINT button, POINT pt);
    void Start(POINT pt);
    void Follow(POINT pt);
    bool Snapping();
    void Finish();

    DragEnv&          env_;
    DragKeys          keys_;
    SIZE              threshold_;
    SIZE              minSize_;
    LONG              snap_;
    State             state_    = State::Idle;
    UINT              taken_    = 0;         // buttons whose press was taken, so their release is too
    UINT              button_   = 0;         // kLeft to move, kRight to resize: the press that began it
    HWND              target_   = nullptr;
    POINT             down_     = {};        // where the button went down
    RECT              from_     = {};        // where the window was before the drag
    RECT              base_     = {};        // where it was as the drag took hold - after un-maximizing it, if so
    POINT             anchor_   = {};        // the pointer position that base_ goes with
    UINT              edges_    = 0;         // a resize: the edges being dragged
    RECT              rect_     = {};        // where the window was last asked to be
    bool              restored_ = false;     // it was maximized, and the drag un-maximized it
    std::vector<RECT> areas_;                // every display's work area, for snapping
    RECT              insets_   = {};        // the window's invisible borders
    bool              insetsRead_ = false;   // not until it is no longer maximized
};

// The real thing, with the Windows calls in. Its MaskModifier() and ReplayClick() post
// to the calling thread's queue, for the drag thread to act on once the hook returns;
// Lower() hands the work to a thread of its own. Log lines go to `notify` as
// kDragLogMessage; with nullptr they are dropped.
std::unique_ptr<DragEnv> MakeWindowsDragEnv(const std::vector<std::wstring>& skip, HWND notify);
