#pragma once

#include <windows.h>

#include <string>

// The icon resource that pictures this many displays. There are icons for one to
// four; more than four still shows four, which is as many as stay legible at 16
// pixels.
int TrayIconResource(int displays);

// "Start with Windows": a value under HKCU's Run key, so it is per-user and needs no
// administrator rights. Nothing is written until the user turns it on.
//
// The key paths are parameters so the self-test can exercise the real logic in a
// scratch key, without ever touching the user's actual start-up entries.
struct StartupKeys
{
    const wchar_t* run;        // holds the command line Windows runs at sign-in
    const wchar_t* approved;   // where Task Manager records its own enable/disable toggle
    const wchar_t* value;      // the value name used under both
};

extern const StartupKeys kStartupKeys;

// The command line registered to start this copy of the exe: its full path, quoted.
std::wstring StartupCommand();

// True only if this copy will actually start at sign-in: registered, registered for
// this exe rather than a copy elsewhere, and not switched off in Task Manager.
bool IsStartupEnabled(const StartupKeys& keys);

// Returns false and sets the last error on failure.
bool SetStartupEnabled(const StartupKeys& keys, bool enable);
