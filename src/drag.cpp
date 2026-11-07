#include "drag.h"

#include "swapper.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <utility>

// ---------------------------------------------------------------- the math ---

RECT RestoredUnderPointer(const RECT& maximized, SIZE normal, POINT pt)
{
    // How far across the maximized window the pointer is, from 0 to 1.
    const auto spot = [](LONG p, LONG lo, LONG hi) {
        if (hi <= lo) return 0.5;
        const double f = static_cast<double>(p - lo) / static_cast<double>(hi - lo);
        return f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);
    };
    const double fx = spot(pt.x, maximized.left, maximized.right);
    const double fy = spot(pt.y, maximized.top,  maximized.bottom);

    RECT r;
    r.left   = pt.x - static_cast<LONG>(std::lround(fx * normal.cx));
    r.top    = pt.y - static_cast<LONG>(std::lround(fy * normal.cy));
    r.right  = r.left + normal.cx;
    r.bottom = r.top  + normal.cy;
    return r;
}

UINT EdgesAt(const RECT& r, POINT pt)
{
    // Which third of the span from lo to hi p is in: -1, 0 or 1. An empty span is all
    // middle.
    const auto third = [](LONG p, LONG lo, LONG hi) {
        const long long span = static_cast<long long>(hi) - lo;
        const long long at   = static_cast<long long>(p) - lo;
        if (span <= 0)          return 0;
        if (3 * at < span)      return -1;
        if (3 * at >= 2 * span) return 1;
        return 0;
    };
    // Which half: -1 or 1.
    const auto half = [](LONG p, LONG lo, LONG hi) {
        return 2 * (static_cast<long long>(p) - lo) < static_cast<long long>(hi) - lo ? -1 : 1;
    };

    int x = third(pt.x, r.left, r.right);
    int y = third(pt.y, r.top,  r.bottom);
    if (x == 0 && y == 0)
    {
        x = half(pt.x, r.left, r.right);
        y = half(pt.y, r.top,  r.bottom);
    }

    UINT edges = 0;
    if (x < 0) edges |= kEdgeLeft;
    if (x > 0) edges |= kEdgeRight;
    if (y < 0) edges |= kEdgeTop;
    if (y > 0) edges |= kEdgeBottom;
    return edges;
}

RECT ResizedRect(const RECT& r, UINT edges, POINT delta, SIZE minSize)
{
    RECT out = r;
    if (edges & kEdgeLeft)   out.left   += delta.x;
    if (edges & kEdgeRight)  out.right  += delta.x;
    if (edges & kEdgeTop)    out.top    += delta.y;
    if (edges & kEdgeBottom) out.bottom += delta.y;

    // Too small, or even turned inside out: the dragged edge goes back far enough.
    const auto atLeast = [](LONG& lo, LONG& hi, bool loMoves, bool hiMoves, LONG least) {
        least = std::max(least, 1L);
        if ((!loMoves && !hiMoves) || hi - lo >= least) return;
        if (loMoves) lo = hi - least;
        else         hi = lo + least;
    };
    atLeast(out.left, out.right,  (edges & kEdgeLeft) != 0, (edges & kEdgeRight)  != 0, minSize.cx);
    atLeast(out.top,  out.bottom, (edges & kEdgeTop)  != 0, (edges & kEdgeBottom) != 0, minSize.cy);
    return out;
}

// ------------------------------------------------------------- the tracker ---

DragTracker::DragTracker(DragEnv& env, UINT moveMods, UINT resizeMods, SIZE threshold, SIZE minSize)
    : env_(env), moveMods_(moveMods), resizeMods_(resizeMods), threshold_(threshold), minSize_(minSize)
{
}

bool DragTracker::OnMouse(UINT msg, POINT pt, UINT held, bool replay)
{
    if (replay) return false;

    switch (msg)
    {
    case WM_LBUTTONDOWN: return Press(false, pt, held);
    case WM_RBUTTONDOWN: return Press(true,  pt, held);
    case WM_LBUTTONUP:   return Release(false, pt);
    case WM_RBUTTONUP:   return Release(true,  pt);

    case WM_MOUSEMOVE:
        if (state_ == State::Pressed &&
            (std::abs(pt.x - down_.x) > threshold_.cx || std::abs(pt.y - down_.y) > threshold_.cy))
            Start(pt);
        if (state_ == State::Dragging) Follow(pt);
        return false;   // never taken: the pointer itself has to move

    default:
        return false;
    }
}

