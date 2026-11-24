// Guest-only Winsock boundary. No socket is created and no name is resolved here. The loader's registry and
// library_loader pseudo-handles bind manually mapped images; the host runtime keeps its normal Windows APIs.
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "shims.h"

namespace rt::winsock {
namespace {

int disconnected() {
    WSASetLastError(WSAENETDOWN);
    return SOCKET_ERROR;
}

SOCKET WSAAPI socket_offline(int, int, int) {
    disconnected();
    return INVALID_SOCKET;
}
SOCKET WSAAPI socket_a(int, int, int, LPWSAPROTOCOL_INFOA, GROUP, DWORD) {
    disconnected();
    return INVALID_SOCKET;
}
SOCKET WSAAPI socket_w(int, int, int, LPWSAPROTOCOL_INFOW, GROUP, DWORD) {
    disconnected();
    return INVALID_SOCKET;
}
SOCKET WSAAPI accept_offline(SOCKET, sockaddr*, int*) {
    disconnected();
    return INVALID_SOCKET;
}
int WSAAPI bind_offline(SOCKET, const sockaddr*, int) { return disconnected(); }
int WSAAPI close_offline(SOCKET) { return disconnected(); }
int WSAAPI connect_offline(SOCKET, const sockaddr*, int) { return disconnected(); }
int WSAAPI wsa_connect(SOCKET, const sockaddr*, int, LPWSABUF, LPWSABUF, LPQOS, LPQOS) {
    return disconnected();
}
int WSAAPI name_offline(SOCKET, sockaddr*, int*) { return disconnected(); }
int WSAAPI get_option(SOCKET, int, int, char*, int*) { return disconnected(); }
int WSAAPI ioctl_offline(SOCKET, long, u_long*) { return disconnected(); }
int WSAAPI listen_offline(SOCKET, int) { return disconnected(); }
int WSAAPI recv_offline(SOCKET, char*, int, int) { return disconnected(); }
int WSAAPI recv_from(SOCKET, char*, int, int, sockaddr*, int*) { return disconnected(); }
int WSAAPI select_offline(int, fd_set*, fd_set*, fd_set*, const timeval*) { return disconnected(); }
int WSAAPI send_offline(SOCKET, const char*, int, int) { return disconnected(); }
int WSAAPI send_to(SOCKET, const char*, int, int, const sockaddr*, int) { return disconnected(); }
int WSAAPI wsa_send(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, LPWSAOVERLAPPED,
                   LPWSAOVERLAPPED_COMPLETION_ROUTINE) {
    // This is a synchronous failure: no overlapped work or completion callback is queued, and no output is written.
    return disconnected();
}
int WSAAPI wsa_send_to(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, const sockaddr*, int, LPWSAOVERLAPPED,
                      LPWSAOVERLAPPED_COMPLETION_ROUTINE) {
    return disconnected();
}
int WSAAPI set_option(SOCKET, int, int, const char*, int) { return disconnected(); }
int WSAAPI shutdown_offline(SOCKET, int) { return disconnected(); }

hostent* WSAAPI host_by_name(const char*) {
    disconnected();
    return nullptr;
}
int WSAAPI host_name(char*, int) { return disconnected(); }

int WSAAPI address_info(PCSTR, PCSTR, const ADDRINFOA*, PADDRINFOA* result) {
    // getaddrinfo returns its error directly, unlike send/connect. Do not inspect names/hints or consult DNS,
    // even for numeric names. Publish no result; an inaccessible output pointer has the normal WSAEFAULT error.
    if (!result) {
        WSASetLastError(WSAEFAULT);
        return WSAEFAULT;
    }
    __try {
        *result = nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        WSASetLastError(WSAEFAULT);
        return WSAEFAULT;
    }
    WSASetLastError(WSATRY_AGAIN);
    return WSATRY_AGAIN;
}

// The original Winsock ordinals used by this image were checked against the host DLL's export table. Cover both
// static ordinal imports and named GetProcAddress. Unknown dynamic exports on the owned pseudo-module fail closed.
#define WS_EXPORT(dll, name, ordinal, function) \
    {dll, name, reinterpret_cast<void*>(function)}, \
    {dll, "#" #ordinal, reinterpret_cast<void*>(function)}
#define WS(name, ordinal, function) \
    WS_EXPORT("ws2_32.dll", name, ordinal, function), \
    WS_EXPORT("wsock32.dll", name, ordinal, function)

}  // namespace
}  // namespace rt::winsock

SHIM_OWNED_TABLE(winsock_offline,
    WS("accept", 1, &rt::winsock::accept_offline),
    WS("bind", 2, &rt::winsock::bind_offline),
    WS("closesocket", 3, &rt::winsock::close_offline),
    WS("connect", 4, &rt::winsock::connect_offline),
    WS("getsockname", 6, &rt::winsock::name_offline),
    WS("getsockopt", 7, &rt::winsock::get_option),
    WS("htons", 9, &::htons),
    WS_EXPORT("ws2_32.dll", "ioctlsocket", 10, &rt::winsock::ioctl_offline),
    WS_EXPORT("ws2_32.dll", "inet_addr", 11, &::inet_addr),
    WS_EXPORT("wsock32.dll", "ioctlsocket", 12, &rt::winsock::ioctl_offline),
    WS_EXPORT("wsock32.dll", "inet_addr", 10, &::inet_addr),
    WS_EXPORT("wsock32.dll", "inet_ntoa", 11, &::inet_ntoa),
    WS("listen", 13, &rt::winsock::listen_offline),
    WS("recv", 16, &rt::winsock::recv_offline),
    WS("recvfrom", 17, &rt::winsock::recv_from),
    WS("select", 18, &rt::winsock::select_offline),
    WS("send", 19, &rt::winsock::send_offline),
    WS("sendto", 20, &rt::winsock::send_to),
    WS("setsockopt", 21, &rt::winsock::set_option),
    WS("shutdown", 22, &rt::winsock::shutdown_offline),
    WS("socket", 23, &rt::winsock::socket_offline),
    {"ws2_32.dll", "WSAConnect", reinterpret_cast<void*>(&rt::winsock::wsa_connect)},
    WS("gethostbyname", 52, &rt::winsock::host_by_name),
    WS("gethostname", 57, &rt::winsock::host_name),
    {"ws2_32.dll", "WSASend", reinterpret_cast<void*>(&rt::winsock::wsa_send)},
    {"ws2_32.dll", "WSASendTo", reinterpret_cast<void*>(&rt::winsock::wsa_send_to)},
    WS("WSAGetLastError", 111, &::WSAGetLastError),
    WS("WSASetLastError", 112, &::WSASetLastError),
    WS("WSAStartup", 115, &::WSAStartup),
    WS("WSACleanup", 116, &::WSACleanup),
    {"ws2_32.dll", "WSASocketA", reinterpret_cast<void*>(&rt::winsock::socket_a)},
    {"ws2_32.dll", "WSASocketW", reinterpret_cast<void*>(&rt::winsock::socket_w)},
    WS("__WSAFDIsSet", 151, &::__WSAFDIsSet),
    {"ws2_32.dll", "freeaddrinfo", reinterpret_cast<void*>(&::freeaddrinfo)},
    {"ws2_32.dll", "getaddrinfo", reinterpret_cast<void*>(&rt::winsock::address_info)},
);

#undef WS
#undef WS_EXPORT
