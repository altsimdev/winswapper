#include "settings.h"

#include <cstdlib>
#include <cwctype>

namespace
{

const wchar_t kDefaultText[] =
    L"; WinSwapper settings. Save this file, then choose \"Reload settings\" from the\r\n"
    L"; tray menu (or restart WinSwapper) to apply it.\r\n"
    L"\r\n"
    L"[Hotkeys]\r\n"
    L"; Modifiers and a key joined with +, such as Ctrl+Alt+S. At least one of Ctrl,\r\n"
    L"; Alt or Win is required, so a hotkey can never swallow ordinary typing.\r\n"
    L";   Modifiers: Ctrl, Alt, Shift, Win\r\n"
    L";   Keys:      A-Z, 0-9, F1-F24, Left, Right, Up, Down, Home, End, PageUp,\r\n"
    L";              PageDown, Insert, Delete, Space, Tab, Enter, Esc\r\n"
    L"; Write \"none\" to turn a hotkey off.\r\n"
    L"RotateLeft  = Ctrl+Alt+S\r\n"
    L"RotateRight = Ctrl+Alt+Shift+S\r\n"
    L"\r\n"
    L"[Ignore]\r\n"
    L"; Programs whose windows are never moved, one per line, named as in the Details\r\n"
    L"; tab of Task Manager. \".exe\" may be left off. For example:\r\n"
    L"; slack.exe\r\n";

struct NamedKey
{
    const wchar_t* name;
    UINT           vk;
};

// The first name listed for a key is the one FormatHotkey writes back.
const NamedKey kNamedKeys[] = {
    { L"Left",   VK_LEFT   }, { L"Right",    VK_RIGHT  }, { L"Up",     VK_UP     },
    { L"Down",   VK_DOWN   }, { L"Home",     VK_HOME   }, { L"End",    VK_END    },
    { L"PageUp", VK_PRIOR  }, { L"PageDown", VK_NEXT   }, { L"Insert", VK_INSERT },
    { L"Delete", VK_DELETE }, { L"Space",    VK_SPACE  }, { L"Tab",    VK_TAB    },
    { L"Enter",  VK_RETURN }, { L"Esc",      VK_ESCAPE }, { L"Escape", VK_ESCAPE },
};

bool Same(const std::wstring& a, const wchar_t* b)
{
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b, -1, TRUE) == CSTR_EQUAL;
}

std::wstring Trim(const std::wstring& s)
{
    // U+FEFF too, in case a byte-order mark survives into the text somehow.
    const wchar_t* ws = L" \t\r\n\xFEFF";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::wstring::npos) return std::wstring();
    const size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

UINT KeyFromName(const std::wstring& t)
{
    if (t.size() == 1)
    {
        const wchar_t c = static_cast<wchar_t>(towupper(t[0]));
        if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) return c;
        return 0;
    }
    if ((t[0] == L'F' || t[0] == L'f') && t.size() <= 3 &&
        t.find_first_not_of(L"0123456789", 1) == std::wstring::npos)
    {
        const int n = _wtoi(t.c_str() + 1);
        if (n >= 1 && n <= 24) return static_cast<UINT>(VK_F1 + n - 1);
        return 0;
    }
    for (const NamedKey& k : kNamedKeys)
        if (Same(t, k.name)) return k.vk;
    return 0;
}

std::wstring NameOfKey(UINT vk)
{
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        return std::wstring(1, static_cast<wchar_t>(vk));
    if (vk >= VK_F1 && vk <= VK_F24)
        return L"F" + std::to_wstring(vk - VK_F1 + 1);
    for (const NamedKey& k : kNamedKeys)
        if (k.vk == vk) return k.name;
    return L"?";
}

// "C:\Program Files\Slack\slack.exe", "Slack" and "slack.exe" all become a bare
// file name, with ".exe" added when there is no extension at all.
std::wstring NormalizeProgram(std::wstring s)
{
    s = Trim(s);
    if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') s = Trim(s.substr(1, s.size() - 2));
    const size_t slash = s.find_last_of(L"\\/");
    if (slash != std::wstring::npos) s = s.substr(slash + 1);
    if (!s.empty() && s.find(L'.') == std::wstring::npos) s += L".exe";
    return s;
}

std::wstring LineNo(int n)
{
    return L"line " + std::to_wstring(n) + L": ";
}

} // namespace