bool DragTracker::Press(bool right, POINT pt, UINT held)
{
    const UINT button = right ? kRight : kLeft;

    // A press of a button whose last press is still taken means that release went
    // missing - to a secure desktop, say - so the drag it began ends where it is.
    if (taken_ & button)
    {
        taken_ &= ~button;
        if (state_ != State::Idle && resizing_ == right) Cancel();
    }

    // The other button, while a drag is under way or about to be: taken as well, and
    // its release with it, so the program gets neither half of a stray click.
    if (state_ != State::Idle)
    {
        taken_ |= button;
        return true;
    }

    const UINT mods = right ? resizeMods_ : moveMods_;
    if (mods == 0 || held != mods) return false;

    const HWND h = env_.WindowAt(pt);
    if (!h) return false;
    if (const wchar_t* why = env_.Refusal(h, right))
    {
        env_.Refused(h, why, right);
        return false;
    }

    target_   = h;
    down_     = pt;
    resizing_ = right;
    state_    = State::Pressed;
    taken_   |= button;

    // The program never sees this press, so to Windows, Alt or Win would look
    // pressed and released on its own, which opens the menu bar or Start.
    if (mods & (MOD_ALT | MOD_WIN)) env_.MaskModifier();
    return true;
}

bool DragTracker::Release(bool right, POINT pt)
{
    const UINT button = right ? kRight : kLeft;
    if (!(taken_ & button)) return false;
    taken_ &= ~button;   // its press was taken, so its release is too

    if (state_ == State::Idle || resizing_ != right) return true;
    if (state_ == State::Pressed)
    {
        // Not a drag after all, so the program gets its click - late, but whole.
        state_  = State::Idle;
        target_ = nullptr;
        env_.ReplayClick(right);
        return true;
    }
    Follow(pt);
    Finish();
    return true;
}

void DragTracker::Start(POINT pt)
{
    env_.Begin(target_);
    from_     = env_.Rect(target_);
    restored_ = env_.Maximized(target_);

    // From where the button went down, so the window catches up with the pointer at
    // once instead of trailing it by the threshold.
    base_   = from_;
    anchor_ = down_;

    if (resizing_)
    {
        // A maximized window has no edges to drag, so it first becomes an ordinary
        // window filling the same work area, and is resized from there.
        if (restored_)
        {
            base_ = env_.WorkArea(target_);
            env_.Restore(target_, base_);
        }
        edges_ = EdgesAt(base_, down_);
    }
    else if (restored_)
    {
        // As when dragging a maximized window by its title bar: it returns to its
        // normal size under the pointer, and follows from there.
        base_   = RestoredUnderPointer(from_, env_.NormalSize(target_), pt);
        anchor_ = pt;
        env_.Restore(target_, base_);
    }
    rect_  = base_;
    state_ = State::Dragging;
}

void DragTracker::Follow(POINT pt)
{
    const POINT by = { pt.x - anchor_.x, pt.y - anchor_.y };
    RECT        to = base_;
    if (resizing_)
    {
        // No smaller than Windows' minimum - unless the window began smaller still.
        const SIZE least = { std::min(minSize_.cx, base_.right - base_.left),
                             std::min(minSize_.cy, base_.bottom - base_.top) };
        to = ResizedRect(base_, edges_, by, least);
    }
    else
        OffsetRect(&to, by.x, by.y);

    if (EqualRect(&to, &rect_)) return;
    rect_ = to;
    if (resizing_) env_.Resize(target_, to, edges_);
    else           env_.Move(target_, { to.left, to.top });
}

void DragTracker::Finish()
{
    env_.End(target_, from_, rect_, resizing_, restored_);
    state_  = State::Idle;
    target_ = nullptr;
}

void DragTracker::Cancel()
{
    if (state_ == State::Dragging) Finish();
    state_  = State::Idle;
    target_ = nullptr;
}

// --------------------------------------------------------------- Windows side ---

