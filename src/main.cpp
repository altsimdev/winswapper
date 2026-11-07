#include <windows.h>
#include <shellapi.h>

#include <string>

#include "drag.h"
#include "log.h"
#include "resource.h"
#include "selftest.h"
#include "settings.h"
#include "swapper.h"
#include "tray.h"

#define WM_TRAYICON (WM_APP + 1)

static const UINT kHotkeyLeft  = 1;
static const UINT kHotkeyRight = 2;
static const UINT kTrayId      = 1;

// Started only if the log could not be opened at start-up, and stopped once it
// is, so the lines kept meanwhile reach the file soon after it frees up - even if
// nothing else is logged for hours.
static const UINT kLogRetryTimer   = 1;
static const UINT kLogRetryEveryMs = 10 * 1000;

static HINSTANCE g_inst           = nullptr;
static HWND      g_hwnd           = nullptr;
static HICON     g_icon           = nullptr;
static bool      g_iconOwned      = false;   // false for the stock fallback, which is shared
static int       g_iconDisplays   = 0;       // how many displays g_icon was chosen for
static UINT      g_taskbarCreated = 0;
static bool      g_leftOk         = false;   // the left hotkey is registered
static bool      g_rightOk        = false;
static bool      g_dragOn         = false;   // the mouse is being watched for dragging
static HANDLE    g_mutex          = nullptr;
static Settings  g_settings       = DefaultSettings();

// ------------------------------------------------------------------ tray ----

// What the mouse does with the keys held, such as "Alt+drag moves, right-drag resizes,
// middle-click lowers": the keys are named again only where they change, which keeps it
// short enough for the tooltip. Empty when all three are off.
static std::wstring Gestures()
{
    struct Gesture
    {
        UINT           mods;
        const wchar_t* does;
    };
    const Gesture all[] = { { g_settings.moveMods,   L"drag moves"          },
                            { g_settings.resizeMods, L"right-drag resizes"  },
                            { g_settings.lowerMods,  L"middle-click lowers" } };
    std::wstring s;
    UINT         keys = 0;
    for (const Gesture& g : all)
    {
        if (!g.mods) continue;
        if (!s.empty()) s += L", ";
        if (g.mods != keys) s += FormatModifiers(g.mods) + L"+";
        s += g.does;
        keys = g.mods;
    }
    return s;
}

// How the mouse stands, for the About box and the reload balloon.
static std::wstring MouseState()
{
    std::wstring s = Gestures();
    if (s.empty())  return L"off (in settings)";
    if (!g_dragOn) return s + L" (UNAVAILABLE - see the log)";
    if (!g_settings.dragSkip.empty())
        s += L", except in " + std::to_wstring(g_settings.dragSkip.size()) + L" program(s)";
    return s;
}

static std::wstring SnapState()
{
    if (!g_settings.snap) return L"off (in settings)";
    if (!g_settings.moveMods && !g_settings.resizeMods) return L"unused, as moving and resizing are off";
    return L"within " + std::to_wstring(g_settings.snap) + L" pixels of a display's edge, at 100% scaling";
}

// The tooltip carries text from the settings now, so it is truncated to fit rather
// than formatted with swprintf_s, which would abort the process on overflow. Windows
// keeps 127 characters of it, which the defaults fit with a few to spare.
static void FillTip(NOTIFYICONDATAW& nid)
{
    const std::wstring gestures = Gestures();
    const std::wstring mouse    = gestures.empty() ? L"Mouse: off"
                                : g_dragOn         ? gestures
                                                   : L"Mouse: unavailable";
    _snwprintf_s(nid.szTip, _TRUNCATE,
                 L"WinSwapper - %d display%s\nLeft: %s\nRight: %s\n%s",
                 g_iconDisplays, g_iconDisplays == 1 ? L"" : L"s",
                 FormatHotkey(g_settings.rotateLeft).c_str(),
                 FormatHotkey(g_settings.rotateRight).c_str(),
                 mouse.c_str());
}

