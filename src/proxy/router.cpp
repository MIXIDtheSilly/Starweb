#include "router.hpp"

#include <algorithm>
#include <cctype>

namespace proxy {

std::string normalize_host(const std::string& host) {
    std::string h = host;
    if (!h.empty() && h[0] == '[') {
        auto close = h.find(']');
        h = close == std::string::npos ? "" : h.substr(1, close - 1);
    } else {
        auto colon = h.rfind(':');
        if (colon != std::string::npos && h.find(':') == colon) h.erase(colon);
    }
    std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return std::tolower(c); });
    if (!h.empty() && h.back() == '.') h.pop_back();
    return h;
}

const Server* pick_server(const Config& cfg, uint16_t port, const std::string& host) {
    const Server* first = nullptr;
    const Server* fallback = nullptr;
    const Server* wildcard = nullptr;
    size_t wildcard_len = 0;

    for (const Server& srv : cfg.servers) {
        auto l = std::find_if(srv.listens.begin(), srv.listens.end(),
                              [port](const Listen& x) { return x.port == port; });
        if (l == srv.listens.end()) continue;
        if (!first) first = &srv;
        if (l->default_server && !fallback) fallback = &srv;
        if (host.empty()) continue;

        for (const std::string& name : srv.names) {
            if (name == host) return &srv;
            if (name[0] != '*') continue;
            std::string_view suffix = std::string_view(name).substr(1);
            if (host.size() > suffix.size() && suffix.size() > wildcard_len &&
                host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0) {
                wildcard = &srv;
                wildcard_len = suffix.size();
            }
        }
    }
    if (wildcard) return wildcard;
    return fallback ? fallback : first;
}

const Location* pick_location(const Server& srv, const std::string& path) {
    const Location* best = nullptr;
    for (const Location& loc : srv.locations) {
        if (loc.match == Match::Exact) {
            if (loc.path == path) return &loc;
            continue;
        }
        if (path.compare(0, loc.path.size(), loc.path) == 0 &&
            (!best || loc.path.size() > best->path.size())) {
            best = &loc;
        }
    }
    return best;
}

} // namespace proxy