namespace
{

// Marks the input this file injects - replayed clicks, and the masking key - so the
// hook lets it through. Any value nothing else is likely to use would do.
const ULONG_PTR kOurInput = 0x57535744;   // "WSWD"

// Pen and touch arrive as mouse events too, tagged like this. Dragging leaves them be.
bool FromPenOrTouch(ULONG_PTR extra)
{
    return (extra & 0xFFFFFF00) == 0xFF515700;
}

// A key no keyboard has (0xE8 is unassigned). Pressing it does nothing, except tell
// Windows that a key went by while Alt or Win was down - which is what stops their
// release opening the menu bar or Start.
const WORD kMaskKey = 0xE8;

// Posted by the environment to the drag thread itself.
const UINT kMsgMask   = WM_APP + 1;
const UINT kMsgReplay = WM_APP + 2;

const UINT kMoveFlags = SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE;
const UINT kSizeFlags = SWP_NOZORDER | SWP_NOACTIVATE;

void BringToFront(HWND h, bool async)
{
    // A window with a dialog open is disabled. Clicking it would bring the dialog
    // forward, so this does too.
    HWND want = GetLastActivePopup(h);
    if (!want || !IsWindowVisible(want) || !IsWindowEnabled(want)) want = h;
    if (GetForegroundWindow() == want) return;
    if (!SetForegroundWindow(want))
        SetWindowPos(h, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | (async ? SWP_ASYNCWINDOWPOS : 0));
}

void Unmaximize(HWND h, const RECT& to, bool async)
{
    WINDOWPLACEMENT wp = {};
    wp.length = sizeof(wp);
    if (!GetWindowPlacement(h, &wp)) return;

    // rcNormalPosition is in workspace coordinates, except for a tool window's.
    const bool  tool = (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0;
    const POINT none = { 0, 0 };
    wp.rcNormalPosition = ScreenToWorkspace(to, tool ? none : WorkspaceOffset());
    wp.showCmd          = SW_SHOWNOACTIVATE;
    wp.flags            = async ? WPF_ASYNCWINDOWPLACEMENT : 0;
    SetWindowPlacement(h, &wp);
}

// Resizes h to r, `edges` being the ones dragged. A program may keep its window larger
// than asked - its own minimum size - or otherwise not quite as asked. A window keeps
// its top-left corner when its size changes, so when the left or top edge is the one
// dragged, the window is then put back against its right or bottom edge rather than
// left to creep across the screen. Returns the size the program insisted on, in the
// directions being dragged, where it was more than asked; 0 elsewhere.
SIZE ApplySize(HWND h, const RECT& r, UINT edges)
{
    const LONG w  = r.right - r.left;
    const LONG ht = r.bottom - r.top;
    SetWindowPos(h, nullptr, r.left, r.top, w, ht, kSizeFlags);

    SIZE insisted = {};
    RECT got      = {};
    if (!GetWindowRect(h, &got)) return insisted;
    const LONG gw = got.right - got.left;
    const LONG gh = got.bottom - got.top;
    if ((edges & (kEdgeLeft | kEdgeRight)) && gw > w)  insisted.cx = gw;
    if ((edges & (kEdgeTop | kEdgeBottom)) && gh > ht) insisted.cy = gh;

    POINT at = { got.left, got.top };
    if ((edges & kEdgeLeft) && gw != w)  at.x = r.right  - gw;
    if ((edges & kEdgeTop)  && gh != ht) at.y = r.bottom - gh;
    if (at.x != got.left || at.y != got.top)
        SetWindowPos(h, nullptr, at.x, at.y, 0, 0, kMoveFlags);
    return insisted;
}

// Moves or resizes one window through one drag, on a thread of its own. A program slow
// to change its window - or hung - then holds up only that thread: never the hook, and
// so never the pointer. Positions that arrive while one is being applied replace each
// other, so only the newest is applied next and the window never falls behind.
class Mover
{
public:
    explicit Mover(HWND h);
    ~Mover() { Finish(); }

    Mover(const Mover&)            = delete;
    Mover& operator=(const Mover&) = delete;

    void Activate();
    void Restore(const RECT& to);
    void Move(POINT topLeft);
    void Resize(const RECT& to, UINT edges);
    RECT Fitted(const RECT& r) const;   // r, at least as large as the program has insisted on so far
    void Finish();   // what is still waiting is applied, then the thread ends

private:
    struct Shared
    {
        CRITICAL_SECTION cs;
        HANDLE           wake      = nullptr;   // auto-reset
        HWND             hwnd      = nullptr;
        bool             activate  = false;
        bool             restore   = false;
        bool             move      = false;
        bool             resize    = false;
        bool             finish    = false;
        RECT             restoreTo = {};
        POINT            moveTo    = {};
        RECT             sizeTo    = {};
        UINT             edges     = 0;    // the edges being dragged, for a resize
        SIZE             least     = {};   // the smallest the program has let the window be, as found so far

        Shared() { InitializeCriticalSection(&cs); }
        ~Shared()
        {
            DeleteCriticalSection(&cs);
            if (wake) CloseHandle(wake);
        }
    };

    static DWORD WINAPI Run(void* p);

    template <class F> void Queue(F change)
    {
        EnterCriticalSection(&s_->cs);
        change(*s_);
        LeaveCriticalSection(&s_->cs);
        SetEvent(s_->wake);
    }

    std::shared_ptr<Shared> s_;
    bool                    threaded_ = false;   // false: do each step here, asynchronously
    bool                    finished_ = false;
};

Mover::Mover(HWND h) : s_(std::make_shared<Shared>())
{
    s_->hwnd = h;
    s_->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!s_->wake) return;

    // The thread holds its own reference, so it can outlive this object - finishing a
    // move into a window that has stopped responding, say.
    auto* hold = new std::shared_ptr<Shared>(s_);
    const HANDLE t = CreateThread(nullptr, 0, Run, hold, 0, nullptr);
    if (!t)
    {
        delete hold;
        return;
    }
    CloseHandle(t);
    threaded_ = true;
}

DWORD WINAPI Mover::Run(void* p)
{
    const std::unique_ptr<std::shared_ptr<Shared>> hold(static_cast<std::shared_ptr<Shared>*>(p));
    Shared& s = **hold;

    for (;;)
    {
        WaitForSingleObject(s.wake, INFINITE);
        for (;;)
        {
            EnterCriticalSection(&s.cs);
            const bool  activate  = std::exchange(s.activate, false);
            const bool  restore   = std::exchange(s.restore,  false);
            const bool  move      = std::exchange(s.move,     false);
            const bool  resize    = std::exchange(s.resize,   false);
            const bool  finish    = s.finish;
            const RECT  restoreTo = s.restoreTo;
            const POINT moveTo    = s.moveTo;
            const RECT  sizeTo    = ResizedRect(s.sizeTo, s.edges, { 0, 0 }, s.least);
            const UINT  edges     = s.edges;
            LeaveCriticalSection(&s.cs);

            if (!activate && !restore && !move && !resize)
            {
                if (finish) return 0;
                break;
            }
            // Synchronous: this thread is the one place it is safe to wait on the
            // window, and waiting is what keeps only one change outstanding.
            if (activate) BringToFront(s.hwnd, false);
            if (restore)  Unmaximize(s.hwnd, restoreTo, false);
            if (move)     SetWindowPos(s.hwnd, nullptr, moveTo.x, moveTo.y, 0, 0, kMoveFlags);
            if (resize)
            {
                // Once the program has refused a size, later steps ask for no less, so
                // the window is not pushed out and pulled back every time.
                const SIZE insisted = ApplySize(s.hwnd, sizeTo, edges);
                EnterCriticalSection(&s.cs);
                s.least.cx = std::max(s.least.cx, insisted.cx);
                s.least.cy = std::max(s.least.cy, insisted.cy);
                LeaveCriticalSection(&s.cs);
            }
        }
    }
}

void Mover::Activate()
{
    if (!threaded_) { BringToFront(s_->hwnd, true); return; }
    Queue([](Shared& s) { s.activate = true; });
}

void Mover::Restore(const RECT& to)
{
    if (!threaded_) { Unmaximize(s_->hwnd, to, true); return; }
    Queue([&to](Shared& s) { s.restore = true; s.restoreTo = to; });
}

void Mover::Move(POINT topLeft)
{
    if (!threaded_)
    {
        SetWindowPos(s_->hwnd, nullptr, topLeft.x, topLeft.y, 0, 0, kMoveFlags | SWP_ASYNCWINDOWPOS);
        return;
    }
    Queue([topLeft](Shared& s) { s.move = true; s.moveTo = topLeft; });
}

void Mover::Resize(const RECT& to, UINT edges)
{
    if (!threaded_)
    {
        SetWindowPos(s_->hwnd, nullptr, to.left, to.top, to.right - to.left, to.bottom - to.top,
                     kSizeFlags | SWP_ASYNCWINDOWPOS);
        return;
    }
    Queue([&to, edges](Shared& s) { s.resize = true; s.sizeTo = to; s.edges = edges; });
}

RECT Mover::Fitted(const RECT& r) const
{
    if (!threaded_) return r;
    EnterCriticalSection(&s_->cs);
    const RECT out = ResizedRect(r, s_->edges, { 0, 0 }, s_->least);
    LeaveCriticalSection(&s_->cs);
    return out;
}

void Mover::Finish()
{
    if (!threaded_ || finished_) return;
    finished_ = true;
    Queue([](Shared& s) { s.finish = true; });
}

// The integrity level of a process - SECURITY_MANDATORY_MEDIUM_RID and so on - or 0
// if it cannot be read.
DWORD IntegrityOf(HANDLE process)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return 0;

