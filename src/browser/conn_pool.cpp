#include "conn_pool.hpp"

#include <chrono>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace connpool {
namespace {

using Clock = std::chrono::steady_clock;

// Shorter than the server's idle timeout, so the client gives up first.
constexpr std::chrono::seconds kIdleTimeout{10};
constexpr std::size_t kMaxPerOrigin = 8;

struct Idle {
    std::unique_ptr<Conn> conn;
    Clock::time_point since;
};

std::mutex g_mux;
std::unordered_map<std::string, std::deque<Idle>> g_idle;

// Readable at once means a FIN or leftover bytes; either way it is no good.
bool still_usable(Conn& c) {
    net::socket_t fd = c.fd();
    if (!net::is_valid(fd)) return false;
#if !defined(_WIN32)
    if (fd >= FD_SETSIZE) return false;  // fd_set is a bitmap on POSIX
#endif
    fd_set r;
    FD_ZERO(&r);
    FD_SET(fd, &r);
    timeval tv{0, 0};
    int n = ::select((int)fd + 1, &r, nullptr, nullptr, &tv);
    return n == 0;
}

void prune(std::deque<Idle>& q, Clock::time_point now) {
    while (!q.empty() && now - q.front().since > kIdleTimeout) q.pop_front();
}

} // namespace

std::unique_ptr<Conn> acquire(const std::string& key) {
    std::lock_guard<std::mutex> lock(g_mux);
    auto it = g_idle.find(key);
    if (it == g_idle.end()) return nullptr;

    const auto now = Clock::now();
    prune(it->second, now);

    // Newest first: least likely to be dead.
    while (!it->second.empty()) {
        std::unique_ptr<Conn> c = std::move(it->second.back().conn);
        it->second.pop_back();
        if (c && still_usable(*c)) return c;
    }
    g_idle.erase(it);
    return nullptr;
}

void release(const std::string& key, std::unique_ptr<Conn> conn) {
    if (!conn || !net::is_valid(conn->fd())) return;
    std::lock_guard<std::mutex> lock(g_mux);
    auto& q = g_idle[key];
    prune(q, Clock::now());
    if (q.size() >= kMaxPerOrigin) q.pop_front();  // drop the stalest
    q.push_back(Idle{std::move(conn), Clock::now()});
}

void clear() {
    std::unordered_map<std::string, std::deque<Idle>> dead;
    {
        std::lock_guard<std::mutex> lock(g_mux);
        dead.swap(g_idle);
    }
    // Destructors run outside the lock: a TLS close can block.
}

} // namespace connpool
