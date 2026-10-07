#include "selftest.h"

#include "log.h"
#include "resource.h"
#include "swapper.h"
#include "tray.h"

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

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

int SelfTest(bool windows)
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

    // Tray icon: a picture for each count from one to four, four for anything
    // beyond - and every one of them has to actually be in the exe.
    LogF(L"Tray icon:");
    {
        const int expect[] = { IDI_DISPLAYS_1, IDI_DISPLAYS_1, IDI_DISPLAYS_2, IDI_DISPLAYS_3,
                               IDI_DISPLAYS_4, IDI_DISPLAYS_4, IDI_DISPLAYS_4 };   // for 0 to 6
        bool mapped = true;
        for (int n = 0; n < static_cast<int>(_countof(expect)); ++n)
            if (TrayIconResource(n) != expect[n]) mapped = false;
        check(mapped, L"0-1 displays picture one, 2 and 3 their own, 4 or more picture four");

        const int ids[] = { IDI_APPICON, IDI_DISPLAYS_1, IDI_DISPLAYS_2, IDI_DISPLAYS_3, IDI_DISPLAYS_4 };
        bool loaded = true;
        for (int id : ids)
        {
            HICON h = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(id),
                                                    IMAGE_ICON, 16, 16, 0));
            if (h) DestroyIcon(h);
            else   loaded = false;
        }
        check(loaded, L"the exe icon and all four tray icons load from the resources");
    }

    // Start with Windows, run through the real code against a scratch key, so the
    // user's actual start-up entries are never touched.
    LogF(L"Start with Windows (in a scratch registry key):");
    {
        const wchar_t*    root = L"Software\\WinSwapperSelfTest";
        const StartupKeys keys = { L"Software\\WinSwapperSelfTest\\Run",
                                   L"Software\\WinSwapperSelfTest\\StartupApproved",
                                   L"WinSwapper" };

        // RegSetKeyValueW is not relied on to create a missing subkey.
        const auto write = [](const wchar_t* sub, const wchar_t* name, DWORD type,
                              const void* data, DWORD bytes) {
            HKEY k = nullptr;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, sub, 0, nullptr, 0, KEY_SET_VALUE,
                                nullptr, &k, nullptr) != ERROR_SUCCESS)
                return false;
            const bool ok = RegSetValueExW(k, name, 0, type, static_cast<const BYTE*>(data), bytes)
                            == ERROR_SUCCESS;
            RegCloseKey(k);
            return ok;
        };
        const auto registered = [&keys](std::wstring& out) {
            wchar_t buf[1024] = {};
            DWORD   bytes     = sizeof(buf);
            if (RegGetValueW(HKEY_CURRENT_USER, keys.run, keys.value, RRF_RT_REG_SZ,
                             nullptr, buf, &bytes) != ERROR_SUCCESS)
                return false;
            out = buf;
            return true;
        };

        RegDeleteTreeW(HKEY_CURRENT_USER, root);   // leftovers from an interrupted run
        std::wstring value;

        check(!IsStartupEnabled(keys) && !registered(value), L"off, with nothing written, until turned on");

        const bool on = SetStartupEnabled(keys, true);
        check(on && IsStartupEnabled(keys) && registered(value) && value == StartupCommand(),
              L"turning it on registers this exe's quoted path");

        // What Task Manager writes when an entry is disabled there.
        const BYTE disabled[12] = { 3 };
        const bool marked = write(keys.approved, keys.value, REG_BINARY, disabled, sizeof(disabled));
        check(marked && !IsStartupEnabled(keys), L"disabled in Task Manager reads as off");

        SetStartupEnabled(keys, true);
        check(IsStartupEnabled(keys), L"turning it on again clears Task Manager's disabled flag");

        SetStartupEnabled(keys, false);
        check(!IsStartupEnabled(keys) && !registered(value), L"turning it off removes the entry");

        const std::wstring other = L"\"C:\\Elsewhere\\winswapper.exe\"";
        const bool elsewhere = write(keys.run, keys.value, REG_SZ, other.c_str(),
                                     static_cast<DWORD>((other.size() + 1) * sizeof(wchar_t)));
        check(elsewhere && !IsStartupEnabled(keys),
              L"registered for a different copy of the exe reads as off for this one");

        RegDeleteTreeW(HKEY_CURRENT_USER, root);
        HKEY gone = nullptr;
        const bool cleaned = RegOpenKeyExW(HKEY_CURRENT_USER, root, 0, KEY_READ, &gone) != ERROR_SUCCESS;
        if (gone) RegCloseKey(gone);
        check(cleaned, L"scratch key removed afterwards");
    }

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

    // Everything from here creates real windows and moves them across the user's
    // displays, which takes the focus and interrupts whatever they are doing. All of
    // the above opened nothing, so --no-windows stops exactly here.
    if (!windows)
    {
        LogF(L"");
        LogF(L"SKIPPED: the window-move tests, as --no-windows asked.");
        LogF(L"Self test: %s (%d failure(s); window tests skipped)",
             failures == 0 ? L"PASS" : L"FAIL", failures);
        return failures == 0 ? 3 : 1;
    }

    LogF(L"Moving test windows from [0] %s to [%d] %s",
         A.device.c_str(), DestSlot(0, static_cast<int>(mons.size()), Rotation::Left),
         B.device.c_str());

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