    DWORD rid = 0;
    DWORD len = 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &len);
    std::vector<BYTE> buf(len);
    if (len && GetTokenInformation(token, TokenIntegrityLevel, buf.data(), len, &len))
    {
        const PSID  sid = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buf.data())->Label.Sid;
        const UCHAR n   = *GetSidSubAuthorityCount(sid);
        if (n > 0) rid = *GetSidSubAuthority(sid, n - 1u);
    }
    CloseHandle(token);
    return rid;
}

bool CoversItsDisplay(HWND h)
{
    RECT        r  = {};
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetWindowRect(h, &r) || !GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi))
        return false;
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top &&
           r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
}

std::wstring Describe(HWND h)
{
    // InternalGetWindowText rather than GetWindowText, which for a window of this
    // process sends it a message - and this runs on the hook's thread, which must
    // never wait on another.
    wchar_t title[256] = {};
    wchar_t cls[256]   = {};
    InternalGetWindowText(h, title, _countof(title));
    GetClassNameW(h, cls, _countof(cls));
    return std::wstring(L"\"") + title + L"\"  [" + cls + L"]";
}

class WindowsDragEnv : public DragEnv
{
public:
    WindowsDragEnv(const std::vector<std::wstring>& skip, HWND notify)
        : skip_(skip), notify_(notify), integrity_(IntegrityOf(GetCurrentProcess()))
    {
        if (integrity_ == 0) integrity_ = SECURITY_MANDATORY_MEDIUM_RID;
    }

