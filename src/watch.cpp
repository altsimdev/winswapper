#include "watch.h"

#include <cstddef>
#include <cwchar>

namespace
{

// What saving a file can do to it: create, delete or rename it, or write to it.
const DWORD kChanges = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE;

ULONGLONG Join(DWORD high, DWORD low)
{
    return (static_cast<ULONGLONG>(high) << 32) | low;
}

}   // namespace

bool StampOf(const std::wstring& path, FileStamp& out)
{
    out = FileStamp();
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
    {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) return true;
        SetLastError(err);
        return false;
    }
    out.exists    = true;
    out.lastWrite = Join(data.ftLastWriteTime.dwHighDateTime, data.ftLastWriteTime.dwLowDateTime);
    out.size      = Join(data.nFileSizeHigh, data.nFileSizeLow);
    return true;
}

FileCheck CheckFile(const std::wstring& path, const FileStamp& was, FileStamp& now)
{
    const FileStamp before = was;   // in case they are one and the same
    if (!StampOf(path, now)) return FileCheck::Busy;   // half way through being replaced, say
    if (now == before) return FileCheck::Same;
    if (!now.exists) return FileCheck::Changed;

    // Opened as it would be to read it: whatever saved it may be holding it shut still.
    const HANDLE f = CreateFileW(path.c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE)
    {
        CloseHandle(f);
        return FileCheck::Changed;
    }
    const DWORD err = GetLastError();
    return err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION ? FileCheck::Busy : FileCheck::Changed;
}

bool FileWatch::Start(const std::wstring& path, HWND notify, UINT msg, DWORD& error)
{
    Stop();
    error = 0;

    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos || slash == 0 || slash + 1 == path.size())
    {
        error = ERROR_INVALID_NAME;
        return false;
    }
    name_   = path.substr(slash + 1);
    notify_ = notify;
    msg_    = msg;

    // Shared every way, so that watching keeps nothing in the folder - nor the folder
    // itself - from being changed.
    dir_ = CreateFileW(path.substr(0, slash).c_str(), FILE_LIST_DIRECTORY,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                       FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (dir_ == INVALID_HANDLE_VALUE)
    {
        error = GetLastError();
        return false;
    }

    stop_  = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stop_ && ready_) thread_ = CreateThread(nullptr, 0, Thread, this, 0, nullptr);
    if (!thread_)
    {
        error = GetLastError();
        Stop();
        return false;
    }

    // Not before the first read is waiting: a change made between the two would be
    // missed, as the folder records changes only from then on.
    const HANDLE both[2] = { ready_, thread_ };
    if (WaitForMultipleObjects(2, both, FALSE, INFINITE) != WAIT_OBJECT_0)
    {
        error = startError_ ? startError_ : ERROR_GEN_FAILURE;
        Stop();
        return false;
    }
    return true;
}

void FileWatch::Stop()
{
    if (thread_)
    {
        SetEvent(stop_);
        // Quick: all the thread has to do is call off its read.
        WaitForSingleObject(thread_, INFINITE);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    if (dir_ != INVALID_HANDLE_VALUE) CloseHandle(dir_);
    if (stop_) CloseHandle(stop_);
    if (ready_) CloseHandle(ready_);
    dir_        = INVALID_HANDLE_VALUE;
    stop_       = nullptr;
    ready_      = nullptr;
    startError_ = 0;
}

bool FileWatch::Watching() const
{
    return thread_ && WaitForSingleObject(thread_, 0) == WAIT_TIMEOUT;
}

DWORD WINAPI FileWatch::Thread(void* param)
{
    static_cast<FileWatch*>(param)->Run();
    return 0;
}

void FileWatch::Run()
{
    // DWORD-aligned, as ReadDirectoryChangesW requires, with room for hundreds of
    // changes - and should more come between two reads, it says so rather than losing
    // them unsaid.
    DWORD      changes[4096];
    OVERLAPPED ov = {};
    ov.hEvent     = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent)
    {
        startError_ = GetLastError();
        return;
    }

    bool  first = true;
    DWORD error = 0;
    for (;;)
    {
        ResetEvent(ov.hEvent);
        if (!ReadDirectoryChangesW(dir_, changes, sizeof(changes), FALSE, kChanges, nullptr, &ov, nullptr))
        {
            error = GetLastError();
            break;
        }
        if (first)
        {
            first = false;
            SetEvent(ready_);
        }

        const HANDLE both[2] = { stop_, ov.hEvent };
        const DWORD  which   = WaitForMultipleObjects(2, both, FALSE, INFINITE);
        DWORD        bytes   = 0;
        if (which != WAIT_OBJECT_0 + 1)
        {
            if (which == WAIT_FAILED) error = GetLastError();
            // The read has to be over before the buffer it would fill goes away.
            CancelIoEx(dir_, &ov);
            GetOverlappedResult(dir_, &ov, &bytes, TRUE);
            break;
        }
        if (!GetOverlappedResult(dir_, &ov, &bytes, FALSE))
        {
            error = GetLastError();
            if (error != ERROR_NOTIFY_ENUM_DIR) break;
            error = 0;
            bytes = 0;
        }
        // Nothing told: more changed than there was room to tell, which may include it.
        if (bytes == 0 || Concerns(changes, bytes)) PostMessageW(notify_, msg_, 0, 0);
    }

    CloseHandle(ov.hEvent);
    if (first)
        startError_ = error ? error : ERROR_GEN_FAILURE;
    else if (error)
        PostMessageW(notify_, msg_, error, 0);
}

bool FileWatch::Concerns(const void* changes, DWORD bytes) const
{
    const auto*  base   = static_cast<const unsigned char*>(changes);
    const size_t header = offsetof(FILE_NOTIFY_INFORMATION, FileName);
    for (size_t at = 0; at + header <= bytes;)
    {
        const auto*  change = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(base + at);
        const size_t room   = bytes - at - header;
        const size_t len    = (change->FileNameLength < room ? change->FileNameLength : room) / sizeof(wchar_t);
        if (CompareStringOrdinal(change->FileName, static_cast<int>(len), name_.c_str(),
                                 static_cast<int>(name_.size()), TRUE) == CSTR_EQUAL)
            return true;
        // A change made through a short name, such as SETTIN~1.INI, is told by that name.
        if (std::wmemchr(change->FileName, L'~', len)) return true;
        if (change->NextEntryOffset == 0) break;
        at += change->NextEntryOffset;
    }
    return false;
}
