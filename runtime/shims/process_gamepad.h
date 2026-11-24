#pragma once

#include <windows.h>

#include <cstdint>
#include <cstdio>

namespace rt::gameinput {

// The low word of a pulse holds either a GameInput button mask or a tagged analog axis.
// The other fields are the sender's GetTickCount() and a 16-bit duration in milliseconds.
// PID plus creation time distinguishes a new process when Windows recycles a PID.
struct ProcessPadState {
    std::uint32_t buttons = 0;
    std::int16_t axes[4]{};  // Left X, left Y, right X, right Y in SDL coordinates (+Y down).
};

class ProcessGamepad {
public:
    // Stick percentages use GameInput coordinates (+Y up). Zero means no request.
    static constexpr std::uint16_t stick_control(unsigned axis, int percent) {
        if (axis >= 4 || !percent || percent < -100 || percent > 100) return 0;
        return static_cast<std::uint16_t>(0x8000 | (axis << 8) | (percent + 100));
    }

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

    ProcessPadState sample() {
        if (!words_) return {};
        const LONG64 request = InterlockedCompareExchange64(words_, 0, 0);
        if (!request) { active_ = 0; return {}; }
        const auto bits = static_cast<std::uint64_t>(request);
        const DWORD duration = DWORD((bits >> 16) & 0xffff);
        const auto now = GetTickCount64();
        if (active_ != request) {
            ProcessPadState next;
            if (DWORD(now) - DWORD(bits >> 32) > 1500 || !duration || duration > 5000 ||
                !decode_control(static_cast<std::uint16_t>(bits), next)) {
                InterlockedCompareExchange64(words_, 0, request);
                return {};
            }
            active_ = request;
            state_ = next;
            release_at_ = now + duration;
            InterlockedExchange64(words_ + 1, request);
        }
        if (now < release_at_) return state_;
        InterlockedCompareExchange64(words_, 0, request);
        active_ = 0;
        return {};
    }

private:
    static bool decode_control(std::uint16_t control, ProcessPadState& state) {
        if (control && control <= 0x3fff) {
            state.buttons = control;
            return true;
        }
        const unsigned axis = (control >> 8) & 3;
        const int percent = (control & 0xff) - 100;
        if (!control || stick_control(axis, percent) != control) return false;
        const int sdl_percent = (axis & 1) ? -percent : percent;
        state.axes[axis] = static_cast<std::int16_t>(sdl_percent * (sdl_percent < 0 ? 32768 : 32767) / 100);
        return true;
    }

    HANDLE mapping_ = nullptr;
    volatile LONG64* words_ = nullptr;
    LONG64 active_ = 0;
    ULONGLONG release_at_ = 0;
    ProcessPadState state_;
};

} // namespace rt::gameinput
