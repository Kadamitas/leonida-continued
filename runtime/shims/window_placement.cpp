// App-local placement for the title's top-level Americas window. Disabled without an exact monitor device.
// Only guest imports use these adapters; the runtime and other processes still call user32 directly.
#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <stdexcept>
#include <thread>

#include "shims.h"
#include "window_placement.h"

namespace {

constexpr wchar_t kTracked[] = L"Leonida.WindowPlacement.Americas.87f18722";
constexpr wchar_t kObserved[] = L"Leonida.WindowPlacement.NativeAmericas.87f18722";
constexpr UINT_PTR kSubclassId = 0x87f18722u;

struct Policy {
    wchar_t device[64]{};
    bool enabled = false;
    bool background = false;
    bool valid = false;
};

const Policy& policy() {
    const DWORD saved = GetLastError();
    static const Policy value = [] {
        Policy p;
        const DWORD count = GetEnvironmentVariableW(L"LEONIDA_WINDOW_MONITOR", p.device, 64);
        p.enabled = count != 0;
        p.valid = count != 0 && count < 64;
        wchar_t background[2]{};
        p.background = GetEnvironmentVariableW(L"LEONIDA_WINDOW_BACKGROUND", background, 2) == 1 &&
                       background[0] == L'1';
        return p;
    }();
    SetLastError(saved);
    return value;
}

struct MonitorSearch {
    MONITORINFOEXW info{};
    bool found = false;
};

BOOL CALLBACK find_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM context) {
    auto& search = *reinterpret_cast<MonitorSearch*>(context);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) && !_wcsicmp(info.szDevice, policy().device)) {
        search.info = info;
        search.found = true;
        return FALSE;
    }
    return TRUE;
}

bool selected_monitor(MONITORINFOEXW& info) {
    if (!policy().enabled || !policy().valid) return false;
    MonitorSearch search;
    EnumDisplayMonitors(nullptr, nullptr, find_monitor, reinterpret_cast<LPARAM>(&search));
    if (!search.found) return false;
    info = search.info;
    return true;
}

RECT bounds(const MONITORINFOEXW& monitor, DWORD style) {
    return style & (WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME) ? monitor.rcWork : monitor.rcMonitor;
}

bool main_window(HWND window) {
    if (!policy().enabled || !window) return false;
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process != GetCurrentProcessId() || (GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD) ||
        GetAncestor(window, GA_ROOT) != window)
        return false;
    if (GetPropW(window, kTracked) || GetPropW(window, kObserved)) return true;
    wchar_t name[16]{};
    return GetWindowTextW(window, name, 16) == 8 && !_wcsicmp(name, L"Americas");
}

bool fit(HWND window, DWORD style, int& x, int& y, int& width, int& height, UINT& flags,
         MONITORINFOEXW* result = nullptr) {
    MONITORINFOEXW monitor{};
    if (!selected_monitor(monitor)) {
        SetLastError(ERROR_INVALID_MONITOR_HANDLE);
        return false;  // never fall back to another display when the explicitly selected one is unavailable
    }
    const RECT area = bounds(monitor, style);
    const int maximum_width = area.right - area.left, maximum_height = area.bottom - area.top;
    if (maximum_width <= 0 || maximum_height <= 0) {
        SetLastError(ERROR_INVALID_MONITOR_HANDLE);
        return false;
    }
    RECT current{};
    const bool have_current = window && GetWindowRect(window, &current);
    if ((flags & SWP_NOMOVE) && have_current) { x = current.left; y = current.top; }
    if ((flags & SWP_NOSIZE) && have_current) {
        width = current.right - current.left;
        height = current.bottom - current.top;
    }
    if (x == CW_USEDEFAULT) x = area.left;
    if (y == CW_USEDEFAULT) y = area.top;
    if (width == CW_USEDEFAULT || width <= 0) width = maximum_width;
    if (height == CW_USEDEFAULT || height <= 0) height = maximum_height;
    width = (std::min)(width, maximum_width);
    height = (std::min)(height, maximum_height);
    x = std::clamp(x, static_cast<int>(area.left), static_cast<int>(area.right) - width);
    y = std::clamp(y, static_cast<int>(area.top), static_cast<int>(area.bottom) - height);
    if (have_current && (x != current.left || y != current.top)) flags &= ~SWP_NOMOVE;
    if (have_current && (width != current.right - current.left || height != current.bottom - current.top))
        flags &= ~SWP_NOSIZE;
    if (policy().background) flags |= SWP_NOACTIVATE;
    if (result) *result = monitor;
    return true;
}

