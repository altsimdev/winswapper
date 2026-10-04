#include <windows.h>
#include <shellapi.h>

#include <string>

#include "log.h"
#include "resource.h"
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
static bool      g_leftOk         = false;
static bool      g_rightOk        = false;
static HANDLE    g_mutex          = nullptr;

// ------------------------------------------------------------------ tray ----

static void FillTip(NOTIFYICONDATAW& nid)
{
    swprintf_s(nid.szTip, L"WinSwapper - %d display%s - Ctrl+Alt+S rotates left, +Shift right",
               g_iconDisplays, g_iconDisplays == 1 ? L"" : L"s");
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
    wcscpy_s(nid.szInfo, text);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// ---------------------------------------------------------------- actions ---

static void DoRotate(Rotation dir)
{
    const SwapResult r = PerformSwap(g_hwnd, false, dir);

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

static void ShowAbout()
{
    wchar_t msg[1024];
    swprintf_s(msg,
               L"WinSwapper 1.04\n\n"
               L"Moves every ordinary window one display to the left or the right, with "
               L"the end display wrapping around. Each window keeps its size and its "
               L"position within its display.\n\n"
               L"The two directions are inverses, so one of each puts everything back - "
               L"pixel-exact when the displays share a resolution. With two displays both "
               L"do the same swap; with N displays, N presses the same way also completes "
               L"the cycle.\n\n"
               L"Rotate left:  %s\n"
               L"Rotate right: %s\n"
               L"Log: %s\n\n"
               L"Command line: --list, --dry-run, --rotate, --reverse, --selftest",
               g_leftOk  ? L"Ctrl+Alt+S"
                         : L"Ctrl+Alt+S (UNAVAILABLE - taken by another app)",
               g_rightOk ? L"Ctrl+Alt+Shift+S"
                         : L"Ctrl+Alt+Shift+S (UNAVAILABLE - taken by another app)",
               LogFilePath());
    MessageBoxW(nullptr, msg, L"About WinSwapper", MB_OK | MB_ICONINFORMATION);
}

static void ShowMenu()
{
    POINT pt;
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    // Mnemonics must stay unique across the whole menu - F, R, W, L, A, X - or Windows
    // only cycles the highlight between the clashing items instead of invoking one.
    AppendMenuW(menu, MF_STRING, IDM_LEFT,  L"Rotate le&ft\tCtrl+Alt+S");
    AppendMenuW(menu, MF_STRING, IDM_RIGHT, L"Rotate &right\tCtrl+Alt+Shift+S");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    // Read fresh each time the menu opens, so a change made in Task Manager or by
    // another copy shows up here.
    AppendMenuW(menu, MF_STRING | (IsStartupEnabled(kStartupKeys) ? MF_CHECKED : MF_UNCHECKED),
                IDM_STARTUP, L"Start with &Windows");
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
        if (g_leftOk)  UnregisterHotKey(hwnd, kHotkeyLeft);
        if (g_rightOk) UnregisterHotKey(hwnd, kHotkeyRight);
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
    LogF(L"                             Ctrl+Alt+S rotates left, Ctrl+Alt+Shift+S right");
    LogF(L"  winswapper.exe --list      show displays and every window, included or not");
    LogF(L"  winswapper.exe --dry-run   compute the rotation and print it, move nothing");
    LogF(L"  winswapper.exe --rotate    perform one rotation and exit (--swap also works)");
    LogF(L"  winswapper.exe --selftest  verify the remap math and the window-move paths");
    LogF(L"");
    LogF(L"  --reverse, --right         with --rotate or --dry-run, rotate right instead;");
    LogF(L"                             an error with anything else");
    LogF(L"");
    LogF(L"--selftest exits 0 if every check ran and passed, 3 if it passed but skipped");
    LogF(L"the window-move tests for want of a second display, and 1 on a failure.");
    LogF(L"Usage errors exit 2.");
    LogF(L"");
    LogF(L"Log file: %s", LogFilePath());
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int)
{
    g_inst = inst;

    bool doList = false, doDry = false, doSwapOnce = false, doSelfTest = false, doHelp = false;
    bool reverse = false;
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
            // A modifier, not a mode: it picks the direction for --rotate/--dry-run.
            else if (a == L"--reverse" || a == L"--right") reverse = true;
            else if (a == L"--help" || a == L"-h" || a == L"/?") doHelp = true;
            else                         badArg     = true;
        }
        LocalFree(argv);
    }

    // --reverse only means something alongside --rotate or --dry-run. On its own it
    // would otherwise fall through to tray mode and be silently ignored.
    const bool strayReverse = reverse && !doDry && !doSwapOnce;

    if (doHelp || badArg || strayReverse || doList || doDry || doSwapOnce || doSelfTest)
    {
        LogInit(LogMode::Cli);

        int rc = 0;
        if (doHelp || badArg || strayReverse)
        {
            if (badArg)            LogF(L"Unrecognised argument.");
            else if (strayReverse) LogF(L"--reverse needs --rotate or --dry-run to act on.");
            PrintUsage();
            rc = (badArg || strayReverse) ? 2 : 0;
        }
        else if (doSelfTest)
        {
            rc = SelfTest();
        }
        else if (doList)
        {
            ListAll(nullptr);
        }
        else
        {
            PerformSwap(nullptr, doDry, reverse ? Rotation::Right : Rotation::Left);
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

    // Registered separately so losing one combination to another app still leaves
    // the other working. Ctrl+Alt+S and Ctrl+Alt+Shift+S are distinct to
    // RegisterHotKey, so holding Shift picks the reverse rotation on its own.
    g_leftOk  = RegisterHotKey(g_hwnd, kHotkeyLeft,
                               MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'S') != FALSE;
    if (!g_leftOk)
        LogF(L"RegisterHotKey(Ctrl+Alt+S) failed, error %lu.", GetLastError());

    g_rightOk = RegisterHotKey(g_hwnd, kHotkeyRight,
                               MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'S') != FALSE;
    if (!g_rightOk)
        LogF(L"RegisterHotKey(Ctrl+Alt+Shift+S) failed, error %lu.", GetLastError());

    if (g_leftOk && g_rightOk)
    {
        LogF(L"Hotkeys registered: Ctrl+Alt+S rotates left, Ctrl+Alt+Shift+S rotates right.");
    }
    else if (!g_leftOk && !g_rightOk)
    {
        Balloon(L"Both Ctrl+Alt+S and Ctrl+Alt+Shift+S are taken by another app.\n"
                L"Use the tray menu to rotate.", NIIF_WARNING);
    }
    else
    {
        Balloon(g_leftOk ? L"Ctrl+Alt+Shift+S is taken by another app.\n"
                           L"Rotating right is still on the tray menu."
                         : L"Ctrl+Alt+S is taken by another app.\n"
                           L"Rotating left is still on the tray menu.",
                NIIF_WARNING);
    }

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
