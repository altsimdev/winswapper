#include "drag.h"

#include "swapper.h"

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

// ------------------------------------------------------------- the tracker ---

DragTracker::DragTracker(DragEnv& env, UINT mods, SIZE threshold)
    : env_(env), mods_(mods), threshold_(threshold)
{
}

bool DragTracker::OnMouse(UINT msg, POINT pt, UINT held, bool replay)
{
    if (replay) return false;

    switch (msg)
    {
    case WM_LBUTTONDOWN:
    {
        // A press while the last one is still down means its release went missing -
        // to a secure desktop, say - so that drag ends where it is.
        if (state_ != State::Idle) Cancel();
        if (mods_ == 0 || held != mods_) return false;

        const HWND h = env_.WindowAt(pt);
        if (!h) return false;
        if (const wchar_t* why = env_.Refusal(h))
        {
            env_.Refused(h, why);
            return false;
        }

        target_ = h;
        down_   = pt;
        state_  = State::Pressed;

        // The program never sees this press, so to Windows, Alt or Win would look
        // pressed and released on its own, which opens the menu bar or Start.
        if (mods_ & (MOD_ALT | MOD_WIN)) env_.MaskModifier();
        return true;
    }

    case WM_MOUSEMOVE:
        if (state_ == State::Pressed &&
            (std::abs(pt.x - down_.x) > threshold_.cx || std::abs(pt.y - down_.y) > threshold_.cy))
            Start(pt);
        if (state_ == State::Dragging) Follow(pt);
        return false;   // never taken: the pointer itself has to move

    case WM_LBUTTONUP:
        if (state_ == State::Idle) return false;
        if (state_ == State::Pressed)
        {
            // Not a drag after all, so the program gets its click - late, but whole.
            state_  = State::Idle;
            target_ = nullptr;
            env_.ReplayClick();
            return true;
        }
        Follow(pt);
        Finish();
        return true;   // its press was taken, so its release is too

    default:
        return false;
    }
}

void DragTracker::Start(POINT pt)
{
    env_.Begin(target_);
    from_     = env_.Rect(target_);
    restored_ = env_.Maximized(target_);

    if (restored_)
    {
        // As when dragging a maximized window by its title bar: it returns to its
        // normal size under the pointer, and follows from there.
        const RECT to = RestoredUnderPointer(from_, env_.NormalSize(target_), pt);
        env_.Restore(target_, to);
        origin_ = { to.left, to.top };
        anchor_ = pt;
    }
    else
    {
        // From where the button went down, so the window catches up with the pointer
        // at once instead of trailing it by the threshold.
        origin_ = { from_.left, from_.top };
        anchor_ = down_;
    }
    at_    = origin_;
    state_ = State::Dragging;
}

void DragTracker::Follow(POINT pt)
{
    const POINT to = { origin_.x + (pt.x - anchor_.x), origin_.y + (pt.y - anchor_.y) };
    if (to.x == at_.x && to.y == at_.y) return;
    at_ = to;
    env_.Move(target_, to);
}

void DragTracker::Finish()
{
    env_.End(target_, from_, at_, restored_);
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

// Moves one window through one drag, on a thread of its own. A program slow to move
// its window - or hung - then holds up only that thread: never the hook, and so never
// the pointer. Positions that arrive while one is being applied replace each other, so
// only the newest is applied next and the window never falls behind.
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
    void Finish();   // what is still waiting is applied, then the thread ends

private:
    struct Shared
    {
        CRITICAL_SECTION cs;
        HANDLE           wake     = nullptr;   // auto-reset
        HWND             hwnd     = nullptr;
        bool             activate = false;
        bool             restore  = false;
        bool             move     = false;
        bool             finish   = false;
        RECT             to       = {};
        POINT            at       = {};

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
            const bool  activate = std::exchange(s.activate, false);
            const bool  restore  = std::exchange(s.restore,  false);
            const bool  move     = std::exchange(s.move,     false);
            const bool  finish   = s.finish;
            const RECT  to       = s.to;
            const POINT at       = s.at;
            LeaveCriticalSection(&s.cs);

            if (!activate && !restore && !move)
            {
                if (finish) return 0;
                break;
            }
            // Synchronous: this thread is the one place it is safe to wait on the
            // window, and waiting is what keeps only one move outstanding.
            if (activate) BringToFront(s.hwnd, false);
            if (restore)  Unmaximize(s.hwnd, to, false);
            if (move)     SetWindowPos(s.hwnd, nullptr, at.x, at.y, 0, 0, kMoveFlags);
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
    Queue([&to](Shared& s) { s.restore = true; s.to = to; });
}

void Mover::Move(POINT topLeft)
{
    if (!threaded_)
    {
        SetWindowPos(s_->hwnd, nullptr, topLeft.x, topLeft.y, 0, 0, kMoveFlags | SWP_ASYNCWINDOWPOS);
        return;
    }
    Queue([topLeft](Shared& s) { s.move = true; s.at = topLeft; });
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

    const wchar_t* Refusal(HWND h) override
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

    void End(HWND h, const RECT& from, POINT to, bool restored) override
    {
        mover_.reset();   // finishes it: the mover's thread applies what is waiting
        wchar_t where[64];
        swprintf_s(where, L"(%ld,%ld)", to.x, to.y);
        Post(L"Dragged " + Describe(h) + L" from " + RectStr(from) + L" to " + where +
             (restored ? L", un-maximizing it." : L"."));
    }

    void Refused(HWND h, const wchar_t* why) override
    {
        Post(L"Not dragging " + Describe(h) + L": " + why + L".");
    }

    // To the calling thread's own queue, so the input goes in after the hook returns.
    void MaskModifier() override { PostThreadMessageW(GetCurrentThreadId(), kMsgMask, 0, 0); }
    void ReplayClick() override  { PostThreadMessageW(GetCurrentThreadId(), kMsgReplay, 0, 0); }

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

void SendReplay()
{
    // The hook sees logical buttons, SendInput takes physical ones: with the buttons
    // swapped, the primary button is the right-hand one.
    const bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
    INPUT in[2] = {};
    for (INPUT& i : in)
    {
        i.type           = INPUT_MOUSE;
        i.mi.dwExtraInfo = kOurInput;
    }
    in[0].mi.dwFlags = swapped ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
    in[1].mi.dwFlags = swapped ? MOUSEEVENTF_RIGHTUP   : MOUSEEVENTF_LEFTUP;
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
            const UINT msg  = static_cast<UINT>(wp);
            const bool ours = (m->flags & LLMHF_INJECTED) != 0 && m->dwExtraInfo == kOurInput;
            const UINT held = msg == WM_LBUTTONDOWN ? HeldModifiers() : 0;
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
    const SIZE threshold = { GetSystemMetrics(SM_CXDRAG), GetSystemMetrics(SM_CYDRAG) };
    DragTracker tracker(*env, opts.mods, threshold);
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
        if (msg.hwnd == nullptr && msg.message == kMsgReplay) { SendReplay(); continue; }
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
    if (opts.mods == 0) return true;

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
