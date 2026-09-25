#include "log.h"

#include <cstdarg>
#include <cwchar>
#include <vector>

static HANDLE  g_file = INVALID_HANDLE_VALUE;
static HANDLE  g_con  = INVALID_HANDLE_VALUE;
static wchar_t g_path[MAX_PATH] = {};

static void BuildPath()
{
    wchar_t base[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH) == 0)
        GetTempPathW(MAX_PATH, base);

    wchar_t dir[MAX_PATH] = {};
    swprintf_s(dir, L"%s\\WinSwapper", base);
    CreateDirectoryW(dir, nullptr);

    swprintf_s(g_path, L"%s\\winswapper.log", dir);
}

void LogInit(bool truncate, bool echoConsole)
{
    BuildPath();

    const DWORD access = truncate ? GENERIC_WRITE : FILE_APPEND_DATA;
    const DWORD disp   = truncate ? CREATE_ALWAYS : OPEN_ALWAYS;

    g_file = CreateFileW(g_path, access, FILE_SHARE_READ, nullptr, disp,
                         FILE_ATTRIBUTE_NORMAL, nullptr);

    if (g_file != INVALID_HANDLE_VALUE)
    {
        if (truncate)
        {
            // BOM so the log opens as UTF-8 in Notepad and Get-Content alike.
            const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
            DWORD written = 0;
            WriteFile(g_file, bom, sizeof(bom), &written, nullptr);
        }
        else
        {
            SetFilePointer(g_file, 0, nullptr, FILE_END);
        }
    }

    if (echoConsole && AttachConsole(ATTACH_PARENT_PROCESS))
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
    if (g_path[0] == L'\0') BuildPath();
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
