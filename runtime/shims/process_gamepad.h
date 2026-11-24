#pragma once

#include <windows.h>

#include <cstdint>
#include <cstdio>

namespace rt::gameinput {

// A local client opens this process's mapping and atomically publishes one pulse:
// GetTickCount() in the high 32 bits, duration in ms in the next 16, GameInput buttons in the low 16.
// PID plus creation time distinguishes a new process when Windows recycles a PID.
class ProcessGamepad {
public:
    static bool channel_name(HANDLE process, wchar_t (&name)[96]) {
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return false;
        return swprintf_s(name, L"Local\\Leonida.Pad.%lu.%08lx%08lx", GetProcessId(process),
                          created.dwHighDateTime, created.dwLowDateTime) > 0;
    }
    bool open() {
        wchar_t name[96];
        if (!channel_name(GetCurrentProcess(), name)) return false;
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 16, name);
        if (!mapping_) return false;
        if (GetLastError() != ERROR_ALREADY_EXISTS)
            words_ = static_cast<volatile LONG64*>(MapViewOfFile(mapping_, FILE_MAP_WRITE, 0, 0, 16));
        if (!words_) { close(); return false; }
        return true;
    }

    void close() {
        if (words_) UnmapViewOfFile(const_cast<LONG64*>(words_));
        if (mapping_) CloseHandle(mapping_);
        words_ = nullptr;
        mapping_ = nullptr;
        active_ = 0;
    }

    std::uint32_t buttons() {
        if (!words_) return 0;
        const LONG64 request = InterlockedCompareExchange64(words_, 0, 0);
        if (!request) { active_ = 0; return 0; }
        const auto bits = static_cast<std::uint64_t>(request);
        const DWORD duration = DWORD((bits >> 16) & 0xffff);
        const UINT mask = UINT(bits & 0xffff);
        const auto now = GetTickCount64();
        if (active_ != request) {
            if (DWORD(now) - DWORD(bits >> 32) > 1500 || !mask || mask > 0x3fff || !duration || duration > 5000) {
                InterlockedCompareExchange64(words_, 0, request);
                return 0;
            }
            active_ = request;
            release_at_ = now + duration;
            InterlockedExchange64(words_ + 1, request);
        }
        if (now < release_at_) return mask;
        InterlockedCompareExchange64(words_, 0, request);
        active_ = 0;
        return 0;
    }

private:
    HANDLE mapping_ = nullptr;
    volatile LONG64* words_ = nullptr;
    LONG64 active_ = 0;
    ULONGLONG release_at_ = 0;
};

} // namespace rt::gameinput
