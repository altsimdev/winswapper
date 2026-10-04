#include "swapper.h"
#include "log.h"

#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <utility>

// ---------------------------------------------------------------- monitors --

static BOOL CALLBACK MonitorProc(HMONITOR h, HDC, LPRECT, LPARAM lp)
{
    auto* out = reinterpret_cast<std::vector<MonitorRec>*>(lp);

    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(h, &mi))
    {
        MonitorRec r;
        r.handle  = h;
        r.bounds  = mi.rcMonitor;
        r.work    = mi.rcWork;
        r.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        r.device  = mi.szDevice;
        out->push_back(std::move(r));
    }
    return TRUE;
}

std::vector<MonitorRec> EnumerateMonitors()
{
    std::vector<MonitorRec> v;
    EnumDisplayMonitors(nullptr, nullptr, MonitorProc, reinterpret_cast<LPARAM>(&v));

    // Left-to-right, top-to-bottom. Device-name suffixes are not reliable display
    // numbers (this machine reports DISPLAY5 and DISPLAY6), so order by geometry.
    std::sort(v.begin(), v.end(), [](const MonitorRec& a, const MonitorRec& b) {
        if (a.bounds.left != b.bounds.left) return a.bounds.left < b.bounds.left;
        return a.bounds.top < b.bounds.top;
    });
    return v;
}

// -------------------------------------------------------- coordinate spaces --

POINT WorkspaceOffset()
{
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);

    const POINT origin = { 0, 0 };
    HMONITOR pm = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    if (pm && GetMonitorInfoW(pm, &mi))
    {
        POINT p = { mi.rcWork.left - mi.rcMonitor.left, mi.rcWork.top - mi.rcMonitor.top };
        return p;
    }
    const POINT zero = { 0, 0 };
    return zero;
}

RECT WorkspaceToScreen(const RECT& r, POINT off)
{
    RECT o = r;
    OffsetRect(&o, off.x, off.y);
    return o;
}

RECT ScreenToWorkspace(const RECT& r, POINT off)
{
    RECT o = r;
    OffsetRect(&o, -off.x, -off.y);
    return o;
}

// -------------------------------------------------------------- remap math --

RECT RemapRect(const RECT& r, const RECT& srcWork, const RECT& dstWork, const RECT& dstBounds)
{
    const double sw = static_cast<double>(srcWork.right  - srcWork.left);
    const double sh = static_cast<double>(srcWork.bottom - srcWork.top);
    const double dw = static_cast<double>(dstWork.right  - dstWork.left);
    const double dh = static_cast<double>(dstWork.bottom - dstWork.top);

    if (sw <= 0.0 || sh <= 0.0) return r;

    const double fx = dw / sw;
    const double fy = dh / sh;

    RECT out;
    out.left   = dstWork.left + static_cast<LONG>(std::lround((r.left - srcWork.left) * fx));
    out.top    = dstWork.top  + static_cast<LONG>(std::lround((r.top  - srcWork.top ) * fy));
    out.right  = out.left + static_cast<LONG>(std::lround((r.right  - r.left) * fx));
    out.bottom = out.top  + static_cast<LONG>(std::lround((r.bottom - r.top ) * fy));

    // Rescue only a window that would land entirely off the destination display.
    // Nudging merely-overhanging windows back on-screen would fight the invisible
    // resize border GetWindowRect reports on Win10/11, and would quietly make each
    // swap lossy instead of reversible.
    RECT probe;
    if (!IntersectRect(&probe, &out, &dstBounds))
    {
        const LONG w = out.right - out.left;
        const LONG h = out.bottom - out.top;
        out.left   = dstWork.left;
        out.top    = dstWork.top;
        out.right  = out.left + w;
        out.bottom = out.top  + h;
    }
    return out;
}

// ------------------------------------------------------- window classifying --

static std::wstring ClassOf(HWND h)
{
    wchar_t buf[256] = {};
    GetClassNameW(h, buf, _countof(buf));
    return buf;
}

static std::wstring TitleOf(HWND h)
{
    wchar_t buf[256] = {};
    GetWindowTextW(h, buf, _countof(buf));
    return buf;
}

static bool IsCloaked(HWND hwnd)
{
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))))
        return cloaked != 0;
    return false;
}

static bool IsShellClass(const std::wstring& c)
{
    static const wchar_t* const kBlocked[] = {
        L"Progman",
        L"WorkerW",
        L"Shell_TrayWnd",
        L"Shell_SecondaryTrayWnd",
        L"Windows.UI.Core.CoreWindow",
        L"ApplicationManager_DesktopShellWindow",
        L"ForegroundStaging",
        L"TaskListThumbnailWnd",
        L"MultitaskingViewFrame",
        L"XamlExplorerHostIslandWindow",
    };
    for (const wchar_t* b : kBlocked)
        if (c == b) return true;
    return false;
}

