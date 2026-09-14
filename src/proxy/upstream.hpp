#pragma once

#include "../common/conn.hpp"
#include "../common/tls.hpp"
#include "config.hpp"

#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace proxy {

std::string peer_name(const Peer& peer);

std::unique_ptr<Conn> dial(const Peer& peer, TlsContext* tls, int connect_timeout_ms, std::string& err);

class UpstreamGroup {
public:
    explicit UpstreamGroup(const Upstream& cfg);

    const Upstream& config() const { return cfg_; }

    int pick(const std::vector<bool>& tried);

    void report(int peer, bool ok);

    std::unique_ptr<Conn> take_idle(int peer);
    void park(int peer, std::unique_ptr<Conn> conn);

private:
    using Clock = std::chrono::steady_clock;

    struct Idle {
        std::unique_ptr<Conn> conn;
        Clock::time_point since;
    };

    struct State {
        int current_weight = 0;
        int fails = 0;
        Clock::time_point window_start{};
        Clock::time_point down_until{};
        std::deque<Idle> idle;
    };

    const Upstream& cfg_;
    std::mutex mu_;
    std::vector<State> peers_;
};

} // namespace proxy