    HWND WindowAt(POINT pt) override
    {
        // The window a click there would reach: hidden, cloaked and click-through
        // windows are passed over. Then its top-level window, since a child window
        // cannot be dragged away from its parent.
        const HWND h = WindowFromPoint(pt);
        return h ? GetAncestor(h, GA_ROOT) : nullptr;
    }

    const wchar_t* Refusal(HWND h, bool resize) override
    {
        wchar_t cls[256] = {};
        GetClassNameW(h, cls, _countof(cls));
        if (h == GetShellWindow() || IsShellClass(cls)) return L"it is the desktop or the taskbar";
        if (!IsWindowVisible(h) || IsIconic(h))         return L"it is not showing";

        // As for rotating: an unowned tool window belongs to no application window.
        const LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
        if ((ex & WS_EX_TOOLWINDOW) && !GetWindow(h, GW_OWNER)) return L"it is a tool window";
        if (IsCloaked(h))                                       return L"it is cloaked";
        if (!IsZoomed(h) && CoversItsDisplay(h))                return L"it is full screen";
        if (IsHungAppWindow(h))                                 return L"it is not responding";

        // A window without a sizing border - a dialog, say - was laid out for one size.
        if (resize && !(GetWindowLongPtrW(h, GWL_STYLE) & WS_THICKFRAME)) return L"its size is fixed";

        // Windows would refuse to move it (UIPI) - but only after its click had been
        // taken, so it is left alone from the start.
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        DWORD level = 0;
        if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))
        {
            level = IntegrityOf(p);
            CloseHandle(p);
        }
        if (level == 0)          return L"WinSwapper cannot check its program's rights";
        if (level > integrity_)  return L"its program runs as administrator";

        if (!skip_.empty())
        {
            const std::wstring entry = IgnoredBy(h, skip_);
            if (!entry.empty())
            {
                why_ = L"its program is skipped in settings (" + entry + L")";
                return why_.c_str();
            }
        }
        return nullptr;
    }

    bool Maximized(HWND h) override { return IsZoomed(h) != FALSE; }

    RECT Rect(HWND h) override
    {
        RECT r = {};
        GetWindowRect(h, &r);
        return r;
    }

    SIZE NormalSize(HWND h) override
    {
        WINDOWPLACEMENT wp = {};
        wp.length = sizeof(wp);
        GetWindowPlacement(h, &wp);
        const RECT& n = wp.rcNormalPosition;
        const SIZE  s = { n.right - n.left, n.bottom - n.top };
        return s;
    }

    RECT WorkArea(HWND h) override
    {
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) return Rect(h);
        return mi.rcWork;
    }

    void Begin(HWND h) override
    {
        mover_ = std::make_unique<Mover>(h);
        mover_->Activate();
    }

    void Restore(HWND, const RECT& to) override
    {
        if (mover_) mover_->Restore(to);
    }

    void Move(HWND, POINT topLeft) override
    {
        if (mover_) mover_->Move(topLeft);
    }

    void Resize(HWND, const RECT& to, UINT edges) override
    {
        if (mover_) mover_->Resize(to, edges);
    }

    void End(HWND h, const RECT& from, const RECT& to, bool resized, bool restored) override
    {
        // As far as is known yet, a resize stopped at the program's own minimum size.
        const RECT went = resized && mover_ ? mover_->Fitted(to) : to;
        mover_.reset();   // finishes it: the mover's thread applies what is waiting
        Post(std::wstring(resized ? L"Resized " : L"Moved ") + Describe(h) + L" from " + RectStr(from) +
             L" to " + RectStr(went) + (restored ? L", un-maximizing it." : L"."));
    }

    void Refused(HWND h, const wchar_t* why, bool resize) override
    {
        Post(std::wstring(resize ? L"Not resizing " : L"Not moving ") + Describe(h) + L": " + why + L".");
    }

    // To the calling thread's own queue, so the input goes in after the hook returns.
    void MaskModifier() override         { PostThreadMessageW(GetCurrentThreadId(), kMsgMask, 0, 0); }
    void ReplayClick(bool right) override { PostThreadMessageW(GetCurrentThreadId(), kMsgReplay, right, 0); }