static const wchar_t* StateName(WinState s)
{
    switch (s)
    {
    case WinState::Maximized: return L"maximized";
    case WinState::Minimized: return L"minimized";
    default:                  return L"normal";
    }
}

static std::wstring RectStr(const RECT& r)
{
    wchar_t b[96];
    swprintf_s(b, L"(%ld,%ld %ldx%ld)", r.left, r.top, r.right - r.left, r.bottom - r.top);
    return b;
}

// Cheap rejections first. Returns nullptr when the window is a swap candidate.
static const wchar_t* Classify(HWND hwnd, HWND self, const std::wstring& cls)
{
    if (hwnd == self)                          return L"self";
    if (!IsWindowVisible(hwnd))                return L"not visible";
    if (GetWindow(hwnd, GW_OWNER) != nullptr)  return L"owned window";

    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW)                 return L"tool window";

    // The filter that matters most: without it we would haul suspended UWP apps and,
    // worse, windows living on other virtual desktops onto the current one.
    if (IsCloaked(hwnd))                       return L"cloaked (other desktop / suspended)";

    if (hwnd == GetShellWindow())              return L"shell window";
    if (IsShellClass(cls))                     return L"shell class";

    return nullptr;
}

// ------------------------------------------------------------- collecting ---

int DestSlot(int slot, int count, Rotation dir)
{
    if (count < 2 || slot < 0 || slot >= count) return -1;

    // The + count before the modulo keeps a left rotation off C++'s
    // implementation-defined negative remainder.
    const int step = (dir == Rotation::Left) ? -1 : 1;
    return ((slot + step) % count + count) % count;
}

static const wchar_t* DirName(Rotation dir)
{
    return dir == Rotation::Left ? L"left" : L"right";
}

static int SlotForMonitor(const std::vector<MonitorRec>& mons, HMONITOR h)
{
    for (size_t i = 0; i < mons.size(); ++i)
        if (mons[i].handle == h) return static_cast<int>(i);
    return -1;
}

struct CollectCtx
{
    HWND                           self     = nullptr;
    const std::vector<MonitorRec>* mons     = nullptr;
    Rotation                       dir      = Rotation::Left;
    POINT                          off      = { 0, 0 };
    std::vector<WindowRec>*        included = nullptr;

    // Only populated for --list.
    bool                                               wantExcluded = false;
    std::vector<std::pair<std::wstring, std::wstring>>* excluded    = nullptr;
};

static BOOL CALLBACK CollectProc(HWND hwnd, LPARAM lp)
{
    auto* ctx = reinterpret_cast<CollectCtx*>(lp);

    const std::wstring cls = ClassOf(hwnd);

    auto reject = [&](const wchar_t* why) {
        if (ctx->wantExcluded && ctx->excluded)
        {
            std::wstring who = TitleOf(hwnd);
            if (who.empty()) who = L"(no title)";
            who += L"  [" + cls + L"]";
            ctx->excluded->emplace_back(std::move(who), why);
        }
    };

    if (const wchar_t* why = Classify(hwnd, ctx->self, cls)) { reject(why); return TRUE; }

    WindowRec w;
    w.hwnd      = hwnd;
    w.cls       = cls;
    w.title     = TitleOf(hwnd);
    w.wp.length = sizeof(w.wp);

    if (!GetWindowPlacement(hwnd, &w.wp)) { reject(L"no placement"); return TRUE; }
    if (!GetWindowRect(hwnd, &w.rect))    { reject(L"no rect");      return TRUE; }

    if      (w.wp.showCmd == SW_SHOWMINIMIZED) w.state = WinState::Minimized;
    else if (w.wp.showCmd == SW_SHOWMAXIMIZED) w.state = WinState::Maximized;
    else                                       w.state = WinState::Normal;

    // A minimized window's GetWindowRect is parked at (-32000,-32000), which would
    // assign every minimized window to the leftmost display. Use its restore rect
    // instead to decide where it really lives.
    RECT effective;
    if (w.state == WinState::Minimized)
    {
        effective = WorkspaceToScreen(w.wp.rcNormalPosition, ctx->off);
    }
    else
    {
        if (w.rect.right <= w.rect.left || w.rect.bottom <= w.rect.top)
        {
            reject(L"empty rect");
            return TRUE;
        }
        effective = w.rect;
    }

    const int count = static_cast<int>(ctx->mons->size());
    HMONITOR  mon   = MonitorFromRect(&effective, MONITOR_DEFAULTTONEAREST);
    const int slot  = SlotForMonitor(*ctx->mons, mon);
    const int dest  = DestSlot(slot, count, ctx->dir);
    if (slot < 0 || dest < 0) { reject(L"on an unrecognized display"); return TRUE; }

    const MonitorRec& src = (*ctx->mons)[static_cast<size_t>(slot)];
    const MonitorRec& dst = (*ctx->mons)[static_cast<size_t>(dest)];

    // Exclusive-fullscreen signature: fills the monitor exactly and has neither a
    // caption nor a sizing frame. Games react badly to being moved; borderless
    // "maximized" app windows keep at least one of those styles and are unaffected.
    if (w.state == WinState::Normal && EqualRect(&w.rect, &src.bounds))
    {
        const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        if (!(style & WS_CAPTION) && !(style & WS_THICKFRAME))
        {
            reject(L"exclusive fullscreen");
            return TRUE;
        }
    }

    w.srcSlot = slot;
    w.dstSlot = dest;

    if (w.state == WinState::Normal)
    {
        w.targetRect   = RemapRect(w.rect, src.work, dst.work, dst.bounds);
        w.targetNormal = ScreenToWorkspace(w.targetRect, ctx->off);
    }
    else
    {
        // Maximized and minimized windows are both relocated by rewriting their
        // restore rect; Windows derives the maximize/restore display from it.
        const RECT normScreen = WorkspaceToScreen(w.wp.rcNormalPosition, ctx->off);
        const RECT remapped   = RemapRect(normScreen, src.work, dst.work, dst.bounds);
        w.targetNormal = ScreenToWorkspace(remapped, ctx->off);
        w.targetRect   = remapped;
    }

    ctx->included->push_back(std::move(w));
    return TRUE;
}

