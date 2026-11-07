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

    // The modifiers (MOD_* bits) that, held while dragging with the left mouse
    // button, move the window under the pointer - and with the right, resize it; held
    // while clicking the middle button, send it to the back. 0 turns that one off.
    UINT moveMods   = 0;
    UINT resizeMods = 0;
    UINT lowerMods  = 0;

    // How near, in pixels at 100% scaling, a dragged window's edge has to come to the
    // edge of a display's work area to snap against it. 0 turns snapping off.
    int snap = 0;

    // Programs whose windows dragging leaves alone, so that the same drag still
    // reaches them; normalised and matched as `ignore` is.
    std::vector<std::wstring> dragSkip;
};

Settings DefaultSettings();

// The most Snap may be: more would make a window cling to an edge it is nowhere near.
const int kMaxSnap = 100;

// "Ctrl+Alt+S" and the like, case-insensitive; "none" turns the hotkey off. At
// least one of Ctrl, Alt or Win is required so a hotkey cannot swallow ordinary
// typing. On failure, `why` says what is wrong in words fit for the user.
bool         ParseHotkey(const std::wstring& text, Hotkey& out, std::wstring& why);
std::wstring FormatHotkey(const Hotkey& h);   // "none" when off

// The same for modifiers alone, such as "Alt" or "Ctrl+Win", as dragging uses them:
// again at least one of Ctrl, Alt or Win, and "none" gives 0.
bool         ParseModifiers(const std::wstring& text, UINT& out, std::wstring& why);
std::wstring FormatModifiers(UINT mods);       // "none" when 0

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
