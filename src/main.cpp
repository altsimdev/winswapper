#include <windows.h>
#include <shellapi.h>

#include <string>

#include "log.h"
#include "resource.h"
#include "swapper.h"

#define WM_TRAYICON (WM_APP + 1)

static const UINT kHotkeyId = 1;
static const UINT kTrayId   = 1;

static HINSTANCE g_inst           = nullptr;
static HWND      g_hwnd           = nullptr;
static HICON     g_icon           = nullptr;
static UINT      g_taskbarCreated = 0;
static bool      g_hotkeyOk       = false;
static HANDLE    g_mutex          = nullptr;

// ------------------------------------------------------------------ tray ----

static void AddTrayIcon()
{
    NOTIFYICONDATAW nid = {};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = g_hwnd;
    nid.uID              = kTrayId;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon            = g_icon;
    wcscpy_s(nid.szTip, L"WinSwapper - Ctrl+Alt+S rotates windows one display left");

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

static void DoSwap()
{
    const SwapResult r = PerformSwap(g_hwnd, false);

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

static void OpenLog()
{
    ShellExecuteW(nullptr, nullptr, L"notepad.exe", LogFilePath(), nullptr, SW_SHOWNORMAL);
}

static void ShowAbout()
{
    wchar_t msg[768];
    swprintf_s(msg,
               L"WinSwapper 1.1\n\n"
               L"Moves every ordinary window one display to the left, with the leftmost "
               L"display wrapping around to the rightmost. Each window keeps its size and "
               L"its position within its display.\n\n"
               L"With two displays that is a straight swap, so pressing twice puts "
               L"everything back; with N displays it takes N presses.\n\n"
               L"Hotkey: %s\n"
               L"Log: %s\n\n"
               L"Command line: --list, --dry-run, --rotate, --selftest",
               g_hotkeyOk ? L"Ctrl+Alt+S" : L"Ctrl+Alt+S (UNAVAILABLE - taken by another app)",
               LogFilePath());
    MessageBoxW(nullptr, msg, L"About WinSwapper", MB_OK | MB_ICONINFORMATION);
}

static void ShowMenu()
{
    POINT pt;
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    AppendMenuW(menu, MF_STRING, IDM_SWAP,  L"&Rotate now\tCtrl+Alt+S");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
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
        case WM_RBUTTONUP:     ShowMenu(); break;
        case WM_LBUTTONDBLCLK: DoSwap();   break;
        default: break;
        }
        return 0;

    case WM_HOTKEY:
        if (wp == kHotkeyId) DoSwap();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDM_SWAP:  DoSwap();            break;
        case IDM_LOG:   OpenLog();           break;
        case IDM_ABOUT: ShowAbout();         break;
        case IDM_EXIT:  DestroyWindow(hwnd); break;
        default: break;
        }
        return 0;

    case WM_DISPLAYCHANGE:
        LogF(L"Display configuration changed.");
        return 0;

    case WM_DESTROY:
        RemoveTrayIcon();
        if (g_hotkeyOk) UnregisterHotKey(hwnd, kHotkeyId);
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
    LogF(L"WinSwapper - rotate windows one display to the left.");
    LogF(L"");
    LogF(L"  winswapper.exe             run in the notification area (Ctrl+Alt+S rotates)");
    LogF(L"  winswapper.exe --list      show displays and every window, included or not");
    LogF(L"  winswapper.exe --dry-run   compute the rotation and print it, move nothing");
    LogF(L"  winswapper.exe --rotate    perform one rotation and exit (--swap also works)");
    LogF(L"  winswapper.exe --selftest  verify the remap math and the window-move paths");
    LogF(L"");
    LogF(L"Log file: %s", LogFilePath());
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int)
{
    g_inst = inst;

    bool doList = false, doDry = false, doSwapOnce = false, doSelfTest = false, doHelp = false;
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
            else if (a == L"--swap")     doSwapOnce = true;   // the pre-1.1 spelling
            else if (a == L"--selftest") doSelfTest = true;
            else if (a == L"--help" || a == L"-h" || a == L"/?") doHelp = true;
            else                         badArg     = true;
        }
        LocalFree(argv);
    }

    if (doHelp || badArg || doList || doDry || doSwapOnce || doSelfTest)
    {
        LogInit(true, true);

        int rc = 0;
        if (doHelp || badArg)
        {
            if (badArg) LogF(L"Unrecognised argument.");
            PrintUsage();
            rc = badArg ? 2 : 0;
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
            PerformSwap(nullptr, doDry);
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

    LogInit(false, false);
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

    g_icon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                           GetSystemMetrics(SM_CXSMICON),
                                           GetSystemMetrics(SM_CYSMICON),
                                           LR_DEFAULTCOLOR));
    if (!g_icon) g_icon = LoadIconW(nullptr, IDI_APPLICATION);

    AddTrayIcon();

    g_hotkeyOk = RegisterHotKey(g_hwnd, kHotkeyId,
                                MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'S') != FALSE;
    if (!g_hotkeyOk)
    {
        LogF(L"RegisterHotKey(Ctrl+Alt+S) failed, error %lu.", GetLastError());
        Balloon(L"Ctrl+Alt+S is already taken by another app.\n"
                L"Use the tray menu to rotate.", NIIF_WARNING);
    }
    else
    {
        LogF(L"Hotkey Ctrl+Alt+S registered.");
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    LogF(L"WinSwapper exited.");
    LogShutdown();

    if (g_mutex)
    {
        ReleaseMutex(g_mutex);
        CloseHandle(g_mutex);
    }
    return 0;
}