// --------------------------------------------------------------- applying ---

bool ApplyOne(const WindowRec& w)
{
    if (w.state == WinState::Normal)
    {
        // SWP_ASYNCWINDOWPOS keeps a hung application from stalling the whole swap.
        return SetWindowPos(w.hwnd, nullptr,
                            w.targetRect.left, w.targetRect.top,
                            w.targetRect.right  - w.targetRect.left,
                            w.targetRect.bottom - w.targetRect.top,
                            SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS) != FALSE;
    }

    if (w.state == WinState::Maximized)
    {
        // SetWindowPlacement on its own will NOT relocate a maximized window: Windows
        // leaves it on its current display and merely rewrites the restore rect.
        // (The self-test catches this.) So un-maximize, move, then re-maximize, which
        // lands it on whichever display it now occupies.
        //
        // The move must be synchronous - SWP_ASYNCWINDOWPOS would let the maximize
        // run before the window had actually gone anywhere.
        //
        // SW_MAXIMIZE also activates the window, which drags it to the front. That
        // is repaired once every window has moved, in RestoreStacking().
        ShowWindow(w.hwnd, SW_SHOWNOACTIVATE);

        const BOOL moved = SetWindowPos(w.hwnd, nullptr,
                                        w.targetRect.left, w.targetRect.top,
                                        w.targetRect.right  - w.targetRect.left,
                                        w.targetRect.bottom - w.targetRect.top,
                                        SWP_NOZORDER | SWP_NOACTIVATE);

        ShowWindow(w.hwnd, SW_MAXIMIZE);
        return moved != FALSE;
    }

    WINDOWPLACEMENT wp = w.wp;
    wp.length           = sizeof(wp);
    wp.rcNormalPosition = w.targetNormal;

    // Keep WPF_RESTORETOMAXIMIZED (so a window minimized from maximized restores
    // maximized) but drop WPF_SETMINPOSITION, whose stale ptMinPosition we do not
    // want re-applied. showCmd is left untouched, so the window keeps its state.
    wp.flags &= WPF_RESTORETOMAXIMIZED;

    return SetWindowPlacement(w.hwnd, &wp) != FALSE;
}