bool track(HWND window);
void native_note(HWND window, const char* operation, DWORD event);
void place_on_owner_thread(HWND window, int event);

// Installed only on a discovered Americas window's own-process UI thread. This callback owns no Session/Loader
// pointer and reads only native window metadata, so an in-flight callback after unhook cannot access freed state.
LRESULT CALLBACK native_quiet_cbt(int code, WPARAM wparam, LPARAM lparam) {
    if (code != HCBT_ACTIVATE && code != HCBT_SETFOCUS && code != HCBT_MOVESIZE)
        return CallNextHookEx(nullptr, code, wparam, lparam);
    const HWND window = reinterpret_cast<HWND>(wparam);
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    const HWND root = GetAncestor(window, GA_ROOT);
    const bool target = process == GetCurrentProcessId() && root &&
                        (GetPropW(root, kObserved) || GetPropW(root, kTracked));
    if (target && (code == HCBT_ACTIVATE || code == HCBT_SETFOCUS)) place_on_owner_thread(root, code);
    if (target && policy().background && (code == HCBT_ACTIVATE || code == HCBT_SETFOCUS)) {
        static std::atomic<unsigned> vetoes{0};
        if (vetoes.fetch_add(1, std::memory_order_relaxed) < 8)
            std::fprintf(stderr, "[rt] window_placement: background native focus request vetoed (CBT %d)\n", code);
        return 1;
    }
    const LRESULT result = CallNextHookEx(nullptr, code, wparam, lparam);
    if (target && root == window && code == HCBT_MOVESIZE && lparam && result == 0) {
        auto& rectangle = *reinterpret_cast<RECT*>(lparam);
        int x = rectangle.left, y = rectangle.top;
        int width = rectangle.right - rectangle.left, height = rectangle.bottom - rectangle.top;
        UINT flags = 0;
        if (fit(nullptr, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)), x, y, width, height, flags))
            rectangle = {x, y, x + width, y + height};
    }
    return result;
}

struct SubclassApi {
    decltype(&SetWindowSubclass) set = nullptr;
    decltype(&RemoveWindowSubclass) remove = nullptr;
    decltype(&DefSubclassProc) next = nullptr;
};

const SubclassApi& subclass_api() {
    static const SubclassApi api = [] {
        SubclassApi result;
        // Keep the system module resident as long as any window can still call the installed procedure.
        const HMODULE module = LoadLibraryExW(L"comctl32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module) {
            result.set = reinterpret_cast<decltype(result.set)>(GetProcAddress(module, "SetWindowSubclass"));
            result.remove = reinterpret_cast<decltype(result.remove)>(GetProcAddress(module, "RemoveWindowSubclass"));
            result.next = reinterpret_cast<decltype(result.next)>(GetProcAddress(module, "DefSubclassProc"));
        }
        return result;
    }();
    return api;
}

LRESULT CALLBACK placement_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR) {
    const auto& api = subclass_api();
    if (message == WM_NCDESTROY) {
        RemovePropW(window, kTracked);
        RemovePropW(window, kObserved);
        api.remove(window, placement_proc, kSubclassId);
        return api.next(window, message, wparam, lparam);
    }
    const LRESULT result = api.next(window, message, wparam, lparam);
    if (message == WM_WINDOWPOSCHANGING && lparam) {
        auto& position = *reinterpret_cast<WINDOWPOS*>(lparam);
        if (!fit(window, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)), position.x, position.y,
                 position.cx, position.cy, position.flags))
            position.flags |= SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
        // WM_WINDOWPOSCHANGED/WM_SIZE still reach the original procedure with the final client size.
    } else if (message == WM_GETMINMAXINFO && lparam) {
        MONITORINFOEXW monitor{};
        if (selected_monitor(monitor)) {
            const RECT area = bounds(monitor, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)));
            auto& limits = *reinterpret_cast<MINMAXINFO*>(lparam);
            limits.ptMaxPosition = {area.left - monitor.rcMonitor.left, area.top - monitor.rcMonitor.top};
            limits.ptMaxSize = {area.right - area.left, area.bottom - area.top};
            limits.ptMaxTrackSize = limits.ptMaxSize;
            limits.ptMinTrackSize.x = (std::min)(limits.ptMinTrackSize.x, limits.ptMaxTrackSize.x);
            limits.ptMinTrackSize.y = (std::min)(limits.ptMinTrackSize.y, limits.ptMaxTrackSize.y);
        }
    }
    return result;
}

