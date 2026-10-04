#include "log.h"

#include <cstdarg>
#include <cwchar>
#include <vector>

static HANDLE  g_file = INVALID_HANDLE_VALUE;
static HANDLE  g_con  = INVALID_HANDLE_VALUE;
static wchar_t g_path[MAX_PATH] = {};

static const wchar_t kTrayLog[] = L"winswapper.log";
static const wchar_t kCliLog[]  = L"winswapper-cli.log";

// The tray log is never truncated by a run, so it is rolled over to a single
// .old.log instead once it passes this size. Checked at start-up only.
static const LONGLONG kTrayLogLimit = 1024 * 1024;

static void BuildPath(const wchar_t* name)
{
    wchar_t base[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH) == 0)
        GetTempPathW(MAX_PATH, base);

    wchar_t dir[MAX_PATH] = {};
    swprintf_s(dir, L"%s\\WinSwapper", base);
    CreateDirectoryW(dir, nullptr);

    swprintf_s(g_path, L"%s\\%s", dir, name);
}

// Before command-line runs got a file of their own, every one of them truncated
// the shared log, which incidentally kept it small. Without that, an app that
// can run for weeks needs an explicit cap.
static void RollOverIfLarge()
{
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    if (!GetFileAttributesExW(g_path, GetFileExInfoStandard, &fa)) return;

    const LONGLONG size = (static_cast<LONGLONG>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    if (size <= kTrayLogLimit) return;

    wchar_t old[MAX_PATH] = {};
    swprintf_s(old, L"%s", g_path);
    wchar_t* dot = wcsrchr(old, L'.');
    if (dot) *dot = L'\0';
    wcscat_s(old, L".old.log");
    MoveFileExW(g_path, old, MOVEFILE_REPLACE_EXISTING);
}

void LogInit(LogMode mode)
{
    const bool cli = (mode == LogMode::Cli);
    BuildPath(cli ? kCliLog : kTrayLog);
    if (!cli) RollOverIfLarge();

    const DWORD access = cli ? GENERIC_WRITE : FILE_APPEND_DATA;
    const DWORD disp   = cli ? CREATE_ALWAYS : OPEN_ALWAYS;

    g_file = CreateFileW(g_path, access, FILE_SHARE_READ, nullptr, disp,
                         FILE_ATTRIBUTE_NORMAL, nullptr);

    if (g_file != INVALID_HANDLE_VALUE)
    {
        LARGE_INTEGER size = {};
        if (GetFileSizeEx(g_file, &size) && size.QuadPart == 0)
        {
            // BOM so the log opens as UTF-8 in Notepad and Get-Content alike.
            // Written to any new file, not only truncated ones, since the tray
            // log is now created fresh rather than inherited from a CLI run.
            const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
            DWORD written = 0;
            WriteFile(g_file, bom, sizeof(bom), &written, nullptr);
        }
        else
        {
            SetFilePointer(g_file, 0, nullptr, FILE_END);
        }
    }

    if (cli && AttachConsole(ATTACH_PARENT_PROCESS))
    {
        // The CRT's stdout is not wired up in a GUI-subsystem process, so talk to
        // the console device directly.
        g_con = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, 0, nullptr);
    }
}

void LogShutdown()
{
    if (g_file != INVALID_HANDLE_VALUE) { CloseHandle(g_file); g_file = INVALID_HANDLE_VALUE; }
    if (g_con  != INVALID_HANDLE_VALUE) { CloseHandle(g_con);  g_con  = INVALID_HANDLE_VALUE; }
}

const wchar_t* LogFilePath()
{
    if (g_path[0] == L'\0') BuildPath(kTrayLog);
    return g_path;
}

void LogF(const wchar_t* fmt, ...)
{
    wchar_t body[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(body, _countof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    if (g_con != INVALID_HANDLE_VALUE)
    {
        DWORD n = 0;
        WriteConsoleW(g_con, body, static_cast<DWORD>(wcslen(body)), &n, nullptr);
        WriteConsoleW(g_con, L"\r\n", 2, &n, nullptr);
    }

    if (g_file != INVALID_HANDLE_VALUE)
    {
        SYSTEMTIME st;
        GetLocalTime(&st);

        wchar_t line[2176];
        swprintf_s(line, L"[%02u:%02u:%02u.%03u] %s\r\n",
                   st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, body);

        const int need = WideCharToMultiByte(CP_UTF8, 0, line, -1, nullptr, 0, nullptr, nullptr);
        if (need > 1)
        {
            std::vector<char> utf8(static_cast<size_t>(need));
            WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8.data(), need, nullptr, nullptr);
            DWORD written = 0;
            WriteFile(g_file, utf8.data(), static_cast<DWORD>(need - 1), &written, nullptr);
        }
    }
}