Settings DefaultSettings()
{
    Settings s;
    s.rotateLeft  = { MOD_CONTROL | MOD_ALT,             'S' };
    s.rotateRight = { MOD_CONTROL | MOD_ALT | MOD_SHIFT, 'S' };
    return s;
}

bool ParseHotkey(const std::wstring& text, Hotkey& out, std::wstring& why)
{
    const std::wstring t = Trim(text);
    if (Same(t, L"none") || Same(t, L"off"))
    {
        out = Hotkey{};
        return true;
    }
    if (t.empty())
    {
        why = L"is empty (write none to turn it off)";
        return false;
    }

    UINT         mods = 0;
    UINT         vk   = 0;
    std::wstring keyName;

    size_t pos = 0;
    while (pos <= t.size())
    {
        size_t plus = t.find(L'+', pos);
        if (plus == std::wstring::npos) plus = t.size();
        const std::wstring tok = Trim(t.substr(pos, plus - pos));
        pos = plus + 1;

        if (tok.empty())                                  { why = L"has an empty part between + signs"; return false; }
        if      (Same(tok, L"Ctrl")  || Same(tok, L"Control")) mods |= MOD_CONTROL;
        else if (Same(tok, L"Alt"))                            mods |= MOD_ALT;
        else if (Same(tok, L"Shift"))                          mods |= MOD_SHIFT;
        else if (Same(tok, L"Win")   || Same(tok, L"Windows")) mods |= MOD_WIN;
        else
        {
            const UINT k = KeyFromName(tok);
            if (!k)
            {
                why = L"has '" + tok + L"', which is neither a modifier nor a key it knows";
                return false;
            }
            if (vk)
            {
                why = L"names two keys, " + keyName + L" and " + tok + L"; a hotkey has one";
                return false;
            }
            vk      = k;
            keyName = tok;
        }
    }

    if (!vk)
    {
        why = L"has modifiers but no key";
        return false;
    }
    if (!(mods & (MOD_CONTROL | MOD_ALT | MOD_WIN)))
    {
        why = L"needs Ctrl, Alt or Win, or it would take over ordinary typing";
        return false;
    }

    out.mods = mods;
    out.vk   = vk;
    return true;
}

std::wstring FormatHotkey(const Hotkey& h)
{
    if (!h.On()) return L"none";
    std::wstring s;
    if (h.mods & MOD_CONTROL) s += L"Ctrl+";
    if (h.mods & MOD_ALT)     s += L"Alt+";
    if (h.mods & MOD_SHIFT)   s += L"Shift+";
    if (h.mods & MOD_WIN)     s += L"Win+";
    return s + NameOfKey(h.vk);
}

Settings ParseSettings(const std::wstring& text, std::vector<std::wstring>& problems)
{
    Settings s = DefaultSettings();

    enum class Section { None, Hotkeys, Ignore, Unknown };
    Section section = Section::None;

    size_t pos    = 0;
    int    lineNo = 0;
    while (pos <= text.size())
    {
        size_t eol = text.find(L'\n', pos);
        if (eol == std::wstring::npos) eol = text.size();
        const std::wstring line = Trim(text.substr(pos, eol - pos));
        pos = eol + 1;
        ++lineNo;

        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;

        if (line[0] == L'[')
        {
            if (line.back() != L']')
            {
                problems.push_back(LineNo(lineNo) + L"a section heading needs a closing ]");
                section = Section::Unknown;
                continue;
            }
            const std::wstring name = Trim(line.substr(1, line.size() - 2));
            if      (Same(name, L"Hotkeys")) section = Section::Hotkeys;
            else if (Same(name, L"Ignore"))  section = Section::Ignore;
            else
            {
                problems.push_back(LineNo(lineNo) + L"unknown section [" + name + L"]; its lines are skipped");
                section = Section::Unknown;
            }
            continue;
        }

        switch (section)
        {
        case Section::None:
            problems.push_back(LineNo(lineNo) + L"'" + line + L"' is not inside a [section]");
            break;

        case Section::Unknown:
            break;   // the heading has been reported already

        case Section::Hotkeys:
        {
            const size_t eq = line.find(L'=');
            if (eq == std::wstring::npos)
            {
                problems.push_back(LineNo(lineNo) + L"expected Name = value, such as RotateLeft = Ctrl+Alt+S");
                break;
            }
            const std::wstring key   = Trim(line.substr(0, eq));
            const std::wstring value = Trim(line.substr(eq + 1));

            Hotkey* target = Same(key, L"RotateLeft")  ? &s.rotateLeft
                           : Same(key, L"RotateRight") ? &s.rotateRight
                           : nullptr;
            if (!target)
            {
                problems.push_back(LineNo(lineNo) + L"unknown setting '" + key +
                                   L"' (expected RotateLeft or RotateRight)");
                break;
            }

            Hotkey       h;
            std::wstring why;
            if (ParseHotkey(value, h, why)) *target = h;
            else
                problems.push_back(LineNo(lineNo) + key + L" '" + value + L"' " + why +
                                   L"; using " + FormatHotkey(*target));
            break;
        }

        case Section::Ignore:
            if (line.find(L'=') != std::wstring::npos)
            {
                problems.push_back(LineNo(lineNo) + L"expected just a program name, such as slack.exe");
                break;
            }
            s.ignore.push_back(NormalizeProgram(line));
            break;
        }
    }

    // RegisterHotKey would refuse the second of two identical combinations anyway;
    // this says why instead of leaving it to look like another app has taken it.
    if (s.rotateLeft.On() && s.rotateLeft == s.rotateRight)
    {
        problems.push_back(L"RotateRight is the same as RotateLeft (" + FormatHotkey(s.rotateLeft) +
                           L"), so it is turned off");
        s.rotateRight = Hotkey{};
    }
    return s;
}