bool track(HWND window) {
    if (GetPropW(window, kTracked)) return true;
    const auto& api = subclass_api();
    if (!api.set || !api.remove || !api.next || !api.set(window, placement_proc, kSubclassId, 0)) return false;
    if (!SetPropW(window, kTracked, reinterpret_cast<HANDLE>(1))) {
        api.remove(window, placement_proc, kSubclassId);
        return false;
    }
    return true;
}

struct PhysicalCoordinates {
    DPI_AWARENESS_CONTEXT previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ~PhysicalCoordinates() { if (previous) SetThreadDpiAwarenessContext(previous); }
};

void native_note(HWND window, const char* operation, DWORD event) {
    const DWORD saved = GetLastError();
    static std::atomic<unsigned> messages{0};
    if (messages.fetch_add(1, std::memory_order_relaxed) < 32) {
        PhysicalCoordinates coordinates;
        MONITORINFOEXW monitor{};
        RECT outer{}, client{};
        const bool monitor_found = selected_monitor(monitor);
        const bool bounds_valid = GetWindowRect(window, &outer) != FALSE;
        GetClientRect(window, &client);
        std::fprintf(stderr, "[rt] window_placement: native %s event=0x%lx hwnd=%p monitor=%ls found=%u "
                             "physical=[%ld,%ld,%ld,%ld] actual=[%ld,%ld,%ld,%ld] bounds_valid=%u "
                             "client=%ldx%ld dpi=%u ex=0x%llx visible=%u iconic=%u foreground=%u\n",
                     operation, event, static_cast<void*>(window), policy().device, monitor_found ? 1u : 0u,
                     monitor.rcMonitor.left, monitor.rcMonitor.top, monitor.rcMonitor.right, monitor.rcMonitor.bottom,
                     outer.left, outer.top, outer.right, outer.bottom, bounds_valid ? 1u : 0u,
                     client.right - client.left, client.bottom - client.top, GetDpiForWindow(window),
                     static_cast<unsigned long long>(GetWindowLongPtrW(window, GWL_EXSTYLE)),
                     IsWindowVisible(window) ? 1u : 0u, IsIconic(window) ? 1u : 0u,
                     GetForegroundWindow() == window ? 1u : 0u);
    }
    SetLastError(saved);
}

void place_on_owner_thread(HWND window, int event) {
    if (GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId()) return;
    static thread_local bool placing = false;
    if (placing) return;
    struct Scope { bool& active; ~Scope() { active = false; } } scope{placing};
    placing = true;
    PhysicalCoordinates coordinates;
    // The subclass API was loaded before any guest images. Installation and SetWindowPos now run on the actual
    // UI thread, avoiding a cross-thread synchronous style/message wait during native SDK initialization.
    track(window);
    RECT before{};
    if (GetWindowRect(window, &before) && !IsIconic(window)) {
        int x = before.left, y = before.top, width = before.right - before.left, height = before.bottom - before.top;
        UINT flags = SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER;
        if (fit(window, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)), x, y, width, height, flags) &&
            (x != before.left || y != before.top || width != before.right - before.left ||
             height != before.bottom - before.top))
            SetWindowPos(window, nullptr, x, y, width, height, flags);
    }
    native_note(window, "owner-thread placement", static_cast<DWORD>(event));
}

void note(HWND window, const char* operation) {
    const DWORD saved = GetLastError();
    static std::atomic<unsigned> calls{0};
    if (calls.fetch_add(1, std::memory_order_relaxed) < 8) {
        MONITORINFOEXW monitor{};
        RECT outer{}, client{};
        if (selected_monitor(monitor) && GetWindowRect(window, &outer) && GetClientRect(window, &client)) {
            using DpiFn = UINT(WINAPI*)(HWND);
            const auto dpi = reinterpret_cast<DpiFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
            std::fprintf(stderr, "[rt] window_placement: %s hwnd=%p monitor=%ls physical=[%ld,%ld,%ld,%ld] "
                                 "outer=[%ld,%ld,%ld,%ld] client=%ldx%ld dpi=%u background=%u foreground=%u\n",
                         operation, window, monitor.szDevice, monitor.rcMonitor.left, monitor.rcMonitor.top,
                         monitor.rcMonitor.right, monitor.rcMonitor.bottom, outer.left, outer.top, outer.right,
                         outer.bottom, client.right - client.left, client.bottom - client.top,
                         dpi ? dpi(window) : 0, policy().background ? 1u : 0u, GetForegroundWindow() == window ? 1u : 0u);
        }
    }
    SetLastError(saved);
}

