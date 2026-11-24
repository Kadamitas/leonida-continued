// Synthetic registry and dynamic-lookup checks. No host socket, connection or name lookup is ever requested.
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <thread>

#include "shims.h"

namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) ++failures;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
}
template <typename Fn> Fn resolve(const char* name) {
    auto fn = reinterpret_cast<Fn>(rt::shims::find("ws2_32.dll", name));
    check(fn != nullptr, name);
    return fn;
}
void failed(int value, const char* what) {
    check(value == SOCKET_ERROR && WSAGetLastError() == WSAENETDOWN, what);
}
int completions = 0;
void CALLBACK completed(DWORD, DWORD, LPWSAOVERLAPPED, DWORD) { ++completions; }
}  // namespace

int main() {
    check(rt::shims::owns_dll("ws2_32.dll"), "guest Winsock is an owned module");
    const struct { const char* name; const char* ordinal; } imported[] = {
        {"accept", "#1"}, {"bind", "#2"}, {"closesocket", "#3"}, {"connect", "#4"},
        {"getsockname", "#6"}, {"getsockopt", "#7"}, {"htons", "#9"}, {"ioctlsocket", "#10"},
        {"inet_addr", "#11"}, {"listen", "#13"}, {"recv", "#16"}, {"recvfrom", "#17"},
        {"select", "#18"}, {"send", "#19"}, {"sendto", "#20"}, {"setsockopt", "#21"},
        {"shutdown", "#22"}, {"socket", "#23"}, {"gethostbyname", "#52"}, {"gethostname", "#57"},
        {"WSAGetLastError", "#111"}, {"WSAStartup", "#115"}, {"__WSAFDIsSet", "#151"},
    };
    for (const auto& entry : imported) {
        const auto named = rt::shims::find("ws2_32.dll", entry.name);
        check(named && named == rt::shims::find("ws2_32.dll", entry.ordinal), entry.name);
    }
    auto startup = resolve<decltype(&::WSAStartup)>("WSAStartup");
    auto cleanup = resolve<decltype(&::WSACleanup)>("WSACleanup");
    check(startup == &::WSAStartup && cleanup == &::WSACleanup, "only startup/cleanup remain host-backed");
    WSADATA data{};
    check(startup(MAKEWORD(2, 2), &data) == 0 && data.wVersion == MAKEWORD(2, 2), "host Winsock initialization");

    void* inaccessible = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    check(inaccessible != nullptr, "inaccessible synthetic input/output page allocated");
    if (!inaccessible) return 1;
    const SOCKET invalid = INVALID_SOCKET;
    auto sock = resolve<decltype(&::socket)>("socket");
    check(sock(AF_INET, SOCK_STREAM, IPPROTO_TCP) == INVALID_SOCKET && WSAGetLastError() == WSAENETDOWN,
          "socket returns full-width INVALID_SOCKET");
    auto sock_a = resolve<decltype(&::WSASocketA)>("WSASocketA");
    auto sock_w = resolve<decltype(&::WSASocketW)>("WSASocketW");
    check(sock_a(AF_INET, SOCK_DGRAM, IPPROTO_UDP, static_cast<LPWSAPROTOCOL_INFOA>(inaccessible), 0, 0) ==
              INVALID_SOCKET && WSAGetLastError() == WSAENETDOWN, "WSASocketA refuses without reading protocol info");
    check(sock_w(AF_INET6, SOCK_STREAM, IPPROTO_TCP, static_cast<LPWSAPROTOCOL_INFOW>(inaccessible), 0, 0) ==
              INVALID_SOCKET && WSAGetLastError() == WSAENETDOWN, "WSASocketW refuses without reading protocol info");
    auto addr = static_cast<sockaddr*>(inaccessible);
    auto bytes = static_cast<char*>(inaccessible);
    auto size = static_cast<int*>(inaccessible);
    auto connect_fn = resolve<decltype(&::connect)>("connect");
    failed(connect_fn(invalid, addr, sizeof(sockaddr)), "connect refuses without reading address");
    auto wsa_connect = resolve<decltype(&::WSAConnect)>("WSAConnect");
    failed(wsa_connect(invalid, addr, sizeof(sockaddr), nullptr, nullptr, nullptr, nullptr), "WSAConnect refuses");
    auto send_fn = resolve<decltype(&::send)>("send");
    auto send_to = resolve<decltype(&::sendto)>("sendto");
    failed(send_fn(invalid, bytes, 4096, 0), "send refuses without reading data");
    failed(send_to(invalid, bytes, 4096, 0, addr, sizeof(sockaddr)), "sendto refuses without reading data/address");

    auto wsa_send = resolve<decltype(&::WSASend)>("WSASend");
    auto wsa_send_to = resolve<decltype(&::WSASendTo)>("WSASendTo");
    DWORD sent = 0x12345678;
    WSAOVERLAPPED overlapped{};
    overlapped.Internal = 0xabc;
    const WSAOVERLAPPED before = overlapped;
    failed(wsa_send(invalid, static_cast<LPWSABUF>(inaccessible), MAXDWORD, &sent, 0, &overlapped, &completed),
           "WSASend is an immediate non-pending failure");
    failed(wsa_send_to(invalid, static_cast<LPWSABUF>(inaccessible), MAXDWORD, &sent, 0, addr, sizeof(sockaddr),
                       &overlapped, &completed), "WSASendTo is an immediate non-pending failure");
    check(sent == 0x12345678 && std::memcmp(&overlapped, &before, sizeof before) == 0 && completions == 0,
          "send failure leaves counts/overlapped untouched and invokes no completion");
    failed(wsa_send(invalid, nullptr, 0, static_cast<LPDWORD>(inaccessible), 0,
                    static_cast<LPWSAOVERLAPPED>(inaccessible), nullptr), "send failure never writes invalid outputs");

    auto accept_fn = resolve<decltype(&::accept)>("accept");
    check(accept_fn(invalid, addr, size) == INVALID_SOCKET && WSAGetLastError() == WSAENETDOWN,
          "accept leaves output address untouched");
    failed(resolve<decltype(&::bind)>("bind")(invalid, addr, 16), "bind refuses");
    failed(resolve<decltype(&::closesocket)>("closesocket")(invalid), "closesocket refuses foreign handles");
    failed(resolve<decltype(&::listen)>("listen")(invalid, 1), "listen refuses");
    failed(resolve<decltype(&::getsockname)>("getsockname")(invalid, addr, size), "getsockname protects outputs");
    failed(resolve<decltype(&::getsockopt)>("getsockopt")(invalid, 0, 0, bytes, size), "getsockopt protects outputs");
    failed(resolve<decltype(&::ioctlsocket)>("ioctlsocket")(invalid, FIONBIO, static_cast<u_long*>(inaccessible)),
           "ioctlsocket protects output");
    failed(resolve<decltype(&::recv)>("recv")(invalid, bytes, 4096, 0), "recv protects output");
    failed(resolve<decltype(&::recvfrom)>("recvfrom")(invalid, bytes, 4096, 0, addr, size), "recvfrom protects outputs");
    failed(resolve<decltype(&::select)>("select")(0, static_cast<fd_set*>(inaccessible), nullptr, nullptr,
                                                static_cast<const timeval*>(inaccessible)), "select protects sets");
    failed(resolve<decltype(&::setsockopt)>("setsockopt")(invalid, 0, 0, bytes, 4096), "setsockopt refuses");
    failed(resolve<decltype(&::shutdown)>("shutdown")(invalid, SD_BOTH), "shutdown refuses");

    auto get_addr = resolve<decltype(&::getaddrinfo)>("getaddrinfo");
    PADDRINFOA result = reinterpret_cast<PADDRINFOA>(1);
    check(get_addr(bytes, bytes, static_cast<const ADDRINFOA*>(inaccessible), &result) == WSATRY_AGAIN &&
              !result && WSAGetLastError() == WSATRY_AGAIN, "getaddrinfo directly returns DNS failure and nulls output");
    check(get_addr("127.0.0.1", "80", nullptr, &result) == WSATRY_AGAIN && !result,
          "numeric addresses do not bypass offline lookup");
    check(get_addr(nullptr, nullptr, nullptr, nullptr) == WSAEFAULT && WSAGetLastError() == WSAEFAULT,
          "null result pointer returns WSAEFAULT");
    check(get_addr(nullptr, nullptr, nullptr, static_cast<PADDRINFOA*>(inaccessible)) == WSAEFAULT &&
              WSAGetLastError() == WSAEFAULT, "inaccessible DNS result pointer fails without an exception");
    check(!resolve<decltype(&::gethostbyname)>("gethostbyname")(bytes) && WSAGetLastError() == WSAENETDOWN,
          "gethostbyname returns null without reading the name");
    failed(resolve<decltype(&::gethostname)>("gethostname")(bytes, 4096), "gethostname protects output");
    check(resolve<decltype(&::freeaddrinfo)>("freeaddrinfo") == &::freeaddrinfo, "result-freeing helper stays local");

    auto load = reinterpret_cast<decltype(&::LoadLibraryW)>(rt::shims::find("kernel32.dll", "LoadLibraryW"));
    auto get_proc = reinterpret_cast<decltype(&::GetProcAddress)>(rt::shims::find("kernel32.dll", "GetProcAddress"));
    const HMODULE host = GetModuleHandleW(L"ws2_32.dll");
    const HMODULE guest = load(L"ws2_32.dll");
    check(guest && guest != host, "guest LoadLibrary yields a pseudo-handle");
    check(get_proc(guest, "socket") == reinterpret_cast<FARPROC>(sock) &&
              get_proc(guest, MAKEINTRESOURCEA(23)) == reinterpret_cast<FARPROC>(sock),
          "guest named and ordinal dynamic lookups return offline socket");
    check(!get_proc(guest, "WSAIoctl") && GetLastError() == ERROR_PROC_NOT_FOUND,
          "unknown guest dynamic export fails closed");
    check(get_proc(host, "socket") == reinterpret_cast<FARPROC>(sock) &&
              get_proc(host, MAKEINTRESOURCEA(23)) == reinterpret_cast<FARPROC>(sock),
          "guest lookup through a real module handle still gets offline socket");
    check(GetProcAddress(host, "socket") == reinterpret_cast<FARPROC>(&::socket) && sock != &::socket,
          "host Windows export remains unchanged");
    check(!get_proc(host, "WSAIoctl") && GetLastError() == ERROR_PROC_NOT_FOUND,
          "unknown guest export also fails through a host Winsock handle");
    const HMODULE legacy = load(L"wsock32.dll");
    check(legacy && rt::shims::owns_dll("wsock32.dll") &&
              get_proc(legacy, MAKEINTRESOURCEA(23)) == reinterpret_cast<FARPROC>(sock),
          "legacy Winsock has the same guest socket implementation");
    check(get_proc(legacy, MAKEINTRESOURCEA(10)) == reinterpret_cast<FARPROC>(&::inet_addr) &&
              get_proc(legacy, MAKEINTRESOURCEA(11)) == reinterpret_cast<FARPROC>(&::inet_ntoa) &&
              get_proc(legacy, MAKEINTRESOURCEA(12)) == get_proc(guest, "ioctlsocket"),
          "legacy Winsock preserves its own address and ioctl ordinals");
    WSASetLastError(1234);
    bool thread_ok = false;
    std::thread thread([&] {
        thread_ok = sock(AF_INET, SOCK_STREAM, 0) == INVALID_SOCKET && WSAGetLastError() == WSAENETDOWN;
    });
    thread.join();
    check(thread_ok && WSAGetLastError() == 1234, "Winsock errors remain thread-local");
    VirtualFree(inaccessible, 0, MEM_RELEASE);
    check(cleanup() == 0, "host startup reference released");
    std::printf("winsock_offline_test: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
