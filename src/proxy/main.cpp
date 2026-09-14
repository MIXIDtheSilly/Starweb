#include "../common/net.hpp"
#include "config.hpp"
#include "proxy.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <pthread.h>
#endif

namespace {

constexpr auto kDrainLimit = std::chrono::seconds(10);

std::mutex g_runtime_mutex;
std::shared_ptr<proxy::Runtime> g_runtime;

std::shared_ptr<proxy::Runtime> current_runtime() {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    return g_runtime;
}

void set_runtime(std::shared_ptr<proxy::Runtime> rt) {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    g_runtime = std::move(rt);
}

void usage() {
    std::cerr << "usage: stwp_proxy [-c file] [-t | -T]\n"
                 "  -c file  configuration file (default conf/stwp_proxy.conf)\n"
                 "  -t       check the configuration and exit\n"
                 "  -T       check the configuration, print it resolved, and exit\n";
}

std::map<uint16_t, bool> listen_ports(const proxy::Config& cfg) {
    std::map<uint16_t, bool> ports;
    for (const auto& srv : cfg.servers) {
        for (const auto& l : srv.listens) ports[l.port] = l.star;
    }
    return ports;
}

bool load(const std::string& path, std::shared_ptr<proxy::Runtime>& rt, std::string& err) {
    proxy::Config cfg;
    if (!proxy::load_config(path, cfg, err)) return false;
    rt = proxy::build_runtime(std::move(cfg), err);
    return rt != nullptr;
}

void accept_loop(net::socket_t listener, uint16_t port, bool star) {
    while (!proxy::shutting_down()) {
        if (net::wait_readable(listener, 250) <= 0) continue;
        sockaddr_storage addr{};
        socklen_t len = sizeof(addr);
        net::socket_t fd = ::accept(listener, (sockaddr*)&addr, &len);
        if (!net::is_valid(fd)) continue;
        std::thread(proxy::serve_connection, current_runtime(), fd, net::ip_string(addr), port, star).detach();
    }
    net::close(listener);
}

#ifndef _WIN32
void reload(const std::string& path, const std::map<uint16_t, bool>& ports) {
    auto old = current_runtime();
    std::shared_ptr<proxy::Runtime> rt;
    std::string err;
    if (!load(path, rt, err)) {
        proxy::log_error(*old, "reload failed, keeping the running configuration: " + err);
        return;
    }
    if (listen_ports(rt->cfg) != ports) {
        proxy::log_error(*old, "reload failed, keeping the running configuration: "
                               "listen ports changed, which needs a restart");
        return;
    }
    proxy::activate(*rt);
    set_runtime(rt);
    proxy::close_idle_connections();
    proxy::log_notice(*rt, "configuration reloaded");
}
#endif

} // namespace

int main(int argc, char* argv[]) {
    net::Startup net_startup;
    std::string conf_path = "conf/stwp_proxy.conf";
    bool test = false, dump = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-c" && i + 1 < argc) {
            conf_path = argv[++i];
        } else if (a == "-t") {
            test = true;
        } else if (a == "-T") {
            test = dump = true;
        } else {
            usage();
            return a == "-h" || a == "--help" ? 0 : 2;
        }
    }

    std::shared_ptr<proxy::Runtime> rt;
    std::string err;
    if (!load(conf_path, rt, err)) {
        std::cerr << "stwp_proxy: " << err << "\n";
        std::cerr << "stwp_proxy: configuration " << conf_path << " test failed\n";
        return 1;
    }

    if (test) {
        if (dump) std::cout << proxy::dump_config(rt->cfg);
        std::cerr << "stwp_proxy: configuration " << conf_path << " test is successful\n";
        return 0;
    }

#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
    // Blocked before any thread starts, so only sigwait below ever sees them.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGHUP);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
#endif

    proxy::activate(*rt);
    set_runtime(rt);
    const auto ports = listen_ports(rt->cfg);

    std::vector<std::thread> threads;
    for (const auto& [port, star] : ports) {
        net::socket_t fd = net::listen_tcp(port);
        if (!net::is_valid(fd)) {
            std::cerr << "stwp_proxy: cannot listen on port " << port << ": " << std::strerror(errno) << "\n";
            return 1;
        }
        std::cerr << "stwp_proxy: listening on " << (star ? "star" : "moon") << " port " << port << "\n";
        threads.emplace_back(accept_loop, fd, port, star);
    }
    rt.reset();

#ifndef _WIN32
    for (;;) {
        int sig = 0;
        sigwait(&signals, &sig);
        if (sig != SIGHUP) break;
        reload(conf_path, ports);
    }

    proxy::log_notice(*current_runtime(), "shutting down");
    proxy::begin_shutdown();
    for (auto& t : threads) t.join();

    auto deadline = std::chrono::steady_clock::now() + kDrainLimit;
    while (proxy::active_connections() > 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (int left = proxy::active_connections(); left > 0) {
        proxy::log_notice(*current_runtime(), "exiting with " + std::to_string(left) + " connections still open");
        std::_Exit(0);
    }
#else
    for (auto& t : threads) t.join();
#endif
    return 0;
}
