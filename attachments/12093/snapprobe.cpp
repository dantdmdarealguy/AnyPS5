#include <windows.h>
#include <winternl.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

struct Snap { long long size; long long lastWrite; long long change; long long start = 0; long long done = 0; };
static long long Now() { FILETIME t; GetSystemTimePreciseAsFileTime(&t); return ((long long)t.dwHighDateTime << 32) | t.dwLowDateTime; }

typedef LONG (NTAPI *QueryFn)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);

struct AllInfo {
    FILE_BASIC_INFO basic;
    FILE_STANDARD_INFO standard;
    char rest[1024];
};

static bool Take(HANDLE h, int mode, Snap& s, QueryFn query) {
    if (mode == 2) {
        AllInfo all{};
        IO_STATUS_BLOCK io{};
        LONG status = query(h, &io, &all, sizeof(all), 18);
        if (status < 0 && status != (LONG)0x80000005) return false;
        s = {all.standard.EndOfFile.QuadPart, all.basic.LastWriteTime.QuadPart, all.basic.ChangeTime.QuadPart};
        return true;
    }
    for (;;) {
        FILE_BASIC_INFO b{}, b2{};
        FILE_STANDARD_INFO st{};
        if (!GetFileInformationByHandleEx(h, FileBasicInfo, &b, sizeof(b))) return false;
        if (!GetFileInformationByHandleEx(h, FileStandardInfo, &st, sizeof(st))) return false;
        s = {st.EndOfFile.QuadPart, b.LastWriteTime.QuadPart, b.ChangeTime.QuadPart};
        if (mode == 0) return true;
        if (!GetFileInformationByHandleEx(h, FileBasicInfo, &b2, sizeof(b2))) return false;
        if (b2.LastWriteTime.QuadPart == b.LastWriteTime.QuadPart && b2.ChangeTime.QuadPart == b.ChangeTime.QuadPart) return true;
    }
}

int main(int argc, char** argv) {
    const int mode = argc > 1 ? atoi(argv[1]) : 0;
    const int seconds = argc > 2 ? atoi(argv[2]) : 3;
    const int pollMs = argc > 3 ? atoi(argv[3]) : 0;
    const bool notify = argc > 4 && atoi(argv[4]) != 0;
    auto query = (QueryFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationFile");
    wchar_t path[MAX_PATH];
    GetTempPathW(MAX_PATH, path);
    wchar_t name[MAX_PATH];
    wchar_t dir[MAX_PATH];
    swprintf(dir, MAX_PATH, L"%lssnapprobe-%lu-%d", path, GetCurrentProcessId(), mode);
    CreateDirectoryW(dir, nullptr);
    swprintf(name, MAX_PATH, L"%ls\\file.bin", dir);
    HANDLE w = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (w == INVALID_HANDLE_VALUE) return 2;
    long long size = 4LL << 20;
    FILE_END_OF_FILE_INFO end{};
    end.EndOfFile.QuadPart = size;
    SetFileInformationByHandle(w, FileEndOfFileInfo, &end, sizeof(end));
    HANDLE r = ReOpenFile(w, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
    std::atomic<bool> stop{false};
    std::thread shrinker([&] {
        while (!stop && size > 1) {
            end.EndOfFile.QuadPart = --size;
            SetFileInformationByHandle(w, FileEndOfFileInfo, &end, sizeof(end));
        }
    });
    HANDLE change = FindFirstChangeNotificationW(dir, FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION | FILE_NOTIFY_CHANGE_SECURITY);
    Snap last{};
    Take(r, mode, last, query);
    long long polls = 0, torn = 0;
    const auto until = GetTickCount64() + seconds * 1000ULL;
    while (GetTickCount64() < until) {
        if (notify) {
            WaitForSingleObject(change, 50);
            FindNextChangeNotification(change);
        }
        Snap now{};
        const long long start = Now();
        if (!Take(r, mode, now, query)) return 3;
        now.start = start;
        now.done = Now();
        ++polls;
        if (now.size == last.size && now.lastWrite != last.lastWrite) {
            ++torn;
            if (torn <= 10) printf("torn: prev start %lld done %lld lastWrite %lld change %lld size %lld | now lastWrite %lld (vs prev start %+lld, prev done %+lld) change %lld\n", last.start, last.done, last.lastWrite, last.change, last.size, now.lastWrite, now.lastWrite - last.start, now.lastWrite - last.done, now.change);
        }
        last = now;
        if (pollMs) Sleep(pollMs);
    }
    stop = true;
    shrinker.join();
    printf("mode %d polls %lld torn %lld\n", mode, polls, torn);
    CloseHandle(r);
    CloseHandle(w);
    FindCloseChangeNotification(change);
    RemoveDirectoryW(dir);
    return torn != 0;
}
