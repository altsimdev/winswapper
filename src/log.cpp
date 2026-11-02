#include "log.h"

#include <cstdarg>
#include <cwchar>

static LogSink g_sink;
static HANDLE  g_con  = INVALID_HANDLE_VALUE;
static wchar_t g_path[MAX_PATH] = {};

static const wchar_t kTrayLog[] = L"winswapper.log";
static const wchar_t kCliLog[]  = L"winswapper-cli.log";

// The tray log is rolled over to a single .old.log once it passes this size -
// checked at start-up and again before every line, since with Start with Windows on
// the app can run for weeks. Before command-line runs got a file of their own, every
// one of them truncated the shared log, which incidentally kept it small; without
// that, the cap has to be explicit.
static const LONGLONG kTrayLogLimit = 1024 * 1024;

static bool g_tray       = false;
static bool g_rollFailed = false;   // set if renaming failed, so it is not retried per line

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

// Renames the log to .old.log, replacing any previous one. The file must be closed.
static bool RollOver()
{
    wchar_t old[MAX_PATH] = {};
    wcscpy_s(old, g_path);
    wchar_t* dot = wcsrchr(old, L'.');
    if (dot) *dot = L'\0';
    wcscat_s(old, L".old.log");
    return MoveFileExW(g_path, old, MOVEFILE_REPLACE_EXISTING) != FALSE;
}

static std::string ToUtf8(const std::wstring& s)
{
    const int need = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(need > 0 ? need : 0), '\0');
    if (need > 0)
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                            &out[0], need, nullptr, nullptr);
    return out;
}

static std::wstring Stamp()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t b[32];
    swprintf_s(b, L"[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return b;
}

// The system's wording for a Windows error, without its closing full stop.
static std::wstring ErrorText(DWORD err)
{
    wchar_t*    buf = nullptr;
    const DWORD n   = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                     FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr, err, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::wstring s = (n && buf) ? std::wstring(buf, n) : std::wstring(L"unknown error");
    if (buf) LocalFree(buf);
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' ' || s.back() == L'.'))
        s.pop_back();
    return s;
}

// --------------------------------------------------------------- LogSink ---

static void Put(HANDLE file, const std::string& bytes)
{
    DWORD written = 0;
    if (!bytes.empty())
        WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
}

bool LogSink::Open()
{
    if (IsOpen()) return true;

    const DWORD access = append ? FILE_APPEND_DATA : GENERIC_WRITE;
    const DWORD disp   = append ? OPEN_ALWAYS      : CREATE_ALWAYS;
    file = CreateFileW(path.c_str(), access, FILE_SHARE_READ, nullptr, disp,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        const DWORD err = GetLastError();
        if (error == 0) GetLocalTime(&since);   // the start of this spell
        error = err ? err : ERROR_GEN_FAILURE;
        return false;
    }

    LARGE_INTEGER size = {};
    if (GetFileSizeEx(file, &size) && size.QuadPart == 0)
    {
        // BOM so the log opens as UTF-8 in Notepad and Get-Content alike.
        // Written to any new file, not only truncated ones, since the tray
        // log is now created fresh rather than inherited from a CLI run.
        Put(file, "\xEF\xBB\xBF");
    }
    else
    {
        SetFilePointer(file, 0, nullptr, FILE_END);
    }

    if (error != 0)
    {
        // First say what happened, so the kept lines' older timestamps make sense.
        wchar_t note[1024];
        _snwprintf_s(note, _TRUNCATE,
                     L"This log could not be opened from %04u-%02u-%02u %02u:%02u:%02u until now "
                     L"(error %lu: %s). %s",
                     since.wYear, since.wMonth, since.wDay, since.wHour, since.wMinute, since.wSecond,
                     error, ErrorText(error).c_str(),
                     kept.empty() ? L"Nothing was logged meanwhile." : L"What was logged meanwhile follows.");
        std::wstring line = Stamp() + note;
        if (dropped > 0)
        {
            wchar_t more[128];
            swprintf_s(more, L" %zu more line(s) did not fit in memory and are lost.", dropped);
            line += more;
        }
        Put(file, ToUtf8(line + L"\r\n"));

        for (const std::string& k : kept) Put(file, k);
        kept.clear();
        keptBytes = 0;
        dropped   = 0;
        error     = 0;
    }
    return true;
}