template<class Name> bool creation_target(DWORD style, Name name, HWND parent) {
    if (!policy().enabled || (style & WS_CHILD) || parent == HWND_MESSAGE || !name) return false;
    if constexpr (sizeof(*name) == sizeof(wchar_t)) return !_wcsicmp(name, L"Americas");
    else return !_stricmp(name, "Americas");
}

HWND finish_creation(HWND window, DWORD requested_ex_style, DWORD requested_style) {
    if (!window) return nullptr;
    if (!track(window)) {
        DestroyWindow(window);
        SetLastError(ERROR_NOT_SUPPORTED);
        return nullptr;
    }
    if (policy().background && !(requested_ex_style & WS_EX_NOACTIVATE)) {
        const LONG_PTR current = GetWindowLongPtrW(window, GWL_EXSTYLE);
        SetWindowLongPtrW(window, GWL_EXSTYLE, current & ~static_cast<LONG_PTR>(WS_EX_NOACTIVATE));
    }
    if (policy().background && (requested_style & WS_VISIBLE)) ShowWindow(window, SW_SHOWNOACTIVATE);
    note(window, "created");
    return window;
}

HWND WINAPI create_window_w(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD style, int x, int y, int width, int height,
                             HWND parent, HMENU menu, HINSTANCE instance, LPVOID parameter) {
    if (!creation_target(style, name, parent))
        return CreateWindowExW(ex, cls, name, style, x, y, width, height, parent, menu, instance, parameter);
    UINT flags = 0;
    if (!fit(nullptr, style, x, y, width, height, flags)) return nullptr;
    const DWORD effective_ex = policy().background ? ex | WS_EX_NOACTIVATE : ex;
    const DWORD effective_style = policy().background ? style & ~WS_VISIBLE : style;
    return finish_creation(CreateWindowExW(effective_ex, cls, name, effective_style, x, y, width, height,
                                          parent, menu, instance, parameter), ex, style);
}

HWND WINAPI create_window_a(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int width, int height,
                             HWND parent, HMENU menu, HINSTANCE instance, LPVOID parameter) {
    if (!creation_target(style, name, parent))
        return CreateWindowExA(ex, cls, name, style, x, y, width, height, parent, menu, instance, parameter);
    UINT flags = 0;
    if (!fit(nullptr, style, x, y, width, height, flags)) return nullptr;
    const DWORD effective_ex = policy().background ? ex | WS_EX_NOACTIVATE : ex;
    const DWORD effective_style = policy().background ? style & ~WS_VISIBLE : style;
    return finish_creation(CreateWindowExA(effective_ex, cls, name, effective_style, x, y, width, height,
                                          parent, menu, instance, parameter), ex, style);
}

BOOL WINAPI set_window_pos(HWND window, HWND after, int x, int y, int width, int height, UINT flags) {
    if (!main_window(window)) return SetWindowPos(window, after, x, y, width, height, flags);
    if (!fit(window, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)), x, y, width, height, flags)) return FALSE;
    const BOOL result = SetWindowPos(window, after, x, y, width, height, flags);
    note(window, "positioned");
    return result;
}

BOOL WINAPI move_window(HWND window, int x, int y, int width, int height, BOOL repaint) {
    if (!main_window(window)) return MoveWindow(window, x, y, width, height, repaint);
    return set_window_pos(window, nullptr, x, y, width, height, SWP_NOZORDER | (repaint ? 0 : SWP_NOREDRAW));
}

int background_show(int command) {
    if (!policy().background || command == SW_HIDE) return command;
    if (command == SW_SHOWMINIMIZED || command == SW_MINIMIZE || command == SW_SHOWMINNOACTIVE ||
        command == SW_FORCEMINIMIZE) return SW_SHOWMINNOACTIVE;
    return SW_SHOWNOACTIVATE;
}

