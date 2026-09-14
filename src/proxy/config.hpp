#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace proxy {

struct Peer {
    std::string host;
    uint16_t port = 0;
    int weight = 1;
    int max_fails = 1;
    int fail_timeout_ms = 10000;
};

struct Upstream {
    std::string name;
    std::vector<Peer> peers;
    int keepalive = 0;
    bool implicit = false;
};

struct Listen {
    uint16_t port = 0;
    bool star = false;
    bool default_server = false;
};

struct HeaderRule {
    std::string name;
    std::string value;
};

struct ProxyPass {
    bool star = false;
    int upstream = -1;
    std::string proxy_host;
    bool has_uri = false;
    std::string uri;
};

enum class Match { Exact, Prefix };

struct Location {
    Match match = Match::Prefix;
    std::string path;
    std::string file;
    int line = 0;

    bool proxy = false;
    ProxyPass pass;
    std::vector<HeaderRule> set_headers;
    int connect_timeout_ms = 5000;
    int read_timeout_ms = 60000;

    std::string root;
    std::string index = "index.html";

    int return_code = 0;
    std::string return_body;
};

struct Server {
    std::vector<Listen> listens;
    std::vector<std::string> names;
    std::string cert;
    std::string key;
    std::vector<Location> locations;
    std::string file;
    int line = 0;
};

struct Config {
    std::string path;
    std::string error_log;
    std::string access_log;
    std::string trusted_ca;
    int keepalive_timeout_ms = 20000;
    uint64_t client_max_body_size = 1u << 20;
    std::vector<Upstream> upstreams;
    std::vector<Server> servers;
};

bool load_config(const std::string& path, Config& out, std::string& err);

std::string dump_config(const Config& cfg);

} // namespace proxy
