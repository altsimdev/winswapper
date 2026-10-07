#pragma once

#include <windows.h>

#include <string>
#include <vector>

// A global hotkey: RegisterHotKey modifiers (MOD_*) and a virtual-key code. A vk of
// zero means the hotkey is turned off.
struct Hotkey
{
    UINT mods = 0;
    UINT vk   = 0;

    bool On() const { return vk != 0; }
    bool operator==(const Hotkey& o) const { return mods == o.mods && vk == o.vk; }
};

// What %LOCALAPPDATA%\WinSwapper\settings.ini can change.
struct Settings
{
    Hotkey rotateLeft;
    Hotkey rotateRight;

    // Program file names whose windows are never moved, normalised when read: no
    // directory, and ".exe" added when there is no extension. Matched without
    // regard to case.
    std::vector<std::wstring> ignore;
};

Settings DefaultSettings();

// "Ctrl+Alt+S" and the like, case-insensitive; "none" turns the hotkey off. At
// least one of Ctrl, Alt or Win is required so a hotkey cannot swallow ordinary
// typing. On failure, `why` says what is wrong in words fit for the user.
bool         ParseHotkey(const std::wstring& text, Hotkey& out, std::wstring& why);
std::wstring FormatHotkey(const Hotkey& h);   // "none" when off

// Turns the settings file's text into settings. Anything unusable is described in
// `problems` (with its line number) and that one setting keeps its default; the
// rest still apply.
Settings ParseSettings(const std::wstring& text, std::vector<std::wstring>& problems);

// Decodes a settings file: UTF-16 or UTF-8 with a byte-order mark, UTF-8 without
// one, or - if the bytes are not valid UTF-8 - the system's ANSI code page.
std::wstring DecodeSettingsBytes(const std::vector<unsigned char>& bytes);

std::wstring SettingsPath();

// Reads and parses the file. A missing file is not a problem: it means defaults.
Settings LoadSettings(const std::wstring& path, std::vector<std::wstring>& problems);

// Creates the file with the defaults and comments explaining every setting, unless
// it already exists - an existing file is never overwritten. False on failure.
bool WriteDefaultSettings(const std::wstring& path);