BOOL WINAPI show_window(HWND window, int command) {
    return ShowWindow(window, main_window(window) ? background_show(command) : command);
}
BOOL WINAPI show_window_async(HWND window, int command) {
    return ShowWindowAsync(window, main_window(window) ? background_show(command) : command);
}
BOOL WINAPI foreground(HWND window) {
    if (policy().background && main_window(window)) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return SetForegroundWindow(window);
}
HWND WINAPI active(HWND window) {
    if (policy().background && main_window(window)) { SetLastError(ERROR_ACCESS_DENIED); return nullptr; }
    return SetActiveWindow(window);
}
BOOL WINAPI bring_to_top(HWND window) {
    if (policy().background && main_window(window)) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return BringWindowToTop(window);
}
HWND WINAPI capture(HWND window) {
    if (policy().background && main_window(window)) { SetLastError(ERROR_ACCESS_DENIED); return nullptr; }
    return SetCapture(window);
}
BOOL WINAPI clip_cursor(const RECT* rectangle) {
    if (policy().enabled && policy().background) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return ClipCursor(rectangle);
}
BOOL WINAPI cursor_position(int x, int y) {
    if (policy().enabled && policy().background) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return SetCursorPos(x, y);
}

}  // namespace

namespace rt::window_placement {

struct Session::State {
    using Caption = int(WINAPI*)(HWND, LPWSTR, int);
    HANDLE stop_event = nullptr;
    HANDLE ready_event = nullptr;
    std::thread thread;
    std::atomic<bool> stopping{false};
    std::atomic<bool> armed{false};
    std::atomic<DWORD> failure{0};
    Caption caption = nullptr;
    struct ThreadHook { DWORD thread = 0; HHOOK hook = nullptr; };
    std::array<ThreadHook, 8> thread_hooks{};
    static thread_local State* current;

    State() {
        stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ready_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        // Unlike same-process GetWindowText, this reads the native caption without sending WM_GETTEXT into a
        // guest procedure. The observer therefore introduces no late caption callbacks during Loader teardown.
        caption = reinterpret_cast<Caption>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "InternalGetWindowText"));
    }

    ~State() {
        stopping.store(true, std::memory_order_release);
        if (stop_event) SetEvent(stop_event);
        if (thread.joinable()) thread.join();
        if (ready_event) CloseHandle(ready_event);
        if (stop_event) CloseHandle(stop_event);
    }

    void ensure_thread_hook(DWORD window_thread) {
        if (!window_thread) return;  // thread0 would mean a system-wide hook, which is never used here
        for (const auto& hook : thread_hooks) if (hook.thread == window_thread) return;
        for (auto& slot : thread_hooks) {
            if (slot.thread) continue;
            const HHOOK hook = SetWindowsHookExW(WH_CBT, native_quiet_cbt, nullptr, window_thread);
            if (hook) {
                slot = {window_thread, hook};
                std::fprintf(stderr, "[rt] window_placement: own-process UI thread %lu CBT placement/focus guard armed\n",
                             window_thread);
            } else {
                std::fprintf(stderr, "[rt] window_placement: own-process UI thread %lu CBT guard unavailable (error %lu)\n",
                             window_thread, GetLastError());
            }
            return;
        }
    }

    void observe(HWND window, DWORD event) {
        if (stopping.load(std::memory_order_acquire) || !window) return;
        DWORD process = 0;
        const DWORD window_thread = GetWindowThreadProcessId(window, &process);
        if (process != GetCurrentProcessId() || (GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD) ||
            GetAncestor(window, GA_ROOT) != window)
            return;
        wchar_t name[16]{};
        if (!GetPropW(window, kObserved) && !GetPropW(window, kTracked) &&
            (caption(window, name, 16) != 8 || _wcsicmp(name, L"Americas")))
            return;
        native_note(window, "target-detected", event);
        SetPropW(window, kObserved, reinterpret_cast<HANDLE>(1));
        ensure_thread_hook(window_thread);

        // Never mutate styles synchronously from this observer thread: a native SDK UI thread can still be
        // initializing without pumping WM_STYLECHANGING. The own-thread CBT guard controls background focus.
        RECT before{};
        if (!GetWindowRect(window, &before) || IsIconic(window)) return;
        int x = before.left, y = before.top;
        int width = before.right - before.left, height = before.bottom - before.top;
        UINT flags = SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS;
        MONITORINFOEXW monitor{};
        if (!fit(window, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)), x, y, width, height, flags,
                 &monitor))
            return;
        const bool changed = x != before.left || y != before.top || width != before.right - before.left ||
                             height != before.bottom - before.top;
        if (changed) {
            static std::atomic<unsigned> requests{0};
            if (requests.fetch_add(1, std::memory_order_relaxed) < 16)
                std::fprintf(stderr, "[rt] window_placement: native async position request hwnd=%p requested=[%d,%d,%d,%d]\n",
                             static_cast<void*>(window), x, y, x + width, y + height);
            SetWindowPos(window, nullptr, x, y, width, height, flags);
        } else {
            native_note(window, "placement-observed", event);
        }
    }

    static void CALLBACK event(HWINEVENTHOOK, DWORD kind, HWND window, LONG object, LONG child, DWORD, DWORD) {
        if (!current || object != OBJID_WINDOW || child != CHILDID_SELF) return;
        if (kind == EVENT_OBJECT_CREATE || kind == EVENT_OBJECT_SHOW || kind == EVENT_OBJECT_NAMECHANGE ||
            kind == EVENT_OBJECT_LOCATIONCHANGE || kind == EVENT_OBJECT_STATECHANGE)
            current->observe(window, kind);
    }

    static BOOL CALLBACK initial_window(HWND window, LPARAM context) {
        auto* state = reinterpret_cast<State*>(context);
        if (state->stopping.load(std::memory_order_acquire)) return FALSE;
        state->observe(window, 0);
        return TRUE;
    }

    void run() {
        current = this;
        MSG message{};
        PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);  // establish the callback thread's message queue
        const HWINEVENTHOOK hook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_NAMECHANGE, nullptr, event,
                                                   GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
        if (!hook) {
            failure.store(GetLastError(), std::memory_order_release);
            std::fprintf(stderr, "[rt] window_placement: own-process event observer failed (error %lu)\n",
                         failure.load(std::memory_order_acquire));
            SetEvent(ready_event);
            current = nullptr;
            return;
        }
        armed.store(true, std::memory_order_release);
        std::fprintf(stderr, "[rt] window_placement: native observer armed for own pid=%lu monitor=%ls background=%u\n",
                     GetCurrentProcessId(), policy().device, policy().background ? 1u : 0u);
        SetEvent(ready_event);
        EnumWindows(initial_window, reinterpret_cast<LPARAM>(this));
        while (!stopping.load(std::memory_order_acquire)) {
            const DWORD wait = MsgWaitForMultipleObjects(1, &stop_event, FALSE, INFINITE, QS_ALLINPUT);
            if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) break;
            while (!stopping.load(std::memory_order_acquire) && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    stopping.store(true, std::memory_order_release);
                    break;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        UnhookWinEvent(hook);
        for (const auto& installed : thread_hooks) if (installed.hook) UnhookWindowsHookEx(installed.hook);
        current = nullptr;
    }
};

