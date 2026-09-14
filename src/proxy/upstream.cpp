#include "upstream.hpp"

#include "../common/resolver.hpp"

#include <cstring>

namespace proxy {

std::string peer_name(const Peer& peer) {
    std::string host = peer.host.find(':') != std::string::npos ? "[" + peer.host + "]" : peer.host;
    return host + ":" + std::to_string(peer.port);
}

std::unique_ptr<Conn> dial(const Peer& peer, TlsContext* tls, int connect_timeout_ms, std::string& err) {
    std::string resolve_err;
    auto endpoints = stardns::resolve(peer.host, peer.port, resolve_err);
    if (endpoints.empty()) {
        err = "cannot resolve " + peer.host + (resolve_err.empty() ? "" : ": " + resolve_err);
        return nullptr;
    }

    for (const auto& ep : endpoints) {
        net::socket_t fd = ::socket(ep.family, SOCK_STREAM, 0);
        if (!net::is_valid(fd)) continue;
        if (net::connect_timeout(fd, (const sockaddr*)&ep.addr, ep.len, connect_timeout_ms)) {
            net::set_nodelay(fd);
            if (!tls) return std::make_unique<PlainConn>(fd);

            net::set_recv_timeout_ms(fd, connect_timeout_ms);
            net::set_send_timeout_ms(fd, connect_timeout_ms);
            auto conn = TlsConn::connect(*tls, fd, peer.host, peer_name(peer), err);
            if (conn) return conn;
            net::close(fd);
            continue;
        }
        err = std::string("connect failed: ") + std::strerror(errno);
        net::close(fd);
    }
    if (err.empty()) err = "connect failed";
    return nullptr;
}

} // namespace proxy

namespace proxy {

namespace {
// Under stwp_server's 20 s, so the proxy lets an idle connection go first.
constexpr auto kIdleLifetime = std::chrono::seconds(10);
}

UpstreamGroup::UpstreamGroup(const Upstream& cfg) : cfg_(cfg), peers_(cfg.peers.size()) {}

int UpstreamGroup::pick(const std::vector<bool>& tried) {
    std::lock_guard<std::mutex> lock(mu_);
    auto now = Clock::now();
    for (bool allow_down : {false, true}) {
        int best = -1;
        int total = 0;
        for (size_t i = 0; i < peers_.size(); ++i) {
            if (tried[i] || (!allow_down && peers_[i].down_until > now)) continue;
            peers_[i].current_weight += cfg_.peers[i].weight;
            total += cfg_.peers[i].weight;
            if (best < 0 || peers_[i].current_weight > peers_[(size_t)best].current_weight) best = (int)i;
        }
        if (best >= 0) {
            peers_[(size_t)best].current_weight -= total;
            return best;
        }
    }
    return -1;
}

void UpstreamGroup::report(int peer, bool ok) {
    std::lock_guard<std::mutex> lock(mu_);
    State& s = peers_[(size_t)peer];
    const Peer& p = cfg_.peers[(size_t)peer];
    if (ok) {
        s.fails = 0;
        return;
    }
    if (p.max_fails == 0) return;
    auto now = Clock::now();
    auto window = std::chrono::milliseconds(p.fail_timeout_ms);
    if (s.fails == 0 || now - s.window_start > window) {
        s.window_start = now;
        s.fails = 0;
    }
    if (++s.fails >= p.max_fails) {
        s.down_until = now + window;
        s.fails = 0;
    }
}

std::unique_ptr<Conn> UpstreamGroup::take_idle(int peer) {
    std::vector<std::unique_ptr<Conn>> expired;
    std::unique_ptr<Conn> found;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto& idle = peers_[(size_t)peer].idle;
        auto now = Clock::now();
        while (!idle.empty() && !found) {
            Idle item = std::move(idle.back());
            idle.pop_back();
            if (now - item.since < kIdleLifetime && net::wait_readable(item.conn->fd(), 0) == 0) {
                found = std::move(item.conn);
            } else {
                expired.push_back(std::move(item.conn));
            }
        }
    }
    return found;
}

void UpstreamGroup::park(int peer, std::unique_ptr<Conn> conn) {
    if (cfg_.keepalive <= 0) return;
    std::unique_ptr<Conn> evicted;
    std::lock_guard<std::mutex> lock(mu_);
    auto& idle = peers_[(size_t)peer].idle;
    idle.push_back({std::move(conn), Clock::now()});
    if ((int)idle.size() > cfg_.keepalive) {
        evicted = std::move(idle.front().conn);
        idle.pop_front();
    }
}

} // namespace proxy