static void RestoreStacking(const std::vector<WindowRec>& plan, const std::vector<bool>& applied,
                            HWND foreground, HWND self)
{
    // Re-maximizing activates a window and activation brings it to the front, so
    // applying a plan top-to-bottom leaves its maximized windows stacked in exactly
    // the reverse of how they started, all in front of normal windows that used to
    // cover them. The self-test reproduces this with real windows.
    //
    // Normal windows are moved with SWP_NOZORDER and never leave their place, which
    // makes them reliable anchors: every window moved by a path that can disturb
    // the stacking goes back directly beneath the plan window that was above it.
    // Working top to bottom means that window has always been put back already.
    //
    // Only maximized and minimized windows are re-stacked, and not merely because
    // they are the ones disturbed. SetWindowPos here is synchronous, so it waits on
    // the window's thread - but those windows have just had synchronous calls from
    // ApplyOne anyway, so a hung application cannot stall the rotation any more
    // than it already could. Re-stacking normal windows too would throw away the
    // protection SWP_ASYNCWINDOWPOS gives them.
    //
    // Topmost and ordinary windows are two separate stacks. Placing a window under
    // one from the other stack would move it across - promoting an ordinary window
    // to always-on-top, or demoting a topmost one - so each window is anchored only
    // to its own kind.
    const auto topmost = [](HWND h) {
        return (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    };

    for (size_t i = 0; i < plan.size(); ++i)
    {
        if (!applied[i] || plan[i].state == WinState::Normal) continue;

        const bool band  = topmost(plan[i].hwnd);
        HWND       above = nullptr;
        for (size_t j = i; j-- > 0; )
        {
            if (topmost(plan[j].hwnd) == band) { above = plan[j].hwnd; break; }
        }

        // No anchor means it was the front window of its stack, which is exactly
        // where activation left it.
        if (above)
            SetWindowPos(plan[i].hwnd, above, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    // Activation also handed the focus to whichever window was re-maximized last.
    // Give it back. Our own hidden window is skipped: it holds the focus only while
    // the tray menu is open, and there is nothing to hand back in that case.
    if (foreground && foreground != self && IsWindow(foreground) &&
        GetForegroundWindow() != foreground)
        SetForegroundWindow(foreground);
}

std::vector<bool> ApplyPlan(const std::vector<WindowRec>& plan, HWND self)
{
    // Taken before anything moves, because re-maximizing hands the focus away.
    const HWND foreground = GetForegroundWindow();

    std::vector<bool> applied(plan.size(), false);
    for (size_t i = 0; i < plan.size(); ++i)
    {
        const WindowRec& w   = plan[i];
        const bool       ok  = ApplyOne(w);
        const DWORD      err = GetLastError();
        applied[i] = ok;
        if (!ok)
            LogF(L"  FAILED (err %lu) \"%s\" [%s]", err, w.title.c_str(), w.cls.c_str());
    }

    RestoreStacking(plan, applied, foreground, self);
    return applied;
}

// ------------------------------------------------------------- operations ---

static bool Prepare(HWND self, std::vector<MonitorRec>& mons, Rotation dir,
                    std::vector<WindowRec>& plan,
                    std::vector<std::pair<std::wstring, std::wstring>>* excluded)
{
    mons = EnumerateMonitors();
    if (mons.size() < 2) return false;

    CollectCtx ctx;
    ctx.self         = self;
    ctx.mons         = &mons;
    ctx.dir          = dir;
    ctx.off          = WorkspaceOffset();
    ctx.included     = &plan;
    ctx.excluded     = excluded;
    ctx.wantExcluded = excluded != nullptr;

    EnumWindows(CollectProc, reinterpret_cast<LPARAM>(&ctx));
    return true;
}

static void LogMonitors(const std::vector<MonitorRec>& mons)
{
    const int count = static_cast<int>(mons.size());

    LogF(L"Displays: %d (ordered left to right)", count);
    for (int i = 0; i < count; ++i)
    {
        const MonitorRec& m     = mons[static_cast<size_t>(i)];
        const int         left  = DestSlot(i, count, Rotation::Left);
        const int         right = DestSlot(i, count, Rotation::Right);

        // Both destinations, because which one applies depends on the hotkey.
        wchar_t role[48] = L"";
        if (left >= 0 && right >= 0)
            swprintf_s(role, L"left -> [%d], right -> [%d]", left, right);

        LogF(L"  [%d] %s%s bounds=%s work=%s %s",
             i, m.device.c_str(), m.primary ? L" (primary)" : L"",
             RectStr(m.bounds).c_str(), RectStr(m.work).c_str(), role);
    }

    if (count == 2)
        LogF(L"  note: with two displays both directions are the same swap.");
    else if (count > 2)
        LogF(L"  note: %d displays; left wraps [0] around to [%d] and right wraps "
             L"[%d] back to [0]. One of each cancels out, or %d the same way.",
             count, count - 1, count - 1, count);
}

static void LogPlanLine(const WindowRec& w, POINT off)
{
    const RECT from = (w.state == WinState::Normal)
                    ? w.rect
                    : WorkspaceToScreen(w.wp.rcNormalPosition, off);

    LogF(L"  %-9s [%d->%d] %s -> %s  \"%s\"  [%s]",
         StateName(w.state), w.srcSlot, w.dstSlot,
         RectStr(from).c_str(),
         RectStr(w.targetRect).c_str(),
         w.title.c_str(), w.cls.c_str());
}

SwapResult PerformSwap(HWND self, bool dryRun, Rotation dir)
{
    SwapResult res;

    std::vector<MonitorRec> mons;
    std::vector<WindowRec>  plan;

    if (!Prepare(self, mons, dir, plan, nullptr))
    {
        res.monitors = static_cast<int>(mons.size());
        LogF(L"Only %d display(s) detected - nothing to rotate.", res.monitors);
        return res;
    }

    const POINT off = WorkspaceOffset();

    res.monitors   = static_cast<int>(mons.size());
    res.considered = static_cast<int>(plan.size());

    LogF(L"---- %s, rotating %s ----", dryRun ? L"dry run" : L"rotate", DirName(dir));
    LogMonitors(mons);
    LogF(L"Windows to move: %d", res.considered);

    for (const WindowRec& w : plan)
        LogPlanLine(w, off);

    if (dryRun)
    {
        LogF(L"Dry run - nothing was moved.");
        return res;
    }

    for (const bool ok : ApplyPlan(plan, self))
    {
        if (ok) ++res.moved;
        else    ++res.failed;
    }

    LogF(L"Moved %d of %d window(s), %d failed.", res.moved, res.considered, res.failed);
    return res;
}

void ListAll(HWND self)
{
    std::vector<MonitorRec> mons;
    std::vector<WindowRec>  plan;
    std::vector<std::pair<std::wstring, std::wstring>> excluded;

    // Listing is direction-agnostic: the include/exclude decision and every
    // window's source display are the same either way. Only the destination
    // column differs, and the display table above shows both.
    const bool ok = Prepare(self, mons, Rotation::Left, plan, &excluded);

    LogF(L"---- list ----");
    LogMonitors(mons);

    if (!ok)
    {
        LogF(L"Fewer than two displays; window list not computed.");
        return;
    }

    const POINT off = WorkspaceOffset();

    LogF(L"");
    LogF(L"Included (%zu) - destinations shown for a left rotation:", plan.size());
    for (const WindowRec& w : plan)
        LogPlanLine(w, off);

    LogF(L"");
    LogF(L"Excluded (%zu):", excluded.size());
    for (const auto& e : excluded)
        LogF(L"  %-40s  %s", e.second.c_str(), e.first.c_str());
}

// --------------------------------------------------------------- self test --

struct ZRankCtx
{
    const std::vector<HWND>* wnds  = nullptr;
    std::vector<int>*        ranks = nullptr;
    int                      next  = 0;
};

static BOOL CALLBACK ZRankProc(HWND h, LPARAM lp)
{
    auto* c = reinterpret_cast<ZRankCtx*>(lp);
    for (size_t i = 0; i < c->wnds->size(); ++i)
        if ((*c->wnds)[i] == h) (*c->ranks)[i] = c->next;
    ++c->next;
    return TRUE;
}

// Where each window sits in the current top-to-bottom stacking order (lower is
// nearer the front), or -1 if it is no longer a top-level window.
static std::vector<int> ZRanks(const std::vector<HWND>& wnds)
{
    std::vector<int> ranks(wnds.size(), -1);
    ZRankCtx ctx;
    ctx.wnds  = &wnds;
    ctx.ranks = &ranks;
    EnumWindows(ZRankProc, reinterpret_cast<LPARAM>(&ctx));
    return ranks;
}

static void PumpFor(DWORD ms)
{
    const ULONGLONG end = GetTickCount64() + ms;
    MSG msg;
    while (GetTickCount64() < end)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
}

int SelfTest()
{
    int failures = 0;
    auto check = [&](bool cond, const wchar_t* what) {
        LogF(L"  %s  %s", cond ? L"PASS" : L"FAIL", what);
        if (!cond) ++failures;
    };

    std::vector<MonitorRec> mons = EnumerateMonitors();
    LogF(L"---- self test ----");
    LogMonitors(mons);

    // The mapping checks below are pure arithmetic and run on any machine, including
    // a one-display laptop and a CI runner. Only the remap and window-move tests
    // further down need a second display, so the bail-out sits after them, not here
    // - otherwise the part most likely to carry a bug would go untested exactly
    // where it is easiest to run.

    // 0. The rotation itself, checked for display counts this machine may not have.
    //    Each direction must rotate the right way and, just as importantly, be a
    //    permutation - if two displays ever shared a destination the rotation would
    //    not be reversible.
    const Rotation DIRS[2]      = { Rotation::Left, Rotation::Right };
    const int      STEP[2]      = { -1, 1 };

    LogF(L"Rotation mapping:");
    for (int n = 2; n <= 5; ++n)
    {
        for (int d = 0; d < 2; ++d)
        {
            const Rotation dir = DIRS[d];
            std::wstring shown;
            std::vector<int> hits(static_cast<size_t>(n), 0);
            bool good = true;

            for (int i = 0; i < n; ++i)
            {
                const int to = DestSlot(i, n, dir);
                wchar_t part[32];
                swprintf_s(part, L"%d->%d ", i, to);
                shown += part;

                if (to != ((i + STEP[d]) % n + n) % n) good = false;
                else                                   ++hits[static_cast<size_t>(to)];
            }
            for (int h : hits) if (h != 1) good = false;

            // n rotations the same way must land every display back on itself.
            for (int i = 0; i < n; ++i)
            {
                int at = i;
                for (int k = 0; k < n; ++k) at = DestSlot(at, n, dir);
                if (at != i) good = false;
            }

            wchar_t msg[256];
            swprintf_s(msg, L"%d displays %-5s: %s(cycle of %d)",
                       n, DirName(dir), shown.c_str(), n);
            check(good, msg);
        }

        // The whole point of the second hotkey: one of each cancels out, both ways
        // round. Without this a sign error would only show up on a real desktop.
        bool inverse = true;
        for (int i = 0; i < n; ++i)
        {
            if (DestSlot(DestSlot(i, n, Rotation::Left),  n, Rotation::Right) != i) inverse = false;
            if (DestSlot(DestSlot(i, n, Rotation::Right), n, Rotation::Left)  != i) inverse = false;
        }
        wchar_t msg[128];
        swprintf_s(msg, L"%d displays: left then right is the identity, and so is right then left", n);
        check(inverse, msg);
    }

    check(DestSlot(0, 2, Rotation::Left)  == 1 && DestSlot(1, 2, Rotation::Left)  == 0 &&
          DestSlot(0, 2, Rotation::Right) == 1 && DestSlot(1, 2, Rotation::Right) == 0,
          L"two displays reduce to the same plain swap in both directions");
    check(DestSlot(-1, 3, Rotation::Left)  < 0 && DestSlot(3, 3, Rotation::Left)  < 0 &&
          DestSlot(0, 1, Rotation::Left)   < 0 && DestSlot(-1, 3, Rotation::Right) < 0 &&
          DestSlot(3, 3, Rotation::Right)  < 0 && DestSlot(0, 1, Rotation::Right)  < 0,
          L"out-of-range slots and single-display setups are rejected either way");

    // Everything from here needs somewhere to move a window to.
    if (mons.size() < 2)
    {
        LogF(L"");
        LogF(L"SKIPPED: the remap and window-move tests need a second display.");
        LogF(L"Self test: %s (%d failure(s); window tests skipped)",
             failures == 0 ? L"PASS" : L"FAIL", failures);
        // 3, not 2: exit 2 already means a usage error, and a CI step that tolerates
        // "partial pass" must not also swallow a mistyped argument.
        return failures == 0 ? 3 : 1;
    }

    // The window tests below follow the real rotation, so with three or more
    // displays they exercise the wrap-around rather than a made-up pair.
    const MonitorRec& A = mons[0];
    const MonitorRec& B = mons[static_cast<size_t>(
        DestSlot(0, static_cast<int>(mons.size()), Rotation::Left))];
    const POINT off = WorkspaceOffset();

    LogF(L"Moving test windows from [0] %s to [%d] %s",
         A.device.c_str(), DestSlot(0, static_cast<int>(mons.size()), Rotation::Left),
         B.device.c_str());

    // 1. Pure math: a round trip through both work areas must be the identity.
    // A round trip between two work areas is pixel-exact only when they are the same
    // size: then every scale factor is 1.0 and the remap is a pure translation. When
    // they differ, the leg that scales down rounds away information the leg that
    // scales up cannot restore, so each edge may come back off by up to the size
    // ratio, rounded up. Asserting exact equality there would fail correct code.
    const auto roundTripSlack = [](const RECT& a, const RECT& b) -> LONG {
        const double aw = a.right - a.left, ah = a.bottom - a.top;
        const double bw = b.right - b.left, bh = b.bottom - b.top;
        if (aw == bw && ah == bh) return 0;
        const double rx = aw > bw ? aw / bw : bw / aw;
        const double ry = ah > bh ? ah / bh : bh / ah;
        return static_cast<LONG>(std::ceil(rx > ry ? rx : ry));
    };
    const auto within = [](const RECT& p, const RECT& q, LONG slack) {
        return std::abs(p.left  - q.left)  <= slack && std::abs(p.top    - q.top)    <= slack &&
               std::abs(p.right - q.right) <= slack && std::abs(p.bottom - q.bottom) <= slack;
    };
    const LONG slack = roundTripSlack(A.work, B.work);

    LogF(L"Remap round-trip (%s):",
         slack == 0 ? L"same-size work areas, must be pixel-exact"
                    : L"work areas differ in size, rounding slack allowed");
    const RECT samples[] = {
        { A.work.left + 10,  A.work.top + 10,  A.work.left + 410,  A.work.top + 310  },
        { A.work.left - 7,   A.work.top + 0,   A.work.left + 953,  A.work.top + 1032 },
        { A.work.left + 500, A.work.top + 200, A.work.left + 1900, A.work.top + 1000 },
    };
    for (const RECT& s : samples)
    {
        const RECT there = RemapRect(s,     A.work, B.work, B.bounds);
        const RECT back  = RemapRect(there, B.work, A.work, A.bounds);
        wchar_t msg[256];
        swprintf_s(msg, L"%s -> %s -> %s (slack %ld px)",
                   RectStr(s).c_str(), RectStr(there).c_str(), RectStr(back).c_str(), slack);
        check(within(back, s, slack), msg);
    }

    // 2. Real windows: the paths that actually move things.
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WinSwapperSelfTestWnd";
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW + 1));
    RegisterClassExW(&wc);

    HWND wnd[3] = {};
    for (int i = 0; i < 3; ++i)
    {
        wnd[i] = CreateWindowExW(0, wc.lpszClassName, L"WinSwapper self test",
                                 WS_OVERLAPPEDWINDOW,
                                 A.work.left + 80 + i * 40, A.work.top + 80 + i * 40,
                                 420, 300, nullptr, nullptr, wc.hInstance, nullptr);
        if (!wnd[i])
        {
            LogF(L"  FAIL could not create test window %d", i);
            return 1;
        }
        ShowWindow(wnd[i], SW_SHOWNORMAL);
    }
    PumpFor(250);
    ShowWindow(wnd[1], SW_MAXIMIZE);
    ShowWindow(wnd[2], SW_MINIMIZE);
    PumpFor(400);

    WindowRec     rec[3];
    const WinState states[3] = { WinState::Normal, WinState::Maximized, WinState::Minimized };

    for (int i = 0; i < 3; ++i)
    {
        rec[i].hwnd      = wnd[i];
        rec[i].state     = states[i];
        rec[i].wp.length = sizeof(rec[i].wp);
        GetWindowPlacement(wnd[i], &rec[i].wp);
        GetWindowRect(wnd[i], &rec[i].rect);

        if (rec[i].state == WinState::Normal)
        {
            rec[i].targetRect   = RemapRect(rec[i].rect, A.work, B.work, B.bounds);
            rec[i].targetNormal = ScreenToWorkspace(rec[i].targetRect, off);
        }
        else
        {
            const RECT ns = WorkspaceToScreen(rec[i].wp.rcNormalPosition, off);
            const RECT rm = RemapRect(ns, A.work, B.work, B.bounds);
            rec[i].targetNormal = ScreenToWorkspace(rm, off);
            rec[i].targetRect   = rm;
        }

        ApplyOne(rec[i]);
    }
    PumpFor(500);

    LogF(L"Window moves:");
    {
        RECT got = {};
        GetWindowRect(wnd[0], &got);
        wchar_t msg[256];
        swprintf_s(msg, L"normal window landed at %s (wanted %s)",
                   RectStr(got).c_str(), RectStr(rec[0].targetRect).c_str());
        check(EqualRect(&got, &rec[0].targetRect) != FALSE, msg);
    }
    {
        const HMONITOR m   = MonitorFromWindow(wnd[1], MONITOR_DEFAULTTONEAREST);
        const bool     zoom = IsZoomed(wnd[1]) != FALSE;
        RECT got = {};
        GetWindowRect(wnd[1], &got);

        wchar_t msg[256];
        swprintf_s(msg, L"maximized window: zoomed=%s, on %s, rect %s",
                   zoom ? L"yes" : L"NO",
                   m == B.handle ? L"the target display"
                                 : (m == A.handle ? L"the SOURCE display" : L"an unknown display"),
                   RectStr(got).c_str());
        check(m == B.handle && zoom, msg);
    }
    {
        WINDOWPLACEMENT wp = {};
        wp.length = sizeof(wp);
        GetWindowPlacement(wnd[2], &wp);
        RECT ns = WorkspaceToScreen(wp.rcNormalPosition, off);
        const HMONITOR m = MonitorFromRect(&ns, MONITOR_DEFAULTTONEAREST);
        check(IsIconic(wnd[2]) != FALSE && m == B.handle,
              L"minimized window stayed minimized and will restore on the other display");
    }

    // The promise the second hotkey makes, tested on a real window rather than on
    // the arithmetic: wnd[0] has already rotated A -> B, so rotating it back B -> A
    // must return it to where it started - exactly when A and B share a size, within
    // rounding slack otherwise. This covers the reverse remap and ApplyOne together.
    //
    // It deliberately does NOT cover two things, which are tested elsewhere - so do
    // not delete those on the strength of this one:
    //  - direction: B -> A is fixed here, not derived from DestSlot. Whether Right
    //    really is Left's inverse is checked by the rotation-mapping tests above.
    //  - clamping: this window sits wholly inside both work areas, so a clamp would
    //    never touch it. samples[1] in the remap round-trip overhangs on purpose.
    {
        const RECT before = rec[0].rect;

        WindowRec back;
        back.hwnd  = wnd[0];
        back.state = WinState::Normal;
        GetWindowRect(wnd[0], &back.rect);
        back.targetRect   = RemapRect(back.rect, B.work, A.work, A.bounds);
        back.targetNormal = ScreenToWorkspace(back.targetRect, off);
        ApplyOne(back);
        PumpFor(400);

        RECT got = {};
        GetWindowRect(wnd[0], &got);
        wchar_t msg[256];
        swprintf_s(msg, L"normal window rotated back to %s (started at %s, slack %ld px)",
                   RectStr(got).c_str(), RectStr(before).c_str(), slack);
        check(within(got, before, slack), msg);
    }

    for (HWND h : wnd) DestroyWindow(h);
    PumpFor(100);

    // 3. Stacking order. Re-maximizing activates a window, and activation brings it
    //    to the front, so a plan applied top-to-bottom used to leave two maximized
    //    windows on one display in reverse order - both now in front of a normal
    //    window that had been covering them. Three windows on A, stacked normal over
    //    maximized over maximized, must arrive on B stacked the same way.
    LogF(L"Stacking order:");
    {
        HWND st[3] = {};
        const wchar_t* names[3] = { L"WinSwapper stack test: normal (top)",
                                    L"WinSwapper stack test: maximized (middle)",
                                    L"WinSwapper stack test: maximized (bottom)" };
        bool created = true;
        for (int i = 0; i < 3; ++i)
        {
            st[i] = CreateWindowExW(0, wc.lpszClassName, names[i], WS_OVERLAPPEDWINDOW,
                                    A.work.left + 120 + i * 40, A.work.top + 120 + i * 40,
                                    420, 300, nullptr, nullptr, wc.hInstance, nullptr);
            if (!st[i]) { created = false; break; }
            ShowWindow(st[i], SW_SHOWNORMAL);
        }
        check(created, L"created the three stacking test windows");

        if (created)
        {
            ShowWindow(st[1], SW_MAXIMIZE);
            ShowWindow(st[2], SW_MAXIMIZE);

            // Stack them explicitly, bottom first, so the starting order is known
            // rather than an accident of creation and activation.
            for (int i = 2; i >= 0; --i)
                SetWindowPos(st[i], HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            PumpFor(400);

            const std::vector<HWND> order(st, st + 3);
            const std::vector<int>  before = ZRanks(order);
            check(before[0] >= 0 && before[0] < before[1] && before[1] < before[2],
                  L"precondition: EnumWindows reports the test windows top to bottom");

            // Built in the same top-to-bottom order Prepare() gets from EnumWindows,
            // and applied through ApplyPlan() exactly as PerformSwap() applies one -
            // so dropping the re-stacking from that path would fail this check.
            std::vector<WindowRec> plan(3);
            for (size_t i = 0; i < plan.size(); ++i)
            {
                WindowRec& r = plan[i];
                r.hwnd      = st[i];
                r.state     = (i == 0) ? WinState::Normal : WinState::Maximized;
                r.wp.length = sizeof(r.wp);
                GetWindowPlacement(st[i], &r.wp);
                GetWindowRect(st[i], &r.rect);

                const RECT from = (r.state == WinState::Normal)
                                ? r.rect
                                : WorkspaceToScreen(r.wp.rcNormalPosition, off);
                r.targetRect   = RemapRect(from, A.work, B.work, B.bounds);
                r.targetNormal = ScreenToWorkspace(r.targetRect, off);
            }
            ApplyPlan(plan, nullptr);
            PumpFor(500);

            const std::vector<int> after = ZRanks(order);
            wchar_t msg[256];
            swprintf_s(msg, L"normal over maximized over maximized after moving "
                            L"(z-ranks before %d,%d,%d; after %d,%d,%d)",
                       before[0], before[1], before[2], after[0], after[1], after[2]);
            check(after[0] >= 0 && after[0] < after[1] && after[1] < after[2], msg);

            bool allOnB = true;
            for (HWND h : st)
                if (MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST) != B.handle) allOnB = false;
            check(allOnB && IsZoomed(st[1]) != FALSE && IsZoomed(st[2]) != FALSE,
                  L"all three reached the target display, the maximized two still maximized");
        }

        for (HWND h : st) if (h) DestroyWindow(h);
        PumpFor(100);
    }

    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    LogF(L"Self test: %s (%d failure(s))", failures == 0 ? L"PASS" : L"FAIL", failures);
    return failures == 0 ? 0 : 1;
}