private:
    void Post(std::wstring line);

    std::vector<std::wstring> skip_;
    HWND                      notify_;
    DWORD                     integrity_;
    std::wstring              why_;   // backs Refusal()'s text when it is built
    std::unique_ptr<Mover>    mover_;
};

// Lines for the log, on their way from the drag thread to the one that writes it.
SRWLOCK                   g_logLock  = SRWLOCK_INIT;
std::vector<std::wstring> g_logLines;   // guarded by g_logLock

void WindowsDragEnv::Post(std::wstring line)
{
    if (!notify_) return;
    AcquireSRWLockExclusive(&g_logLock);
    // One line a drag is the most there should be; this limit only matters if the
    // tray app stopped taking them.
    if (g_logLines.size() < 256) g_logLines.push_back(std::move(line));
    ReleaseSRWLockExclusive(&g_logLock);
    PostMessageW(notify_, kDragLogMessage, 0, 0);
}

// ------------------------------------------------------------- the hook thread ---

HANDLE g_thread   = nullptr;
DWORD  g_threadId = 0;

// The hook calls back on the thread that set it, so each drag thread has its own: one
// still ending can never reach the tracker of the one that replaced it.
thread_local DragTracker* t_tracker = nullptr;

UINT HeldModifiers()
{
    const auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    UINT m = 0;
    if (down(VK_CONTROL))                m |= MOD_CONTROL;
    if (down(VK_MENU))                   m |= MOD_ALT;
    if (down(VK_SHIFT))                  m |= MOD_SHIFT;
    if (down(VK_LWIN) || down(VK_RWIN))  m |= MOD_WIN;
    return m;
}

