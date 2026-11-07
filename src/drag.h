#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

// Moving and resizing a window by holding a modifier - Alt, unless the settings say
// otherwise - and dragging anywhere in it: with the left mouse button to move it, with
// the right to resize it, as many Linux desktops do.
//
// A low-level mouse hook, on a thread of its own, watches for a button going down while
// exactly those modifiers are held. Over a window it may change, it takes the press, so
// the program under the pointer never sees it. If the pointer then moves further than
// a click can wander, the window comes to the front and either follows the pointer or,
// for a resize, has its corner or edge nearest the pointer follow it, until the button
// is released. A maximized window first returns to its normal size, under the pointer;
// to be resized, it becomes an ordinary window filling the same work area. If the
// pointer stays put, the press and release are replayed when the button comes up, so a
// plain Alt+click or Alt+right-click still reaches the program.

// Posted to DragOptions::notify when the drag thread has lines for the log, which
// DragTakeLog() then hands over, oldest first. The message carries nothing itself, so
// a stray one from elsewhere is harmless.
const UINT kDragLogMessage = WM_APP + 2;
std::vector<std::wstring> DragTakeLog();

struct DragOptions
{
    UINT                      moveMods   = 0;         // MOD_* bits for the left button; 0: no moving
    UINT                      resizeMods = 0;         // the same for resizing, with the right button
    std::vector<std::wstring> skip;                   // programs left alone, as Settings::dragSkip
    HWND                      notify     = nullptr;   // gets kDragLogMessage
};

// Starts watching the mouse with these options, ending any earlier watch first. With
// both kinds of mods 0 it only ends it. False if the hook could not be installed, with
// the Windows error in `error`. Both are for the thread that runs the tray app's
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

// What dragging needs from Windows, kept behind an interface so that the self-test can
// stand in for any of it.
class DragEnv
{
public:
    virtual ~DragEnv() = default;

    virtual HWND           WindowAt(POINT pt) = 0;             // the top-level window there, or nullptr
    virtual const wchar_t* Refusal(HWND h, bool resize) = 0;   // why h may not be moved or resized, or nullptr
    virtual bool           Maximized(HWND h) = 0;
    virtual RECT           Rect(HWND h) = 0;                   // GetWindowRect
    virtual SIZE           NormalSize(HWND h) = 0;             // its size when not maximized
    virtual RECT           WorkArea(HWND h) = 0;               // of the display it is on

    virtual void Begin(HWND h) = 0;                     // a drag starts: bring h to the front
    virtual void Restore(HWND h, const RECT& to) = 0;   // un-maximize h, landing at `to`
    virtual void Move(HWND h, POINT topLeft) = 0;
    virtual void Resize(HWND h, const RECT& to, UINT edges) = 0;   // edges: the ones being dragged
    virtual void End(HWND h, const RECT& from, const RECT& to, bool resized, bool restored) = 0;
    virtual void Refused(HWND h, const wchar_t* why, bool resize) = 0;

    virtual void MaskModifier() = 0;           // so releasing Alt or Win opens no menu
    virtual void ReplayClick(bool right) = 0;  // a press that never became a drag
};

// The decisions: which mouse events to take, when a press becomes a drag, and where
// the window goes. It is fed the hook's events and moves nothing itself.
class DragTracker
{
public:
    // moveMods and resizeMods: the MOD_* bits that must be held, exactly, to move a
    // window with the left button and to resize it with the right; 0 turns that off.
    // threshold: how far the pointer may wander, either way, before a press counts as a
    // drag. minSize: the smallest a resize makes a window, unless it was smaller still
    // when the drag began - Windows' own minimum; a program may insist on more, which
    // the Windows side allows for.
    DragTracker(DragEnv& env, UINT moveMods, UINT resizeMods, SIZE threshold, SIZE minSize);

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
    enum : UINT { kLeft = 1, kRight = 2 };

    bool Press(bool right, POINT pt, UINT held);
    bool Release(bool right, POINT pt);
    void Start(POINT pt);
    void Follow(POINT pt);
    void Finish();

    DragEnv& env_;
    UINT     moveMods_;
    UINT     resizeMods_;
    SIZE     threshold_;
    SIZE     minSize_;
    State    state_    = State::Idle;
    UINT     taken_    = 0;         // kLeft, kRight: buttons whose press was taken, so their release is too
    bool     resizing_ = false;     // the press was the right button's: a resize, not a move
    HWND     target_   = nullptr;
    POINT    down_     = {};        // where the button went down
    RECT     from_     = {};        // where the window was before the drag
    RECT     base_     = {};        // where it was as the drag took hold - after un-maximizing it, if so
    POINT    anchor_   = {};        // the pointer position that base_ goes with
    UINT     edges_    = 0;         // a resize: the edges being dragged
    RECT     rect_     = {};        // where the window was last asked to be
    bool     restored_ = false;     // it was maximized, and the drag un-maximized it
};

// The real thing, with the Windows calls in. Its MaskModifier() and ReplayClick() post
// to the calling thread's queue, for the drag thread to act on once the hook returns.
// Log lines go to `notify` as kDragLogMessage; with nullptr they are dropped.
std::unique_ptr<DragEnv> MakeWindowsDragEnv(const std::vector<std::wstring>& skip, HWND notify);