static void UpdateTrayTip()
{
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = g_hwnd;
    nid.uID    = kTrayId;
    nid.uFlags = NIF_TIP;
    FillTip(nid);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// Points the tray icon at the picture of however many displays are connected, if
// that has changed. `notify` is false only before the icon has been added.
static void RefreshTrayIcon(bool notify)
{
    const int displays = static_cast<int>(EnumerateMonitors().size());
    if (g_icon && displays == g_iconDisplays) return;

    HICON icon  = static_cast<HICON>(LoadImageW(g_inst, MAKEINTRESOURCEW(TrayIconResource(displays)),
                                                IMAGE_ICON,
                                                GetSystemMetrics(SM_CXSMICON),
                                                GetSystemMetrics(SM_CYSMICON),
                                                LR_DEFAULTCOLOR));
    bool  owned = true;
    if (!icon)
    {
        if (g_icon) return;                              // keep the picture we have
        icon  = LoadIconW(nullptr, IDI_APPLICATION);     // stock icon: shared, never destroyed
        owned = false;
    }

    const HICON oldIcon  = g_icon;
    const bool  oldOwned = g_iconOwned;
    g_icon         = icon;
    g_iconOwned    = owned;
    g_iconDisplays = displays;

    if (notify)
    {
        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd   = g_hwnd;
        nid.uID    = kTrayId;
        nid.uFlags = NIF_ICON | NIF_TIP;
        nid.hIcon  = g_icon;
        FillTip(nid);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
    LogF(L"Tray icon shows %d display(s).", displays);

    // The shell keeps its own copy, so the old icon can go once it has the new one.
    if (oldIcon && oldOwned) DestroyIcon(oldIcon);
}

static void AddTrayIcon()
{
    NOTIFYICONDATAW nid = {};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = g_hwnd;
    nid.uID              = kTrayId;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon            = g_icon;
    FillTip(nid);

    Shell_NotifyIconW(NIM_ADD, &nid);

    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

static void RemoveTrayIcon()
{
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = g_hwnd;
    nid.uID    = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void Balloon(const wchar_t* text, DWORD infoFlag)
{
    NOTIFYICONDATAW nid = {};
    nid.cbSize      = sizeof(nid);
    nid.hWnd        = g_hwnd;
    nid.uID         = kTrayId;
    nid.uFlags      = NIF_INFO;
    nid.dwInfoFlags = infoFlag;
    wcscpy_s(nid.szInfoTitle, L"WinSwapper");
    // Truncated rather than wcscpy_s'd: balloon text can now include hotkeys and
    // program names from the settings, and wcscpy_s aborts on overflow.
    wcsncpy_s(nid.szInfo, text, _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// ---------------------------------------------------------- hotkeys, settings ---

static void UnregisterHotkeys()
{
    if (g_leftOk)  UnregisterHotKey(g_hwnd, kHotkeyLeft);
    if (g_rightOk) UnregisterHotKey(g_hwnd, kHotkeyRight);
    g_leftOk = g_rightOk = false;
}

// Registers the hotkeys the settings ask for, each separately so losing one to
// another application leaves the other working. Returns the ones that could not be
// had, for the balloon, or an empty string.
static std::wstring RegisterHotkeys()
{
    UnregisterHotkeys();

    std::wstring lost;
    const auto reg = [&lost](UINT id, const Hotkey& h, const wchar_t* dir, bool& ok) {
        if (!h.On())
        {
            LogF(L"Rotating %s has no hotkey (turned off in settings).", dir);
            return;
        }
        ok = RegisterHotKey(g_hwnd, id, h.mods | MOD_NOREPEAT, h.vk) != FALSE;
        const DWORD err = GetLastError();
        if (ok)
        {
            LogF(L"Hotkey %s rotates %s.", FormatHotkey(h).c_str(), dir);
            return;
        }
        LogF(L"RegisterHotKey(%s) for rotating %s failed, error %lu.", FormatHotkey(h).c_str(), dir, err);
        if (!lost.empty()) lost += L" and ";
        lost += FormatHotkey(h);
    };
    reg(kHotkeyLeft,  g_settings.rotateLeft,  L"left",  g_leftOk);
    reg(kHotkeyRight, g_settings.rotateRight, L"right", g_rightOk);
    return lost;
}

// Starts or stops watching the mouse, as the settings say. Returns false only when
// dragging is wanted but could not be set up.
static bool ApplyDragging()
{
    DragOptions opts;
    opts.moveMods   = g_settings.moveMods;
    opts.resizeMods = g_settings.resizeMods;
    opts.lowerMods  = g_settings.lowerMods;
    opts.snap       = g_settings.snap;
    opts.skip       = g_settings.dragSkip;
    opts.notify     = g_hwnd;

    DWORD              err      = 0;
    const bool         ok       = DragStart(opts, err);
    const std::wstring gestures = Gestures();
    const bool         want     = !gestures.empty();
    g_dragOn = ok && want;

    if (!want)
        LogF(L"Moving, resizing and lowering windows with the mouse are all turned off in settings.");
    else if (ok)
    {
        std::wstring line = L"Mouse: " + gestures;
        if (!opts.skip.empty())
            line += L", except in " + std::to_wstring(opts.skip.size()) + L" program(s)";
        LogF(L"%s. Snapping: %s.", line.c_str(), SnapState().c_str());
    }
    else
        LogF(L"Could not watch the mouse for dragging windows, error %lu.", err);
    return ok;
}

// Logs what loading the settings found, and says so in a balloon when something
// needs the user's attention - or, after an explicit reload, that it worked.
static void ReportSettings(const std::vector<std::wstring>& problems, const std::wstring& lost,
                           bool dragOk, bool reloaded)
{
    for (const std::wstring& p : problems)
        LogF(L"settings.ini %s", p.c_str());

    wchar_t msg[256];
    if (!problems.empty() || !lost.empty() || !dragOk)
    {
        std::wstring text;
        if (!problems.empty())
            text = L"settings.ini has " + std::to_wstring(problems.size()) +
                   L" problem(s); the rest was applied. See Open log.";
        if (!lost.empty())
            text += (text.empty() ? L"" : L"\n") + lost + L" is taken by another app. "
                    L"Use the tray menu, or pick another in Edit settings.";
        if (!dragOk)
            text += (text.empty() ? L"" : L"\n") + std::wstring(L"Dragging windows could not be set up. See Open log.");
        Balloon(text.c_str(), NIIF_WARNING);
    }
    else if (reloaded)
    {
        _snwprintf_s(msg, _TRUNCATE,
                     L"Settings reloaded.\nLeft: %s   Right: %s\nIgnoring %zu program(s).\nMouse: %s",
                     FormatHotkey(g_settings.rotateLeft).c_str(),
                     FormatHotkey(g_settings.rotateRight).c_str(), g_settings.ignore.size(),
                     MouseState().c_str());
        Balloon(msg, NIIF_INFO);
    }
}

// A missing or blank settings file is written with every setting explained and shown
// at its default, all commented out; one with anything in it is never touched. False if
// it needed writing and could not be.
static bool EnsureSettings(const std::wstring& path)
{
    switch (EnsureSettingsFile(path))
    {
    case SettingsFile::Created:
        LogF(L"Created %s, which explains every setting, all left at their defaults.", path.c_str());
        return true;
    case SettingsFile::Filled:
        LogF(L"%s was empty, so it now explains every setting, all left at their defaults.", path.c_str());
        return true;
    case SettingsFile::Failed:
        LogF(L"Could not write %s, error %lu.", path.c_str(), GetLastError());
        return false;
    default:
        return true;
    }
}

static void LoadAndApplySettings(bool reloaded)
{
    std::vector<std::wstring> problems;
    const std::wstring path = SettingsPath();
    EnsureSettings(path);   // if it cannot be written, the defaults still apply
    g_settings = LoadSettings(path, problems);

    const bool exists = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    LogF(L"Settings %s %s%s; ignoring %zu program(s).", reloaded ? L"reloaded from" : L"read from",
         path.c_str(), exists ? L"" : L" (not created yet, so the defaults)", g_settings.ignore.size());

    const std::wstring lost   = RegisterHotkeys();
    const bool         dragOk = ApplyDragging();
    UpdateTrayTip();
    ReportSettings(problems, lost, dragOk, reloaded);
}

static void EditSettings()
{
    // Normally there since start-up, but it may have been deleted or emptied since.
    const std::wstring path = SettingsPath();
    if (!EnsureSettings(path))
    {
        Balloon(L"Could not create the settings file. See Open log.", NIIF_WARNING);
        return;
    }
    ShellExecuteW(nullptr, nullptr, L"notepad.exe", path.c_str(), nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------- actions ---

static void DoRotate(Rotation dir)
{
    const SwapResult r = PerformSwap(g_hwnd, false, dir, g_settings.ignore);

    if (r.monitors < 2)
    {
        Balloon(L"Only one display detected - nothing to rotate.", NIIF_WARNING);
        return;
    }

    // Success is silent; a hotkey tool that nags on every use gets uninstalled.
    // Only the surprising case is worth a balloon.
    if (r.failed > 0)
    {
        wchar_t msg[256];
        swprintf_s(msg,
                   L"Moved %d window(s).\nSkipped %d - owned by an elevated process.",
                   r.moved, r.failed);
        Balloon(msg, NIIF_WARNING);
    }
}

static void ToggleStartup()
{
    const bool enable = !IsStartupEnabled(kStartupKeys);
    if (SetStartupEnabled(kStartupKeys, enable))
    {
        if (enable) LogF(L"Start with Windows turned on: %s", StartupCommand().c_str());
        else        LogF(L"Start with Windows turned off.");
        return;
    }

    LogF(L"Could not turn start with Windows %s, error %lu.", enable ? L"on" : L"off", GetLastError());
    Balloon(enable ? L"Could not set WinSwapper to start with Windows."
                   : L"Could not stop WinSwapper starting with Windows.", NIIF_WARNING);
}

static void OpenLog()
{
    // Without this, a log that could not be opened would show an earlier run's
    // lines as though they were this one's - which is how the problem went unseen.
    if (!LogRetry())
    {
        const std::wstring msg = L"WinSwapper's log is not being written.\n\n" + LogProblem() +
                                 L"\n\n" + LogFilePath() +
                                 L"\n\nThe file opens next, as it was before then.";
        MessageBoxW(nullptr, msg.c_str(), L"WinSwapper", MB_OK | MB_ICONWARNING);
    }
    ShellExecuteW(nullptr, nullptr, L"notepad.exe", LogFilePath(), nullptr, SW_SHOWNORMAL);
}

static std::wstring HotkeyState(const Hotkey& h, bool registered)
{
    if (!h.On()) return L"off (in settings)";
    return FormatHotkey(h) + (registered ? L"" : L" (UNAVAILABLE - taken by another app)");
}

static void ShowAbout()
{
    const std::wstring logProblem = LogRetry() ? std::wstring() : L"\n" + LogProblem();

    // Truncated to fit rather than swprintf_s'd: two paths and two hotkeys from the
    // settings make the length depend on things outside this code.
    wchar_t msg[2048];
    _snwprintf_s(msg, _TRUNCATE,
               L"WinSwapper 1.09\n\n"
               L"Moves every ordinary window one display to the left or the right, with "
               L"the end display wrapping around. Each window keeps its size and its "
               L"position within its display.\n\n"
               L"The two directions are inverses, so one of each puts everything back - "
               L"pixel-exact when the displays share a resolution. With two displays both "
               L"do the same swap; with N displays, N presses the same way also completes "
               L"the cycle.\n\n"
               L"Hold Alt, or the keys the settings name, anywhere in a window: dragging "
               L"with the left mouse button moves it, with the right resizes it, and a "
               L"middle-click sends it to the back, as on many Linux desktops. A dragged "
               L"window's edges snap to the edges of the displays.\n\n"
               L"Rotate left:  %s\n"
               L"Rotate right: %s\n"
               L"Ignoring: %zu program(s)\n"
               L"Mouse: %s\n"
               L"Snapping: %s\n\n"
               L"Settings: %s\n"
               L"Log: %s%s\n\n"
               L"Command line: --list, --dry-run, --rotate, --reverse, --selftest, --no-windows",
               HotkeyState(g_settings.rotateLeft,  g_leftOk).c_str(),
               HotkeyState(g_settings.rotateRight, g_rightOk).c_str(),
               g_settings.ignore.size(),
               MouseState().c_str(),
               SnapState().c_str(),
               SettingsPath().c_str(),
               LogFilePath(), logProblem.c_str());
    MessageBoxW(nullptr, msg, L"About WinSwapper", MB_OK | MB_ICONINFORMATION);
}

static void ShowMenu()
{
    POINT pt;
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    // The hotkey shown beside each item is the one actually registered, so an item
    // whose hotkey is turned off or taken shows none.
    const std::wstring left  = std::wstring(L"Rotate le&ft") +
        (g_leftOk  ? L"\t" + FormatHotkey(g_settings.rotateLeft)  : std::wstring());
    const std::wstring right = std::wstring(L"Rotate &right") +
        (g_rightOk ? L"\t" + FormatHotkey(g_settings.rotateRight) : std::wstring());

    // Mnemonics must stay unique across the whole menu - F, R, W, E, D, L, A, X - or
    // Windows only cycles the highlight between the clashing items.
    AppendMenuW(menu, MF_STRING, IDM_LEFT,  left.c_str());
    AppendMenuW(menu, MF_STRING, IDM_RIGHT, right.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    // Read fresh each time the menu opens, so a change made in Task Manager or by
    // another copy shows up here.
    AppendMenuW(menu, MF_STRING | (IsStartupEnabled(kStartupKeys) ? MF_CHECKED : MF_UNCHECKED),
                IDM_STARTUP, L"Start with &Windows");
    AppendMenuW(menu, MF_STRING, IDM_EDIT_SETTINGS,   L"&Edit settings");
    AppendMenuW(menu, MF_STRING, IDM_RELOAD_SETTINGS, L"Reloa&d settings");
    AppendMenuW(menu, MF_STRING, IDM_LOG,   L"Open &log");
    AppendMenuW(menu, MF_STRING, IDM_ABOUT, L"&About");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT,  L"E&xit");

    // Both halves of the classic fix: without these the menu refuses to dismiss
    // when the user clicks somewhere else.
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);

    DestroyMenu(menu);
}

// ---------------------------------------------------------------- window ----

// When Windows signed out or shut down, the tray log used to just stop, which looked
// like a crash - or like a run that was never logged at all. Now it says why. Windows
// does not say whether it is shutting down or restarting.
static void LogSessionEnd(bool ending, LPARAM lp)
{
    const bool closeApp = (lp & ENDSESSION_CLOSEAPP) != 0;   // Restart Manager, for an installer
    const bool logOff   = (lp & ENDSESSION_LOGOFF) != 0;
    if (ending)
    {
        const wchar_t* forced = (lp & ENDSESSION_CRITICAL) ? L" (forced)" : L"";
        if (closeApp)    LogF(L"Windows asks WinSwapper to close%s, for an installer or an update. Closing.", forced);
        else if (logOff) LogF(L"Windows is signing out%s. WinSwapper closes with it.", forced);
        else             LogF(L"Windows is shutting down or restarting%s. WinSwapper closes with it.", forced);
    }
    else if (closeApp)
        LogF(L"Windows asked WinSwapper to close, for an installer or an update, but that was called off. "
             L"WinSwapper carries on.");
    else if (logOff)
        LogF(L"Windows was going to sign out, but that was called off. WinSwapper carries on.");
    else
        LogF(L"Windows was going to shut down or restart, but that was called off. WinSwapper carries on.");
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    // Explorer restarted: the shell forgot our icon, so put it back.
    if (g_taskbarCreated != 0 && msg == g_taskbarCreated)
    {
        AddTrayIcon();
        return 0;
    }

    switch (msg)
    {
    case WM_TRAYICON:
        switch (LOWORD(lp))
        {
        case WM_CONTEXTMENU:
        case WM_RBUTTONUP:     ShowMenu();               break;
        case WM_LBUTTONDBLCLK: DoRotate(Rotation::Left); break;
        default: break;
        }
        return 0;

    case WM_HOTKEY:
        if      (wp == kHotkeyLeft)  DoRotate(Rotation::Left);
        else if (wp == kHotkeyRight) DoRotate(Rotation::Right);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDM_LEFT:    DoRotate(Rotation::Left);  break;
        case IDM_RIGHT:   DoRotate(Rotation::Right); break;
        case IDM_STARTUP: ToggleStartup();           break;
        case IDM_EDIT_SETTINGS:   EditSettings();             break;
        case IDM_RELOAD_SETTINGS: LoadAndApplySettings(true); break;
        case IDM_LOG:     OpenLog();                 break;
        case IDM_ABOUT:   ShowAbout();               break;
        case IDM_EXIT:    DestroyWindow(hwnd);       break;
        default: break;
        }
        return 0;

    case WM_DISPLAYCHANGE:
        LogF(L"Display configuration changed.");
        RefreshTrayIcon(true);
        return 0;

    case WM_TIMER:
        if (wp == kLogRetryTimer && LogRetry()) KillTimer(hwnd, kLogRetryTimer);
        return 0;

    case kDragLogMessage:
        for (const std::wstring& line : DragTakeLog())
            LogF(L"%s", line.c_str());
        return 0;

    // Sent to every top-level window, shown or not - one more reason this window is
    // not message-only. WinSwapper has nothing to save, so it never stands in the way.
    case WM_QUERYENDSESSION:
        return TRUE;

    case WM_ENDSESSION:
        LogSessionEnd(wp != FALSE, lp);
        // Windows may end the process any time after this returns, so it closes now,
        // the ordinary way: WM_DESTROY stops watching the mouse, takes the icon away
        // and ends the message loop, which logs the exit.
        if (wp) DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        DragStop();
        for (const std::wstring& line : DragTakeLog())   // a drag that ended just now
            LogF(L"%s", line.c_str());
        RemoveTrayIcon();
        UnregisterHotkeys();
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ------------------------------------------------------------------ entry ---

static void PrintUsage()
{
    LogF(L"WinSwapper - rotate windows one display left or right.");
    LogF(L"");
    LogF(L"  winswapper.exe             run in the notification area");
    LogF(L"                             %s rotates left, %s right",
         FormatHotkey(g_settings.rotateLeft).c_str(), FormatHotkey(g_settings.rotateRight).c_str());
    // "anywhere in a window" goes on the first of these lines only.
    const wchar_t* where   = L" anywhere in a window";
    const auto     gesture = [&where](UINT mods, const wchar_t* how, const wchar_t* does) {
        if (!mods) return;
        LogF(L"                             %s+%s%s %s", FormatModifiers(mods).c_str(), how, where, does);
        where = L"";
    };
    gesture(g_settings.moveMods,   L"drag",         L"moves it");
    gesture(g_settings.resizeMods, L"right-drag",   L"resizes it");
    gesture(g_settings.lowerMods,  L"middle-click", L"sends it to the back");
    LogF(L"  winswapper.exe --list      show displays and every window, included or not");
    LogF(L"  winswapper.exe --dry-run   compute the rotation and print it, move nothing");
    LogF(L"  winswapper.exe --rotate    perform one rotation and exit (--swap also works)");
    LogF(L"  winswapper.exe --selftest  verify the remap and drag logic and the window-move paths");
    LogF(L"");
    LogF(L"  --reverse, --right         with --rotate or --dry-run, rotate right instead;");
    LogF(L"                             an error with anything else");
    LogF(L"  --no-windows               with --selftest, skip the tests that open and move");
    LogF(L"                             windows, so nothing on screen is disturbed");
    LogF(L"");
    LogF(L"--selftest exits 0 if every check ran and passed, 3 if it passed but skipped");
    LogF(L"the window-move tests (for --no-windows, or with only one display), and 1 on a");
    LogF(L"failure. Usage errors exit 2.");
    LogF(L"");
    LogF(L"Settings: %s", SettingsPath().c_str());
    LogF(L"Log file: %s", LogFilePath());
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int)
{
    g_inst = inst;

    bool doList = false, doDry = false, doSwapOnce = false, doSelfTest = false, doHelp = false;
    bool reverse = false, noWindows = false;
    bool badArg = false;

    int     argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv)
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::wstring a = argv[i];
            if      (a == L"--list")     doList     = true;
            else if (a == L"--dry-run")  doDry      = true;
            else if (a == L"--rotate")   doSwapOnce = true;
            else if (a == L"--swap")     doSwapOnce = true;   // the pre-1.01 spelling
            else if (a == L"--selftest") doSelfTest = true;
            // Modifiers, not modes: they change how --rotate/--dry-run or --selftest run.
            else if (a == L"--reverse" || a == L"--right") reverse = true;
            else if (a == L"--no-windows") noWindows = true;
            else if (a == L"--help" || a == L"-h" || a == L"/?") doHelp = true;
            else                         badArg     = true;
        }
        LocalFree(argv);
    }

    // A modifier only means something alongside the mode it modifies. Without one it
    // would fall through to tray mode and be silently ignored, so it is an error.
    const wchar_t* stray = nullptr;
    if      (reverse && !doDry && !doSwapOnce) stray = L"--reverse needs --rotate or --dry-run to act on.";
    else if (noWindows && !doSelfTest)         stray = L"--no-windows only applies to --selftest.";

    if (doHelp || badArg || stray || doList || doDry || doSwapOnce || doSelfTest)
    {
        LogInit(LogMode::Cli);

        // The command-line modes honour the settings too: --list shows ignored
        // programs, --rotate leaves them alone, and --help shows the real hotkeys.
        std::vector<std::wstring> problems;
        g_settings = LoadSettings(SettingsPath(), problems);
        for (const std::wstring& p : problems)
            LogF(L"settings.ini %s", p.c_str());

        int rc = 0;
        if (doHelp || badArg || stray)
        {
            if (badArg)     LogF(L"Unrecognised argument.");
            else if (stray) LogF(L"%s", stray);
            PrintUsage();
            rc = (badArg || stray) ? 2 : 0;
        }
        else if (doSelfTest)
        {
            rc = SelfTest(!noWindows);
        }
        else if (doList)
        {
            ListAll(nullptr, g_settings.ignore);
        }
        else
        {
            PerformSwap(nullptr, doDry, reverse ? Rotation::Right : Rotation::Left, g_settings.ignore);
        }

        LogF(L"(log: %s)", LogFilePath());
        LogShutdown();
        return rc;
    }

    g_mutex = CreateMutexW(nullptr, TRUE, L"Local\\WinSwapper_SingleInstance");
    if (g_mutex && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        MessageBoxW(nullptr,
                    L"WinSwapper is already running - look in the notification area.",
                    L"WinSwapper", MB_OK | MB_ICONINFORMATION);
        CloseHandle(g_mutex);
        return 0;
    }

    LogInit(LogMode::Tray);
    LogF(L"WinSwapper started.");

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = inst;
    wc.lpszClassName = L"WinSwapperHiddenWnd";
    if (!RegisterClassExW(&wc))
    {
        MessageBoxW(nullptr, L"Could not register the window class.", L"WinSwapper",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    // Must be a real top-level window rather than HWND_MESSAGE: message-only
    // windows do not receive the shell's TaskbarCreated broadcast. It is simply
    // never shown.
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"WinSwapper", WS_OVERLAPPED,
                             0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd)
    {
        MessageBoxW(nullptr, L"Could not create the message window.", L"WinSwapper",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    RefreshTrayIcon(false);
    AddTrayIcon();

    // Reads settings.ini and registers its hotkeys, each separately so losing one to
    // another app leaves the other working. The defaults, Ctrl+Alt+S and
    // Ctrl+Alt+Shift+S, are distinct to RegisterHotKey, so holding Shift picks the
    // reverse rotation on its own.
    LoadAndApplySettings(false);

    // Every line tries the log again anyway, but an idle tray app logs nothing for
    // hours, and the start-up lines are the ones that say what it is running with.
    if (!LogRetry()) SetTimer(g_hwnd, kLogRetryTimer, kLogRetryEveryMs, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_icon && g_iconOwned) DestroyIcon(g_icon);

    LogF(L"WinSwapper exited.");
    LogShutdown();

    if (g_mutex)
    {
        ReleaseMutex(g_mutex);
        CloseHandle(g_mutex);
    }
    return 0;
}
