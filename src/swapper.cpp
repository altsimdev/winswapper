#include "swapper.h"
#include "log.h"

#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
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

std::wstring RectStr(const RECT& r)
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

std::wstring ProgramName(DWORD pid)
{
    // PROCESS_QUERY_LIMITED_INFORMATION is all the image name needs and, unlike
    // fuller access, is usually granted even for elevated processes.
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return std::wstring();

    wchar_t path[MAX_PATH * 4] = {};
    DWORD   len = _countof(path);
    const BOOL ok = QueryFullProcessImageNameW(p, 0, path, &len);
    CloseHandle(p);
    if (!ok) return std::wstring();

    const wchar_t* name = wcsrchr(path, L'\\');
    return name ? name + 1 : path;
}

std::wstring IgnoredBy(HWND hwnd, const std::vector<std::wstring>& ignore)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    const std::wstring program = ProgramName(pid);
    if (program.empty()) return std::wstring();

    for (const std::wstring& entry : ignore)
        if (CompareStringOrdinal(entry.c_str(), -1, program.c_str(), -1, TRUE) == CSTR_EQUAL)
            return entry;
    return std::wstring();
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

const wchar_t* DirName(Rotation dir)
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
    const std::vector<std::wstring>* ignore = nullptr;   // program names from settings

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

    // After the cheaper checks, so a hidden or tool window still reports that more
    // basic reason. Naming the matching entry lets --list show which line did it.
    if (ctx->ignore && !ctx->ignore->empty())
    {
        const std::wstring entry = IgnoredBy(hwnd, *ctx->ignore);
        if (!entry.empty())
        {
            const std::wstring why = L"ignored in settings (" + entry + L")";
            reject(why.c_str());
            return TRUE;
        }
    }

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
                    const std::vector<std::wstring>& ignore,
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
    ctx.ignore       = &ignore;
    ctx.excluded     = excluded;
    ctx.wantExcluded = excluded != nullptr;

    EnumWindows(CollectProc, reinterpret_cast<LPARAM>(&ctx));
    return true;
}

void LogMonitors(const std::vector<MonitorRec>& mons)
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

SwapResult PerformSwap(HWND self, bool dryRun, Rotation dir, const std::vector<std::wstring>& ignore)
{
    SwapResult res;

    std::vector<MonitorRec> mons;
    std::vector<WindowRec>  plan;

    if (!Prepare(self, mons, dir, ignore, plan, nullptr))
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

void ListAll(HWND self, const std::vector<std::wstring>& ignore)
{
    std::vector<MonitorRec> mons;
    std::vector<WindowRec>  plan;
    std::vector<std::pair<std::wstring, std::wstring>> excluded;

    // Listing is direction-agnostic: the include/exclude decision and every
    // window's source display are the same either way. Only the destination
    // column differs, and the display table above shows both.
    const bool ok = Prepare(self, mons, Rotation::Left, ignore, plan, &excluded);

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
