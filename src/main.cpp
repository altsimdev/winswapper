#include <windows.h>
#include <shellapi.h>

#include <string>

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

static HINSTANCE g_inst           = nullptr;
static HWND      g_hwnd           = nullptr;
static HICON     g_icon           = nullptr;
static bool      g_iconOwned      = false;   // false for the stock fallback, which is shared
static int       g_iconDisplays   = 0;       // how many displays g_icon was chosen for
static UINT      g_taskbarCreated = 0;
static bool      g_leftOk         = false;   // the left hotkey is registered
static bool      g_rightOk        = false;
static HANDLE    g_mutex          = nullptr;
static Settings  g_settings       = DefaultSettings();

// ------------------------------------------------------------------ tray ----

// The tooltip carries text from the settings now, so it is truncated to fit rather
// than formatted with swprintf_s, which would abort the process on overflow.
static void FillTip(NOTIFYICONDATAW& nid)
{
    _snwprintf_s(nid.szTip, _TRUNCATE, L"WinSwapper - %d display%s\nLeft: %s\nRight: %s",
                 g_iconDisplays, g_iconDisplays == 1 ? L"" : L"s",
                 FormatHotkey(g_settings.rotateLeft).c_str(),
                 FormatHotkey(g_settings.rotateRight).c_str());
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

// Logs what loading the settings found, and says so in a balloon when something
// needs the user's attention - or, after an explicit reload, that it worked.
static void ReportSettings(const std::vector<std::wstring>& problems, const std::wstring& lost,
                           bool reloaded)
{
    for (const std::wstring& p : problems)
        LogF(L"settings.ini %s", p.c_str());

    wchar_t msg[256];
    if (!problems.empty() || !lost.empty())
    {
        std::wstring text;
        if (!problems.empty())
            text = L"settings.ini has " + std::to_wstring(problems.size()) +
                   L" problem(s); the rest was applied. See Open log.";
        if (!lost.empty())
            text += (text.empty() ? L"" : L"\n") + lost + L" is taken by another app. "
                    L"Use the tray menu, or pick another in Edit settings.";
        Balloon(text.c_str(), NIIF_WARNING);
    }
    else if (reloaded)
    {
        _snwprintf_s(msg, _TRUNCATE, L"Settings reloaded.\nLeft: %s   Right: %s\nIgnoring %zu program(s).",
                     FormatHotkey(g_settings.rotateLeft).c_str(),
                     FormatHotkey(g_settings.rotateRight).c_str(), g_settings.ignore.size());
        Balloon(msg, NIIF_INFO);
    }
}

static void LoadAndApplySettings(bool reloaded)
{
    std::vector<std::wstring> problems;
    const std::wstring path = SettingsPath();
    g_settings = LoadSettings(path, problems);

    const bool exists = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    LogF(L"Settings %s %s%s; ignoring %zu program(s).", reloaded ? L"reloaded from" : L"read from",
         path.c_str(), exists ? L"" : L" (not created yet, so the defaults)", g_settings.ignore.size());

    const std::wstring lost = RegisterHotkeys();
    UpdateTrayTip();
    ReportSettings(problems, lost, reloaded);
}

static void EditSettings()
{
    // Created with the defaults and comments on first use; never overwritten.
    const std::wstring path = SettingsPath();
    if (!WriteDefaultSettings(path))
    {
        LogF(L"Could not create %s, error %lu.", path.c_str(), GetLastError());
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
    ShellExecuteW(nullptr, nullptr, L"notepad.exe", LogFilePath(), nullptr, SW_SHOWNORMAL);
}

static std::wstring HotkeyState(const Hotkey& h, bool registered)
{
    if (!h.On()) return L"off (in settings)";
    return FormatHotkey(h) + (registered ? L"" : L" (UNAVAILABLE - taken by another app)");
}

static void ShowAbout()
{
    // Truncated to fit rather than swprintf_s'd: two paths and two hotkeys from the
    // settings make the length depend on things outside this code.
    wchar_t msg[2048];
    _snwprintf_s(msg, _TRUNCATE,
               L"WinSwapper 1.06\n\n"
               L"Moves every ordinary window one display to the left or the right, with "
               L"the end display wrapping around. Each window keeps its size and its "
               L"position within its display.\n\n"
               L"The two directions are inverses, so one of each puts everything back - "
               L"pixel-exact when the displays share a resolution. With two displays both "
               L"do the same swap; with N displays, N presses the same way also completes "
               L"the cycle.\n\n"
               L"Rotate left:  %s\n"
               L"Rotate right: %s\n"
               L"Ignoring: %zu program(s)\n\n"
               L"Settings: %s\n"
               L"Log: %s\n\n"
               L"Command line: --list, --dry-run, --rotate, --reverse, --selftest, --no-windows",
               HotkeyState(g_settings.rotateLeft,  g_leftOk).c_str(),
               HotkeyState(g_settings.rotateRight, g_rightOk).c_str(),
               g_settings.ignore.size(),
               SettingsPath().c_str(),
               LogFilePath());
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

    case WM_DESTROY:
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
    LogF(L"  winswapper.exe --list      show displays and every window, included or not");
    LogF(L"  winswapper.exe --dry-run   compute the rotation and print it, move nothing");
    LogF(L"  winswapper.exe --rotate    perform one rotation and exit (--swap also works)");
    LogF(L"  winswapper.exe --selftest  verify the remap math and the window-move paths");
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
