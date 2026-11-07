#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

// Moving a window by holding a modifier - Alt, unless the settings say otherwise - and
// dragging anywhere in it with the left mouse button, as many Linux desktops do.
//
// A low-level mouse hook, on a thread of its own, watches for the left button going
// down while exactly those modifiers are held. Over a window it can move, it takes the
// press, so the program under the pointer never sees it. If the pointer then moves
// further than a click can wander, the window comes to the front and follows the
// pointer until the button is released; a maximized window first returns to its normal
// size, under the pointer. If the pointer stays put, the press and release are replayed
// when the button comes up, so a plain Alt+click still reaches the program.

// Posted to DragOptions::notify when the drag thread has lines for the log, which
// DragTakeLog() then hands over, oldest first. The message carries nothing itself, so
// a stray one from elsewhere is harmless.
const UINT kDragLogMessage = WM_APP + 2;
std::vector<std::wstring> DragTakeLog();

struct DragOptions
{
    UINT                      mods   = 0;         // MOD_* bits; 0 means dragging is off
    std::vector<std::wstring> skip;               // programs left alone, as Settings::dragSkip
    HWND                      notify = nullptr;   // gets kDragLogMessage
};

// Starts watching the mouse with these options, ending any earlier watch first. With
// mods 0 it only ends it. False if the hook could not be installed, with the Windows
// error in `error`. Both are for the thread that runs the tray app's message loop.
bool DragStart(const DragOptions& opts, DWORD& error);
void DragStop();

// ---------------------------------------------- the parts the self-test drives ---

// Where a maximized window goes when a drag un-maximizes it: at its normal size,
// placed so the pointer keeps the same relative position within it.
RECT RestoredUnderPointer(const RECT& maximized, SIZE normal, POINT pt);

// What dragging needs from Windows, kept behind an interface so that the self-test can
// stand in for any of it.
class DragEnv
{
public:
    virtual ~DragEnv() = default;

    virtual HWND           WindowAt(POINT pt) = 0;   // the top-level window there, or nullptr
    virtual const wchar_t* Refusal(HWND h) = 0;      // why h may not be dragged, or nullptr
    virtual bool           Maximized(HWND h) = 0;
    virtual RECT           Rect(HWND h) = 0;         // GetWindowRect
    virtual SIZE           NormalSize(HWND h) = 0;   // its size when not maximized

    virtual void Begin(HWND h) = 0;                     // a drag starts: bring h to the front
    virtual void Restore(HWND h, const RECT& to) = 0;   // un-maximize h, landing at `to`
    virtual void Move(HWND h, POINT topLeft) = 0;
    virtual void End(HWND h, const RECT& from, POINT to, bool restored) = 0;
    virtual void Refused(HWND h, const wchar_t* why) = 0;

    virtual void MaskModifier() = 0;   // so releasing Alt or Win opens no menu
    virtual void ReplayClick() = 0;    // a press that never became a drag
};

// The decisions: which mouse events to take, when a press becomes a drag, and where
// the window goes. It is fed the hook's events and moves nothing itself.
class DragTracker
{
public:
    // mods: the MOD_* bits that must be held, exactly; 0 takes nothing. threshold: how
    // far the pointer may wander, either way, before a press counts as a drag.
    DragTracker(DragEnv& env, UINT mods, SIZE threshold);

    // msg is the hook's WM_LBUTTONDOWN, WM_MOUSEMOVE and so on, held the MOD_* bits of
    // the modifiers down at that moment, and replay marks a click ReplayClick() put
    // back, which always goes through. True when the event is taken: nothing else
    // sees it.
    bool OnMouse(UINT msg, POINT pt, UINT held, bool replay);

    // Ends a drag in progress where it is, and forgets a press not yet a drag.
    void Cancel();

    bool Dragging() const { return state_ == State::Dragging; }

private:
    enum class State { Idle, Pressed, Dragging };

    void Start(POINT pt);
    void Follow(POINT pt);
    void Finish();

    DragEnv& env_;
    UINT     mods_;
    SIZE     threshold_;
    State    state_    = State::Idle;
    HWND     target_   = nullptr;
    POINT    down_     = {};      // where the button went down
    POINT    anchor_   = {};      // the pointer position that origin_ goes with
    POINT    origin_   = {};      // the window's top-left as the drag started
    POINT    at_       = {};      // its top-left as last asked for
    RECT     from_     = {};      // where it was before the drag, for the log
    bool     restored_ = false;   // it was maximized, and the drag un-maximized it
};

// The real thing, with the Windows calls in. Its MaskModifier() and ReplayClick() post
// to the calling thread's queue, for the drag thread to act on once the hook returns.
// Log lines go to `notify` as kDragLogMessage; with nullptr they are dropped.
std::unique_ptr<DragEnv> MakeWindowsDragEnv(const std::vector<std::wstring>& skip, HWND notify);