void LogSink::Write(const std::wstring& line)
{
    std::string bytes = ToUtf8(line);
    if (Open())
    {
        Put(file, bytes);
        return;
    }

    // Keeps the earliest lines: they show what the app was doing when the file
    // was first found shut, and the cap only matters if it stays shut for days.
    if (keptBytes + bytes.size() <= keepBytes)
    {
        keptBytes += bytes.size();
        kept.push_back(std::move(bytes));
    }
    else
    {
        ++dropped;
    }
}

void LogSink::Close()
{
    if (IsOpen())
    {
        CloseHandle(file);
        file = INVALID_HANDLE_VALUE;
    }
}

std::wstring LogSink::Problem() const
{
    if (IsOpen() || error == 0) return std::wstring();

    wchar_t b[1024];
    _snwprintf_s(b, _TRUNCATE,
                 L"Not written since %04u-%02u-%02u %02u:%02u:%02u: the file could not be opened "
                 L"(error %lu: %s). WinSwapper keeps trying, and keeps what it logs until then.",
                 since.wYear, since.wMonth, since.wDay, since.wHour, since.wMinute, since.wSecond,
                 error, ErrorText(error).c_str());
    return b;
}

// ------------------------------------------------------------------ log ----

// Tray mode only. Called with the file closed (at start-up) or open (before a line).
static void RollOverIfLarge()
{
    if (g_rollFailed) return;

    LONGLONG size = 0;
    if (g_sink.IsOpen())
    {
        LARGE_INTEGER li = {};
        if (!GetFileSizeEx(g_sink.file, &li)) return;
        size = li.QuadPart;
    }
    else
    {
        WIN32_FILE_ATTRIBUTE_DATA fa = {};
        if (!GetFileAttributesExW(g_path, GetFileExInfoStandard, &fa)) return;
        size = (static_cast<LONGLONG>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    }
    if (size <= kTrayLogLimit) return;

    // A file that is open cannot be renamed, so close it, roll, and reopen. If the
    // rename fails - something holding .old.log, say - keep appending and stop
    // trying, rather than closing and reopening the file for every line.
    const bool wasOpen = g_sink.IsOpen();
    g_sink.Close();
    if (!RollOver()) g_rollFailed = true;
    if (wasOpen) g_sink.Open();
}

void LogInit(LogMode mode)
{
    const bool cli = (mode == LogMode::Cli);
    g_tray = !cli;
    BuildPath(cli ? kCliLog : kTrayLog);
    g_sink.path   = g_path;
    g_sink.append = !cli;
    if (g_tray) RollOverIfLarge();
    g_sink.Open();

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
    // One last try, so lines kept while the file was shut are not lost needlessly.
    // The path goes too: a line logged after this must not reopen the file, which
    // for a command-line run would start it afresh and wipe the run's output.
    if (!g_sink.path.empty()) g_sink.Open();
    g_sink.Close();
    g_sink.path.clear();
    if (g_con != INVALID_HANDLE_VALUE) { CloseHandle(g_con); g_con = INVALID_HANDLE_VALUE; }
}

const wchar_t* LogFilePath()
{
    if (g_path[0] == L'\0') BuildPath(kTrayLog);
    return g_path;
}

bool LogRetry()
{
    return !g_sink.path.empty() && g_sink.Open();
}

std::wstring LogProblem()
{
    return g_sink.Problem();
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

    if (g_sink.path.empty()) return;   // before LogInit
    if (g_tray) RollOverIfLarge();
    g_sink.Write(Stamp() + body + L"\r\n");
}
