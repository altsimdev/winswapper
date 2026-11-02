#pragma once

#include <windows.h>

#include <string>
#include <vector>

// Where log output goes. The two modes write separate files under
// %LOCALAPPDATA%\WinSwapper. The tray app holds its file open for as long as it
// runs, so sharing one file let whichever process opened it first lock the other
// out - and a command-line run would silently lose all of its output.
enum class LogMode
{
    Tray,   // winswapper.log: appended to across runs, and capped in size
    Cli,    // winswapper-cli.log: fresh for every run, and mirrored to the console
};

void LogInit(LogMode mode);
void LogShutdown();
void LogF(const wchar_t* fmt, ...);

const wchar_t* LogFilePath();

// Opening the log can fail - another program holding the file, say - and it used
// to be tried once, at start-up, so a whole run could go unlogged without a word.
// Now every later line tries again, and so does LogRetry(); what is logged
// meanwhile is kept, and written once the file opens, after a note saying why it
// could not be opened before.
//
// LogRetry() returns true if the log is open now. LogProblem() is empty while the
// log is being written; otherwise it says, in a sentence, since when and why not.
bool         LogRetry();
std::wstring LogProblem();

// The file underneath, exposed so the self-test can hold one locked and watch it
// recover. Lines are passed whole: timestamped, and with their line ending.
struct LogSink
{
    std::wstring path;
    bool         append    = true;          // add to the file; false starts it afresh
    size_t       keepBytes = 256 * 1024;    // what is kept while the file is shut

    // Each returns at once if the file is already open. Write() tries to open it
    // first, so a shut file is tried again on every line.
    bool         Open();
    void         Write(const std::wstring& line);
    void         Close();
    bool         IsOpen() const { return file != INVALID_HANDLE_VALUE; }
    std::wstring Problem() const;

    HANDLE                   file      = INVALID_HANDLE_VALUE;
    DWORD                    error     = 0;     // why it last failed to open; 0 once open
    SYSTEMTIME               since     = {};    // when it was first found shut, this time
    std::vector<std::string> kept;              // UTF-8 lines waiting for the file
    size_t                   keptBytes = 0;
    size_t                   dropped   = 0;     // lines that did not fit in keepBytes
};
