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

static int SlotForMonitor(const std::vector<MonitorRec>& mons, HMONITOR h, int a, int b)
{
    if (a >= 0 && a < static_cast<int>(mons.size()) && mons[static_cast<size_t>(a)].handle == h) return 0;
    if (b >= 0 && b < static_cast<int>(mons.size()) && mons[static_cast<size_t>(b)].handle == h) return 1;
    return -1;
}

struct CollectCtx
{
    HWND                           self     = nullptr;
    const std::vector<MonitorRec>* mons     = nullptr;
    int                            slotA    = 0;
    int                            slotB    = 1;
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

    HMONITOR  mon  = MonitorFromRect(&effective, MONITOR_DEFAULTTONEAREST);
    const int slot = SlotForMonitor(*ctx->mons, mon, ctx->slotA, ctx->slotB);
    if (slot < 0) { reject(L"on a display outside the swapped pair"); return TRUE; }

    const MonitorRec& src = (*ctx->mons)[static_cast<size_t>(slot == 0 ? ctx->slotA : ctx->slotB)];
    const MonitorRec& dst = (*ctx->mons)[static_cast<size_t>(slot == 0 ? ctx->slotB : ctx->slotA)];

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
    w.dstSlot = slot == 0 ? 1 : 0;

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

// ------------------------------------------------------------- operations ---

static bool Prepare(HWND self, std::vector<MonitorRec>& mons, int& slotA, int& slotB,
                    std::vector<WindowRec>& plan,
                    std::vector<std::pair<std::wstring, std::wstring>>* excluded)
{
    mons = EnumerateMonitors();
    if (mons.size() < 2) return false;

    slotA = 0;
    slotB = 1;

    CollectCtx ctx;
    ctx.self         = self;
    ctx.mons         = &mons;
    ctx.slotA        = slotA;
    ctx.slotB        = slotB;
    ctx.off          = WorkspaceOffset();
    ctx.included     = &plan;
    ctx.excluded     = excluded;
    ctx.wantExcluded = excluded != nullptr;

    EnumWindows(CollectProc, reinterpret_cast<LPARAM>(&ctx));
    return true;
}

static void LogMonitors(const std::vector<MonitorRec>& mons, int slotA, int slotB)
{
    LogF(L"Displays: %zu", mons.size());
    for (size_t i = 0; i < mons.size(); ++i)
    {
        const MonitorRec& m = mons[i];
        const wchar_t* role = (static_cast<int>(i) == slotA) ? L"<- pair A"
                            : (static_cast<int>(i) == slotB) ? L"<- pair B"
                            : L"";
        LogF(L"  [%zu] %s%s bounds=%s work=%s %s",
             i, m.device.c_str(), m.primary ? L" (primary)" : L"",
             RectStr(m.bounds).c_str(), RectStr(m.work).c_str(), role);
    }
    if (mons.size() > 2)
        LogF(L"  note: more than two displays; swapping the two leftmost.");
}

static void LogPlanLine(const WindowRec& w, POINT off)
{
    const RECT from = (w.state == WinState::Normal)
                    ? w.rect
                    : WorkspaceToScreen(w.wp.rcNormalPosition, off);

    LogF(L"  %-9s [%c->%c] %s -> %s  \"%s\"  [%s]",
         StateName(w.state),
         w.srcSlot == 0 ? L'A' : L'B',
         w.dstSlot == 0 ? L'A' : L'B',
         RectStr(from).c_str(),
         RectStr(w.targetRect).c_str(),
         w.title.c_str(), w.cls.c_str());
}

SwapResult PerformSwap(HWND self, bool dryRun)
{
    SwapResult res;

    std::vector<MonitorRec> mons;
    std::vector<WindowRec>  plan;
    int slotA = 0, slotB = 1;

    if (!Prepare(self, mons, slotA, slotB, plan, nullptr))
    {
        res.monitors = static_cast<int>(mons.size());
        LogF(L"Only %d display(s) detected - nothing to swap.", res.monitors);
        return res;
    }

    const POINT off = WorkspaceOffset();

    res.monitors   = static_cast<int>(mons.size());
    res.considered = static_cast<int>(plan.size());

    LogF(L"---- %s ----", dryRun ? L"dry run" : L"swap");
    LogMonitors(mons, slotA, slotB);
    LogF(L"Windows to swap: %d", res.considered);

    for (const WindowRec& w : plan)
        LogPlanLine(w, off);

    if (dryRun)
    {
        LogF(L"Dry run - nothing was moved.");
        return res;
    }

    for (const WindowRec& w : plan)
    {
        if (ApplyOne(w))
        {
            ++res.moved;
        }
        else
        {
            ++res.failed;
            LogF(L"  FAILED (err %lu) \"%s\" [%s]", GetLastError(), w.title.c_str(), w.cls.c_str());
        }
    }

    LogF(L"Swapped %d of %d window(s), %d failed.", res.moved, res.considered, res.failed);
    return res;
}

void ListAll(HWND self)
{
    std::vector<MonitorRec> mons;
    std::vector<WindowRec>  plan;
    std::vector<std::pair<std::wstring, std::wstring>> excluded;
    int slotA = 0, slotB = 1;

    const bool ok = Prepare(self, mons, slotA, slotB, plan, &excluded);

    LogF(L"---- list ----");
    LogMonitors(mons, slotA, slotB);

    if (!ok)
    {
        LogF(L"Fewer than two displays; window list not computed.");
        return;
    }

    const POINT off = WorkspaceOffset();

    LogF(L"");
    LogF(L"Included (%zu):", plan.size());
    for (const WindowRec& w : plan)
        LogPlanLine(w, off);

    LogF(L"");
    LogF(L"Excluded (%zu):", excluded.size());
    for (const auto& e : excluded)
        LogF(L"  %-40s  %s", e.second.c_str(), e.first.c_str());
}

// --------------------------------------------------------------- self test --

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
    LogMonitors(mons, 0, 1);

    if (mons.size() < 2)
    {
        LogF(L"Self test needs two displays.");
        return 2;
    }

    const MonitorRec& A = mons[0];
    const MonitorRec& B = mons[1];
    const POINT off = WorkspaceOffset();

    // 1. Pure math: a round trip through both work areas must be the identity.
    LogF(L"Remap round-trip:");
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
        swprintf_s(msg, L"%s -> %s -> %s",
                   RectStr(s).c_str(), RectStr(there).c_str(), RectStr(back).c_str());
        check(EqualRect(&back, &s) != FALSE, msg);
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

    for (HWND h : wnd) DestroyWindow(h);
    PumpFor(100);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    LogF(L"Self test: %s (%d failure(s))", failures == 0 ? L"PASS" : L"FAIL", failures);
    return failures == 0 ? 0 : 1;
}