void SendMask()
{
    INPUT in[2] = {};
    for (INPUT& i : in)
    {
        i.type           = INPUT_KEYBOARD;
        i.ki.wVk         = kMaskKey;
        i.ki.dwExtraInfo = kOurInput;
    }
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

void SendReplay(bool right)
{
    // The hook sees logical buttons, SendInput takes physical ones: with the buttons
    // swapped, the primary button is the right-hand one, and the secondary the left.
    const bool swapped  = GetSystemMetrics(SM_SWAPBUTTON) != 0;
    const bool physical = right != swapped;   // true: the right-hand button
    INPUT in[2] = {};
    for (INPUT& i : in)
    {
        i.type           = INPUT_MOUSE;
        i.mi.dwExtraInfo = kOurInput;
    }
    in[0].mi.dwFlags = physical ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
    in[1].mi.dwFlags = physical ? MOUSEEVENTF_RIGHTUP   : MOUSEEVENTF_LEFTUP;
    SendInput(2, in, sizeof(INPUT));
}

LRESULT CALLBACK HookProc(int code, WPARAM wp, LPARAM lp)
{
    // Every mouse event on the machine passes through here and waits for it, so it
    // does almost nothing: no I/O, and no call that could wait on another program.
    if (code == HC_ACTION && t_tracker)
    {
        const auto* m = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
        if (!FromPenOrTouch(m->dwExtraInfo))
        {
            const UINT msg   = static_cast<UINT>(wp);
            const bool ours  = (m->flags & LLMHF_INJECTED) != 0 && m->dwExtraInfo == kOurInput;
            const bool press = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN;
            const UINT held  = press ? HeldModifiers() : 0;
            if (t_tracker->OnMouse(msg, m->pt, held, ours)) return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

struct StartInfo
{
    DragOptions opts;
    HANDLE      ready = nullptr;
    DWORD       error = 0;
};

DWORD WINAPI WatchThread(void* p)
{
    auto* start = static_cast<StartInfo*>(p);
    const DragOptions opts = start->opts;   // *start is gone once ready is set

    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);   // the queue posts need

    // The hook runs on this thread, and the pointer waits for it; above-normal
    // priority keeps a busy machine from making it late.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    const std::unique_ptr<DragEnv> env = MakeWindowsDragEnv(opts.skip, opts.notify);
    const SIZE threshold = { GetSystemMetrics(SM_CXDRAG),     GetSystemMetrics(SM_CYDRAG) };
    const SIZE minSize   = { GetSystemMetrics(SM_CXMINTRACK), GetSystemMetrics(SM_CYMINTRACK) };
    DragTracker tracker(*env, opts.moveMods, opts.resizeMods, threshold, minSize);
    t_tracker = &tracker;

    const HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, HookProc, GetModuleHandleW(nullptr), 0);
    start->error = hook ? 0 : GetLastError();
    SetEvent(start->ready);
    if (!hook)
    {
        t_tracker = nullptr;
        return 1;
    }

    // Hook calls arrive while this waits for messages.
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (msg.hwnd == nullptr && msg.message == kMsgMask)   { SendMask();   continue; }
        if (msg.hwnd == nullptr && msg.message == kMsgReplay) { SendReplay(msg.wParam != 0); continue; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnhookWindowsHookEx(hook);
    tracker.Cancel();
    t_tracker = nullptr;
    return 0;
}

} // namespace

std::unique_ptr<DragEnv> MakeWindowsDragEnv(const std::vector<std::wstring>& skip, HWND notify)
{
    return std::make_unique<WindowsDragEnv>(skip, notify);
}

std::vector<std::wstring> DragTakeLog()
{
    std::vector<std::wstring> lines;
    AcquireSRWLockExclusive(&g_logLock);
    lines.swap(g_logLines);
    ReleaseSRWLockExclusive(&g_logLock);
    return lines;
}

bool DragStart(const DragOptions& opts, DWORD& error)
{
    DragStop();
    error = 0;
    if (opts.moveMods == 0 && opts.resizeMods == 0) return true;

    StartInfo start;
    start.opts  = opts;
    start.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!start.ready)
    {
        error = GetLastError();
        return false;
    }

    DWORD        tid = 0;
    const HANDLE t   = CreateThread(nullptr, 0, WatchThread, &start, 0, &tid);
    if (!t)
    {
        error = GetLastError();
        CloseHandle(start.ready);
        return false;
    }

    // Until it says whether the hook went in - or, should it somehow die first, ends.
    const HANDLE both[2] = { start.ready, t };
    const DWORD  which   = WaitForMultipleObjects(2, both, FALSE, INFINITE);
    CloseHandle(start.ready);

    if (which != WAIT_OBJECT_0 || start.error != 0)
    {
        error = start.error ? start.error : ERROR_GEN_FAILURE;
        WaitForSingleObject(t, 5000);
        CloseHandle(t);
        return false;
    }

    g_thread   = t;
    g_threadId = tid;
    return true;
}

void DragStop()
{
    if (!g_thread) return;

    PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
    // The thread never waits on anything, so this is quick; the limit is only a guard.
    WaitForSingleObject(g_thread, 5000);
    CloseHandle(g_thread);
    g_thread   = nullptr;
    g_threadId = 0;
}
