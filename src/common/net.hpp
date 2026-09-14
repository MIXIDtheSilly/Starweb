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
    #include <fcntl.h>
    #include <poll.h>
    #include <cerrno>
#endif

#include <cstdint>
#include <cstring>
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

inline void set_recv_timeout_ms(socket_t s, int ms) {
#if defined(_WIN32)
    DWORD v = (DWORD)ms;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&v, sizeof(v));
#else
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#endif
}

inline void set_send_timeout_ms(socket_t s, int ms) {
#if defined(_WIN32)
    DWORD v = (DWORD)ms;
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&v, sizeof(v));
#else
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
#endif
}

inline bool last_error_was_timeout() {
#if defined(_WIN32)
    int e = WSAGetLastError();
    return e == WSAETIMEDOUT || e == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

inline void set_nonblocking(socket_t s, bool on) {
#if defined(_WIN32)
    u_long mode = on ? 1 : 0;
    ::ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = ::fcntl(s, F_GETFL, 0);
    ::fcntl(s, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
#endif
}

// 1 readable, 0 timed out, -1 error.
inline int wait_readable(socket_t s, int timeout_ms) {
#if defined(_WIN32)
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(s, &rfds);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    int n = ::select(0, &rfds, nullptr, nullptr, &tv);
    return n < 0 ? -1 : (n > 0 ? 1 : 0);
#else
    pollfd p{s, POLLIN, 0};
    int n;
    do { n = ::poll(&p, 1, timeout_ms); } while (n < 0 && errno == EINTR);
    return n < 0 ? -1 : (n > 0 ? 1 : 0);
#endif
}

inline bool connect_timeout(socket_t s, const sockaddr* addr, socklen_t len, int timeout_ms) {
    set_nonblocking(s, true);
    if (::connect(s, addr, len) != 0) {
#if defined(_WIN32)
        if (WSAGetLastError() != WSAEWOULDBLOCK) return false;
        fd_set wfds, efds;
        FD_ZERO(&wfds);
        FD_ZERO(&efds);
        FD_SET(s, &wfds);
        FD_SET(s, &efds);
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        int n = ::select(0, nullptr, &wfds, &efds, &tv);
        if (n <= 0) { WSASetLastError(WSAETIMEDOUT); return false; }
#else
        if (errno != EINPROGRESS) return false;
        pollfd p{s, POLLOUT, 0};
        int n;
        do { n = ::poll(&p, 1, timeout_ms); } while (n < 0 && errno == EINTR);
        if (n == 0) { errno = ETIMEDOUT; return false; }
        if (n < 0) return false;
#endif
        int soerr = 0;
        socklen_t sl = sizeof(soerr);
        ::getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&soerr, &sl);
        if (soerr != 0) {
#if defined(_WIN32)
            WSASetLastError(soerr);
#else
            errno = soerr;
#endif
            return false;
        }
    }
    set_nonblocking(s, false);
    return true;
}

inline void shutdown_write(socket_t s) {
#if defined(_WIN32)
    ::shutdown(s, SD_SEND);
#else
    ::shutdown(s, SHUT_WR);
#endif
}

inline void shutdown_both(socket_t s) {
#if defined(_WIN32)
    ::shutdown(s, SD_BOTH);
#else
    ::shutdown(s, SHUT_RDWR);
#endif
}

inline std::string ip_string(const sockaddr_storage& ss) {
    char buf[INET6_ADDRSTRLEN] = "";
    if (ss.ss_family == AF_INET) {
        auto* in = reinterpret_cast<const sockaddr_in*>(&ss);
        ::inet_ntop(AF_INET, (void*)&in->sin_addr, buf, sizeof(buf));
    } else if (ss.ss_family == AF_INET6) {
        auto* in6 = reinterpret_cast<const sockaddr_in6*>(&ss);
        ::inet_ntop(AF_INET6, (void*)&in6->sin6_addr, buf, sizeof(buf));
    }
    std::string ip = buf;
    if (ip.rfind("::ffff:", 0) == 0 && ip.find('.') != std::string::npos) ip.erase(0, 7);
    return ip;
}

// An IPv6 socket with V6ONLY off also serves IPv4 peers as v4-mapped addresses.
// Falls back to IPv4 where that is refused or IPv6 is unavailable.
inline socket_t listen_tcp(int port, int backlog = 64) {
    socket_t fd = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (is_valid(fd)) {
        enable_reuseaddr(fd);
        int v6only = 0;
        ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&v6only, sizeof(v6only));

        sockaddr_in6 address6{};
        address6.sin6_family = AF_INET6;
        address6.sin6_addr = in6addr_any;
        address6.sin6_port = htons((uint16_t)port);

        if (::bind(fd, (sockaddr*)&address6, sizeof(address6)) == 0 &&
            ::listen(fd, backlog) == 0) {
            return fd;
        }
        close(fd);
    }

    fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (!is_valid(fd)) return kInvalidSocket;
    enable_reuseaddr(fd);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons((uint16_t)port);

    if (::bind(fd, (sockaddr*)&address, sizeof(address)) != 0 || ::listen(fd, backlog) != 0) {
        close(fd);
        return kInvalidSocket;
    }
    return fd;
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
