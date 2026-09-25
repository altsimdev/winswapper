#pragma once

#include <windows.h>

// Logging goes to %LOCALAPPDATA%\WinSwapper\winswapper.log, and optionally to the
// console that launched us (the CLI modes attach to the parent console).
//
// truncate    - start a fresh file (CLI modes) instead of appending (tray mode).
// echoConsole - also mirror output to the invoking console.
void LogInit(bool truncate, bool echoConsole);
void LogShutdown();
void LogF(const wchar_t* fmt, ...);

const wchar_t* LogFilePath();