std::wstring DecodeSettingsBytes(const std::vector<unsigned char>& b)
{
    if (b.size() >= 2 && b[0] == 0xFF && b[1] == 0xFE)
        return std::wstring(reinterpret_cast<const wchar_t*>(b.data() + 2), (b.size() - 2) / 2);

    const size_t start = (b.size() >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) ? 3 : 0;
    const int    n     = static_cast<int>(b.size() - start);
    if (n <= 0) return std::wstring();

    const char* p = reinterpret_cast<const char*>(b.data() + start);

    // Strict UTF-8 first; bytes that are not valid UTF-8 most likely came from an
    // older editor saving in the ANSI code page, so read them that way instead.
    UINT  cp    = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int   len   = MultiByteToWideChar(cp, flags, p, n, nullptr, 0);
    if (len == 0)
    {
        cp    = CP_ACP;
        flags = 0;
        len   = MultiByteToWideChar(cp, flags, p, n, nullptr, 0);
    }

    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(cp, flags, p, n, out.data(), len);
    return out;
}

std::wstring SettingsPath()
{
    wchar_t base[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH) == 0)
        GetTempPathW(MAX_PATH, base);
    return std::wstring(base) + L"\\WinSwapper\\settings.ini";
}

Settings LoadSettings(const std::wstring& path, std::vector<std::wstring>& problems)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
    {
        const DWORD err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND)
            problems.push_back(L"could not read " + path + L" (error " + std::to_wstring(err) +
                               L"); using the defaults");
        return DefaultSettings();
    }

    std::vector<unsigned char> bytes;
    LARGE_INTEGER size = {};
    // A settings file is a few hundred bytes; anything vast is not one.
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < 1024 * 1024)
    {
        bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        if (!ReadFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr)) got = 0;
        bytes.resize(got);
    }
    CloseHandle(f);

    return ParseSettings(DecodeSettingsBytes(bytes), problems);
}

bool WriteDefaultSettings(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L'\\');
    if (slash != std::wstring::npos) CreateDirectoryW(path.substr(0, slash).c_str(), nullptr);

    // CREATE_NEW: never overwrite a file the user may have edited.
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_EXISTS;

    const int need = WideCharToMultiByte(CP_UTF8, 0, kDefaultText, -1, nullptr, 0, nullptr, nullptr);
    std::vector<char> utf8(static_cast<size_t>(need));
    WideCharToMultiByte(CP_UTF8, 0, kDefaultText, -1, utf8.data(), need, nullptr, nullptr);

    // BOM first, so Notepad and other editors open it as UTF-8.
    const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
    DWORD written = 0;
    const bool ok = WriteFile(f, bom, sizeof(bom), &written, nullptr) &&
                    WriteFile(f, utf8.data(), static_cast<DWORD>(need - 1), &written, nullptr);
    CloseHandle(f);
    return ok;
}