thread_local Session::State* Session::State::current = nullptr;

Session::Session() {
    if (!policy().enabled) return;
    MONITORINFOEXW monitor{};
    if (!selected_monitor(monitor)) throw std::runtime_error("Selected LEONIDA_WINDOW_MONITOR is unavailable");
    subclass_api();  // preload on this startup thread before any guest/native SDK window callback can need it
    state_ = std::make_unique<State>();
    if (!state_->stop_event || !state_->ready_event || !state_->caption)
        throw std::runtime_error("Cannot initialize the own-process window placement observer");
    state_->thread = std::thread([state = state_.get()] { state->run(); });
    if (WaitForSingleObject(state_->ready_event, 5000) != WAIT_OBJECT_0 ||
        !state_->armed.load(std::memory_order_acquire))
        throw std::runtime_error("Cannot arm the own-process window placement observer");
}

Session::~Session() = default;

}  // namespace rt::window_placement

SHIM_TABLE(window_placement,
    {"user32.dll", "CreateWindowExW", reinterpret_cast<void*>(&create_window_w)},
    {"user32.dll", "CreateWindowExA", reinterpret_cast<void*>(&create_window_a)},
    {"user32.dll", "SetWindowPos", reinterpret_cast<void*>(&set_window_pos)},
    {"user32.dll", "MoveWindow", reinterpret_cast<void*>(&move_window)},
    {"user32.dll", "ShowWindow", reinterpret_cast<void*>(&show_window)},
    {"user32.dll", "ShowWindowAsync", reinterpret_cast<void*>(&show_window_async)},
    {"user32.dll", "SetForegroundWindow", reinterpret_cast<void*>(&foreground)},
    {"user32.dll", "SetActiveWindow", reinterpret_cast<void*>(&active)},
    {"user32.dll", "BringWindowToTop", reinterpret_cast<void*>(&bring_to_top)},
    {"user32.dll", "SetCapture", reinterpret_cast<void*>(&capture)},
    {"user32.dll", "ClipCursor", reinterpret_cast<void*>(&clip_cursor)},
    {"user32.dll", "SetCursorPos", reinterpret_cast<void*>(&cursor_position)}
);
