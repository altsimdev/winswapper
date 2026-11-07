#pragma once

#include <windows.h>

#include <string>

// How a file looks from outside - whether it is there, when it was last written and
// how big it is - which is enough to tell that it has been saved or replaced.
struct FileStamp
{
    bool      exists    = false;
    ULONGLONG lastWrite = 0;   // a FILETIME, as one number
    ULONGLONG size      = 0;

    bool operator==(const FileStamp& o) const
    {
        return exists == o.exists && lastWrite == o.lastWrite && size == o.size;
    }
    bool operator!=(const FileStamp& o) const { return !(*this == o); }
};

// False, with the reason in GetLastError, when that cannot be told for now. A missing
// file can be: it does not exist.
bool StampOf(const std::wstring& path, FileStamp& out);

enum class FileCheck
{
    Same,      // as it was
    Changed,   // written, replaced or deleted since
    Busy,      // changed, but still open somewhere in a way that keeps it from being read
};

// Compares a file with how it was when last read, as the owner of a FileWatch does once
// the changes stop. `now` is how it is, to compare with next time.
FileCheck CheckFile(const std::wstring& path, const FileStamp& was, FileStamp& now);

// Watches one file by watching the folder it is in, so it is noticed being created,
// deleted or replaced by another file as well as written to. Each change to it is
// posted to `notify` as `msg`, with a wParam of 0. A save is often several changes in a
// row, so the owner should wait for them to stop before reading the file - and since
// the folder's other files can set it off too, should check the file has changed at
// all. If the watch fails later on, `msg` comes once more with the error as its wParam,
// and then no more.
class FileWatch
{
public:
    FileWatch() = default;
    ~FileWatch() { Stop(); }
    FileWatch(const FileWatch&)            = delete;
    FileWatch& operator=(const FileWatch&) = delete;

    // Once it returns true, every change from then on is reported. False, with `error`
    // set, if the folder cannot be watched: when it does not exist, say.
    bool Start(const std::wstring& path, HWND notify, UINT msg, DWORD& error);
    void Stop();
    bool Watching() const;

private:
    static DWORD WINAPI Thread(void* param);
    void Run();
    bool Concerns(const void* changes, DWORD bytes) const;

    std::wstring name_;                           // the file's name, without its folder
    HWND         notify_     = nullptr;
    UINT         msg_        = 0;
    HANDLE       dir_        = INVALID_HANDLE_VALUE;
    HANDLE       stop_       = nullptr;           // set to end the thread
    HANDLE       ready_      = nullptr;           // set once the first read is waiting
    DWORD        startError_ = 0;                 // why the first read could not be made
    HANDLE       thread_     = nullptr;
};
