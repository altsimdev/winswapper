#pragma once

#include <windows.h>

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
