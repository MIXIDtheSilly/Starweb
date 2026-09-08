#pragma once
// BSD sockets on POSIX, Winsock2 on Windows. Use socket_t / kInvalidSocket
// rather than int / -1: on Windows an invalid socket is not negative.

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #if defined(_MSC_VER)
        #pragma comment(lib, "Ws2_32.lib")  // MinGW links -lws2_32 instead
    #endif
#else
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netdb.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <cerrno>
#endif

#include <string>

namespace net {

#if defined(_WIN32)
    using socket_t = SOCKET;      // UINT_PTR, not int
    using ssize_t_ = int;         // recv()/send() return int on Winsock
    static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
    using socket_t = int;
    using ssize_t_ = ssize_t;
    static constexpr socket_t kInvalidSocket = -1;
#endif

inline bool is_valid(socket_t s) { return s != kInvalidSocket; }

inline int close(socket_t s) {
    if (s == kInvalidSocket) return 0;
#if defined(_WIN32)
    return ::closesocket(s);
#else
    return ::close(s);
#endif
}

// Winsock wants a DWORD of milliseconds; POSIX wants a struct timeval.
inline void set_recv_timeout(socket_t s, int seconds) {
#if defined(_WIN32)
    DWORD ms = (DWORD)seconds * 1000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof(ms));
#else
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#endif
}

inline void set_send_timeout(socket_t s, int seconds) {
#if defined(_WIN32)
    DWORD ms = (DWORD)seconds * 1000;
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&ms, sizeof(ms));
#else
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
#endif
}

// Nagle plus delayed ACK stalls the header write; the writes are batched by hand.
inline int set_nodelay(socket_t s) {
    int opt = 1;
    return ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&opt, sizeof(opt));
}

inline int enable_reuseaddr(socket_t s) {
    int opt = 1;
    return ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
}

// Declare one instance in main(): WSAStartup/WSACleanup on Windows, no-op elsewhere.
struct Startup {
    Startup() {
#if defined(_WIN32)
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
#endif
    }
    ~Startup() {
#if defined(_WIN32)
        WSACleanup();
#endif
    }
    Startup(const Startup&) = delete;
    Startup& operator=(const Startup&) = delete;
};

} // namespace net
