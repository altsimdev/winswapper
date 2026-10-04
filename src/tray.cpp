#include "tray.h"

#include "resource.h"

#include <vector>

int TrayIconResource(int displays)
{
    if (displays <= 1) return IDI_DISPLAYS_1;
    if (displays == 2) return IDI_DISPLAYS_2;
    if (displays == 3) return IDI_DISPLAYS_3;
    return IDI_DISPLAYS_4;
}

// ------------------------------------------------------- start with Windows ---

const StartupKeys kStartupKeys = {
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run",
    L"WinSwapper",
};

static std::wstring ExePath()
{
    // MAX_PATH is not a real limit on where an exe can live, so grow until it fits.
    std::vector<wchar_t> buf(MAX_PATH);
    while (buf.size() <= 65536)
    {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0)         return std::wstring();
        if (n < buf.size()) return std::wstring(buf.data(), n);
        buf.resize(buf.size() * 2);
    }
    return std::wstring();
}

std::wstring StartupCommand()
{
    const std::wstring path = ExePath();
    return path.empty() ? std::wstring() : L"\"" + path + L"\"";
}

static bool ReadString(const wchar_t* key, const wchar_t* value, std::wstring& out)
{
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, key, value, RRF_RT_REG_SZ, nullptr, nullptr, &bytes)
        != ERROR_SUCCESS)
        return false;

    std::vector<wchar_t> buf(bytes / sizeof(wchar_t) + 1);
    bytes = static_cast<DWORD>(buf.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_CURRENT_USER, key, value, RRF_RT_REG_SZ, nullptr, buf.data(), &bytes)
        != ERROR_SUCCESS)
        return false;

    out.assign(buf.data());
    return true;
}

bool IsStartupEnabled(const StartupKeys& keys)
{
    std::wstring registered;
    if (!ReadString(keys.run, keys.value, registered)) return false;

    // Registered for a different copy - an older download, say - means this one is
    // not the one that will start. Paths compare without regard to case.
    const std::wstring mine = StartupCommand();
    if (mine.empty() ||
        CompareStringOrdinal(registered.c_str(), -1, mine.c_str(), -1, TRUE) != CSTR_EQUAL)
        return false;

    // Task Manager's Startup apps toggle leaves the Run value alone and records its
    // choice here instead, as a binary value whose first byte is 2 when enabled and
    // 3 when disabled. Testing the low bit treats any odd value as disabled.
    BYTE  flags[64] = {};
    DWORD size      = sizeof(flags);
    if (RegGetValueW(HKEY_CURRENT_USER, keys.approved, keys.value, RRF_RT_REG_BINARY,
                     nullptr, flags, &size) == ERROR_SUCCESS &&
        size >= 1 && (flags[0] & 1) != 0)
        return false;

    return true;
}

bool SetStartupEnabled(const StartupKeys& keys, bool enable)
{
    if (!enable)
    {
        const LSTATUS rc = RegDeleteKeyValueW(HKEY_CURRENT_USER, keys.run, keys.value);
        if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND)
        {
            SetLastError(static_cast<DWORD>(rc));
            return false;
        }
        RegDeleteKeyValueW(HKEY_CURRENT_USER, keys.approved, keys.value);
        return true;
    }

    const std::wstring cmd = StartupCommand();
    if (cmd.empty()) return false;   // GetModuleFileNameW has set the last error

    HKEY    key = nullptr;
    LSTATUS rc  = RegCreateKeyExW(HKEY_CURRENT_USER, keys.run, 0, nullptr, 0, KEY_SET_VALUE,
                                  nullptr, &key, nullptr);
    if (rc == ERROR_SUCCESS)
    {
        rc = RegSetValueExW(key, keys.value, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(cmd.c_str()),
                            static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
    if (rc != ERROR_SUCCESS)
    {
        SetLastError(static_cast<DWORD>(rc));
        return false;
    }

    // Clear a "disabled" that Task Manager may have recorded, or turning the option
    // on here would appear to do nothing. With no value there, Windows treats the
    // entry as enabled.
    RegDeleteKeyValueW(HKEY_CURRENT_USER, keys.approved, keys.value);
    return true;
}
