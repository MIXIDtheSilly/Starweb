#pragma once

#include "../common/net.hpp"
#include "../common/tls.hpp"
#include "config.hpp"
#include "log.hpp"
#include "upstream.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace proxy {

struct Runtime {
    Config cfg;
    uint64_t generation = 0;
    Log access;
    Log error;
    std::vector<std::unique_ptr<TlsContext>> server_tls;
    std::map<uint16_t, std::unique_ptr<TlsContext>> port_tls;
    std::unique_ptr<TlsContext> client_tls;
    std::vector<std::unique_ptr<UpstreamGroup>> groups;
};

std::shared_ptr<Runtime> build_runtime(Config cfg, std::string& err);

void log_error(Runtime& rt, const std::string& message);

void serve_connection(std::shared_ptr<Runtime> rt, net::socket_t fd, std::string client_ip,
                      uint16_t port, bool star);

void log_notice(Runtime& rt, const std::string& message);

void activate(Runtime& rt);

bool shutting_down();
void begin_shutdown();

void close_idle_connections();
int active_connections();

} // namespace proxy
