#include <windows.h>
#include <winternl.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

typedef LONG (NTAPI *QueryFn)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
static QueryFn query;
static bool all;
struct AllInfo { FILE_BASIC_INFO basic; FILE_STANDARD_INFO standard; char rest[1024]; };

static bool Take(HANDLE h, long long& time, long long& size) {
    if (all) {
        AllInfo info{};
        IO_STATUS_BLOCK io{};
        const LONG status = query(h, &io, &info, sizeof(info), 18);
        if (status < 0 && status != (LONG)0x80000005) return false;
        time = info.basic.LastWriteTime.QuadPart;
        size = info.standard.EndOfFile.QuadPart;
        return true;
    }
    for (;;) {
        FILE_BASIC_INFO b{}, b2{};
        FILE_STANDARD_INFO st{};
        if (!GetFileInformationByHandleEx(h, FileBasicInfo, &b, sizeof(b))) return false;
        if (!GetFileInformationByHandleEx(h, FileStandardInfo, &st, sizeof(st))) return false;
        if (!GetFileInformationByHandleEx(h, FileBasicInfo, &b2, sizeof(b2))) return false;
        if (b2.LastWriteTime.QuadPart == b.LastWriteTime.QuadPart) {
            time = b.LastWriteTime.QuadPart;
            size = st.EndOfFile.QuadPart;
            return true;
        }
    }
}

int main(int argc, char** argv) {
    const int ops = argc > 1 ? atoi(argv[1]) : 2000;
    const bool extend = argc > 2 && atoi(argv[2]) != 0;
    const int pause = argc > 3 ? atoi(argv[3]) : 1;
    all = argc > 4 && atoi(argv[4]) != 0;
    query = (QueryFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationFile");
    wchar_t path[MAX_PATH], name[MAX_PATH];
    GetTempPathW(MAX_PATH, path);
    swprintf(name, MAX_PATH, L"%lsorderprobe-%lu.bin", path, GetCurrentProcessId());
    HANDLE w = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (w == INVALID_HANDLE_VALUE) return 2;
    long long size = extend ? 0 : 1 << 22;
    FILE_END_OF_FILE_INFO end{};
    end.EndOfFile.QuadPart = size;
    SetFileInformationByHandle(w, FileEndOfFileInfo, &end, sizeof(end));
    HANDLE r = ReOpenFile(w, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
    std::atomic<int> done{0};
    std::thread changer([&] {
        for (int i = 0; i < ops; ++i) {
            end.EndOfFile.QuadPart = extend ? ++size : --size;
            SetFileInformationByHandle(w, FileEndOfFileInfo, &end, sizeof(end));
            if (pause >= 0) Sleep(pause);
        }
        done = 1;
    });
    long long lastTime = 0, lastSize = 0;
    Take(r, lastTime, lastSize);
    long long timeFirst = 0, sizeFirst = 0, both = 0;
    while (!done) {
        long long t = 0, s = 0;
        if (!Take(r, t, s)) return 3;
        if (t == lastTime && s == lastSize) continue;
        if (t != lastTime && s == lastSize) ++timeFirst;
        else if (t == lastTime && s != lastSize) ++sizeFirst;
        else ++both;
        lastTime = t;
        lastSize = s;
    }
    changer.join();
    printf("%s %s ops %d: time changed alone %lld, size changed alone %lld, both at once %lld\n", all ? "all-info" : "basic-standard-basic", extend ? "extend" : "shrink", ops, timeFirst, sizeFirst, both);
    CloseHandle(r);
    CloseHandle(w);
    return 0;
}
