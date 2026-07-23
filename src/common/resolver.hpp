#pragma once
// Name resolution for StarWeb.
//
// Names inside the StarWeb zone (.web) are resolved by asking StarDNS
// directly over UDP, not through getaddrinfo — the system resolver knows
// nothing about .web, and routing these names through it would put the
// namespace back in ICANN's hands. Everything else still goes to getaddrinfo,
// so localhost and IP literals behave exactly as before.
//
//   STARWEB_DNS       server to ask, "host[:port]"   (default 127.0.0.1:5354)
//                     set to "off" to disable and use the system resolver
//   STARWEB_DNS_ZONE  zone routed to it              (default web)

#include "net.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace stardns {

struct Endpoint {
    sockaddr_storage addr{};
    socklen_t len = 0;
    int family = 0;
};

namespace detail {

constexpr uint16_t kTypeA = 1, kTypeCNAME = 5, kTypeAAAA = 28, kTypeOPT = 41;
constexpr uint16_t kClassIN = 1;
constexpr int kTimeoutSecs = 2;
constexpr int kAttempts = 2;
constexpr int kMaxChase = 8;
constexpr uint32_t kMinTTL = 5, kMaxTTL = 3600;

inline std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

inline std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : fallback;
}

inline const std::string& zone() {
    static const std::string z = lower(env_or("STARWEB_DNS_ZONE", "web"));
    return z;
}

// "host[:port]" -> both halves. Empty host means resolution is disabled.
struct ServerAddr { std::string host; uint16_t port; };

inline const ServerAddr& server() {
    static const ServerAddr s = [] {
        std::string raw = env_or("STARWEB_DNS", "127.0.0.1:5354");
        if (lower(raw) == "off" || lower(raw) == "system") return ServerAddr{"", 0};
        uint16_t port = 53;
        size_t colon = raw.rfind(':');
        // Only split on a colon that isn't part of a bare IPv6 literal.
        if (colon != std::string::npos && raw.find(':') == colon) {
            port = (uint16_t)std::strtoul(raw.substr(colon + 1).c_str(), nullptr, 10);
            raw = raw.substr(0, colon);
        }
        if (port == 0) port = 53;
        return ServerAddr{raw, port};
    }();
    return s;
}

inline bool in_zone(const std::string& host) {
    const std::string& z = zone();
    if (z.empty() || host.size() < z.size()) return false;
    std::string h = lower(host);
    if (h == z) return true;
    return h.size() > z.size() + 1 &&
           h.compare(h.size() - z.size() - 1, z.size() + 1, "." + z) == 0;
}

// ---------------------------------------------------------------- wire

inline void put16(std::string& out, uint16_t v) {
    out += (char)(v >> 8); out += (char)(v & 0xFF);
}

inline bool encode_name(const std::string& name, std::string& out) {
    size_t start = 0;
    while (start < name.size()) {
        size_t dot = name.find('.', start);
        size_t len = (dot == std::string::npos ? name.size() : dot) - start;
        if (len == 0 || len > 63) return false;
        out += (char)len;
        out.append(name, start, len);
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    out += '\0';
    return true;
}

// Reads a possibly-compressed name. `pos` advances past the name as it appears
// here, which for a pointer is two bytes regardless of what it points at.
inline bool decode_name(const uint8_t* buf, size_t len, size_t& pos, std::string& out) {
    size_t here = pos;
    bool jumped = false;
    int hops = 0;
    out.clear();
    while (true) {
        if (here >= len) return false;
        uint8_t n = buf[here];
        if ((n & 0xC0) == 0xC0) {
            if (here + 1 >= len) return false;
            size_t target = (size_t)((n & 0x3F) << 8 | buf[here + 1]);
            if (!jumped) { pos = here + 2; jumped = true; }
            if (target >= here || ++hops > 16) return false;  // only backwards
            here = target;
            continue;
        }
        here++;
        if (n == 0) break;
        if (n > 63 || here + n > len) return false;
        if (!out.empty()) out += '.';
        out.append((const char*)buf + here, n);
        here += n;
    }
    if (!jumped) pos = here;
    out = lower(out);
    return true;
}

struct Record {
    std::string owner;
    uint16_t type = 0;
    uint32_t ttl = 0;
    std::string ip;      // A / AAAA, presentation form
    std::string target;  // CNAME
};

inline std::string ip_to_text(const uint8_t* p, size_t n) {
    char text[INET6_ADDRSTRLEN] = {};
    if (n == 4)  return inet_ntop(AF_INET, p, text, sizeof text) ? text : "";
    if (n == 16) return inet_ntop(AF_INET6, p, text, sizeof text) ? text : "";
    return "";
}

inline bool parse_answers(const uint8_t* buf, size_t len, uint16_t want_id,
                          bool& truncated, std::vector<Record>& out) {
    if (len < 12) return false;
    uint16_t id = (uint16_t)(buf[0] << 8 | buf[1]);
    uint16_t flags = (uint16_t)(buf[2] << 8 | buf[3]);
    if (id != want_id || !(flags & 0x8000)) return false;
    truncated = (flags & 0x0200) != 0;
    if ((flags & 0xF) != 0) return true;  // NXDOMAIN and friends: no records

    uint16_t qd = (uint16_t)(buf[4] << 8 | buf[5]);
    uint16_t an = (uint16_t)(buf[6] << 8 | buf[7]);

    size_t pos = 12;
    std::string scratch;
    for (uint16_t i = 0; i < qd; i++) {
        if (!decode_name(buf, len, pos, scratch)) return false;
        pos += 4;
        if (pos > len) return false;
    }
    for (uint16_t i = 0; i < an; i++) {
        Record rec;
        if (!decode_name(buf, len, pos, rec.owner)) return false;
        if (pos + 10 > len) return false;
        rec.type = (uint16_t)(buf[pos] << 8 | buf[pos + 1]);
        uint16_t cls = (uint16_t)(buf[pos + 2] << 8 | buf[pos + 3]);
        rec.ttl = (uint32_t)buf[pos + 4] << 24 | (uint32_t)buf[pos + 5] << 16 |
                  (uint32_t)buf[pos + 6] << 8 | (uint32_t)buf[pos + 7];
        uint16_t rdlen = (uint16_t)(buf[pos + 8] << 8 | buf[pos + 9]);
        pos += 10;
        if (pos + rdlen > len) return false;

        if (cls == kClassIN && rec.type == kTypeA && rdlen == 4) {
            rec.ip = ip_to_text(buf + pos, 4);
        } else if (cls == kClassIN && rec.type == kTypeAAAA && rdlen == 16) {
            rec.ip = ip_to_text(buf + pos, 16);
        } else if (cls == kClassIN && rec.type == kTypeCNAME) {
            size_t sub = pos;
            if (!decode_name(buf, len, sub, rec.target)) return false;
        }
        pos += rdlen;
        if (!rec.ip.empty() || !rec.target.empty()) out.push_back(std::move(rec));
    }
    return true;
}

inline std::string build_query(const std::string& name, uint16_t type, uint16_t id) {
    std::string q;
    put16(q, id);
    put16(q, 0x0100);  // RD, so this works against a recursor too
    put16(q, 1); put16(q, 0); put16(q, 0); put16(q, 1);
    if (!encode_name(name, q)) return "";
    put16(q, type);
    put16(q, kClassIN);
    q += '\0';                       // OPT root name
    put16(q, kTypeOPT);
    put16(q, 4096);                  // our UDP buffer, to avoid truncation
    put16(q, 0); put16(q, 0); put16(q, 0);
    return q;
}

// Fills `server_ep` with the configured DNS server's address.
inline bool server_endpoint(sockaddr_storage& ss, socklen_t& len, int& family) {
    const ServerAddr& s = server();
    sockaddr_in* v4 = (sockaddr_in*)&ss;
    if (inet_pton(AF_INET, s.host.c_str(), &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        v4->sin_port = htons(s.port);
        len = sizeof(sockaddr_in);
        family = AF_INET;
        return true;
    }
    sockaddr_in6* v6 = (sockaddr_in6*)&ss;
    std::memset(&ss, 0, sizeof ss);
    if (inet_pton(AF_INET6, s.host.c_str(), &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(s.port);
        len = sizeof(sockaddr_in6);
        family = AF_INET6;
        return true;
    }
    return false;  // the DNS server itself must be an address, not a name
}

inline bool exchange_udp(const std::string& query, std::vector<uint8_t>& reply) {
    sockaddr_storage ss{};
    socklen_t sslen = 0;
    int family = 0;
    if (!server_endpoint(ss, sslen, family)) return false;

    net::socket_t fd = socket(family, SOCK_DGRAM, 0);
    if (!net::is_valid(fd)) return false;
    net::set_recv_timeout(fd, kTimeoutSecs);

    bool ok = false;
    for (int attempt = 0; attempt < kAttempts && !ok; attempt++) {
        if (sendto(fd, query.data(), (int)query.size(), 0,
                   (sockaddr*)&ss, sslen) < 0) continue;
        reply.assign(4096, 0);
        net::ssize_t_ n = recvfrom(fd, (char*)reply.data(), (int)reply.size(), 0,
                                   nullptr, nullptr);
        if (n > 0) { reply.resize((size_t)n); ok = true; }
    }
    net::close(fd);
    return ok;
}

inline bool exchange_tcp(const std::string& query, std::vector<uint8_t>& reply) {
    sockaddr_storage ss{};
    socklen_t sslen = 0;
    int family = 0;
    if (!server_endpoint(ss, sslen, family)) return false;

    net::socket_t fd = socket(family, SOCK_STREAM, 0);
    if (!net::is_valid(fd)) return false;
    net::set_recv_timeout(fd, kTimeoutSecs);
    net::set_send_timeout(fd, kTimeoutSecs);

    bool ok = false;
    if (connect(fd, (sockaddr*)&ss, sslen) == 0) {
        std::string framed;
        put16(framed, (uint16_t)query.size());
        framed += query;
        if (send(fd, framed.data(), (int)framed.size(), 0) == (net::ssize_t_)framed.size()) {
            uint8_t head[2];
            if (recv(fd, (char*)head, 2, 0) == 2) {
                size_t want = (size_t)(head[0] << 8 | head[1]);
                reply.assign(want, 0);
                size_t got = 0;
                while (got < want) {
                    net::ssize_t_ n = recv(fd, (char*)reply.data() + got,
                                           (int)(want - got), 0);
                    if (n <= 0) break;
                    got += (size_t)n;
                }
                ok = (got == want);
            }
        }
    }
    net::close(fd);
    return ok;
}

inline bool ask(const std::string& name, uint16_t type, std::vector<Record>& out) {
    static uint16_t counter = 0;
    uint16_t id = (uint16_t)(++counter ^ (uint16_t)(std::chrono::steady_clock::now()
                      .time_since_epoch().count() & 0xFFFF));
    std::string query = build_query(name, type, id);
    if (query.empty()) return false;

    std::vector<uint8_t> reply;
    if (!exchange_udp(query, reply)) return false;

    bool truncated = false;
    if (!parse_answers(reply.data(), reply.size(), id, truncated, out)) return false;
    if (truncated) {
        out.clear();
        if (!exchange_tcp(query, reply)) return false;
        if (!parse_answers(reply.data(), reply.size(), id, truncated, out)) return false;
    }
    return true;
}

// ---------------------------------------------------------------- cache

struct Entry {
    std::vector<std::string> ips;
    std::chrono::steady_clock::time_point expires;
};

inline std::mutex& cache_mutex() { static std::mutex m; return m; }
inline std::unordered_map<std::string, Entry>& cache() {
    static std::unordered_map<std::string, Entry> c;
    return c;
}

// Every fetch opens its own connection, so a page with subresources would
// otherwise re-query for each one.
inline bool cache_get(const std::string& host, std::vector<std::string>& ips) {
    std::lock_guard<std::mutex> lock(cache_mutex());
    auto it = cache().find(host);
    if (it == cache().end()) return false;
    if (it->second.expires < std::chrono::steady_clock::now()) {
        cache().erase(it);
        return false;
    }
    ips = it->second.ips;
    return true;
}

inline void cache_put(const std::string& host, const std::vector<std::string>& ips,
                      uint32_t ttl) {
    if (ttl < kMinTTL) ttl = kMinTTL;
    if (ttl > kMaxTTL) ttl = kMaxTTL;
    std::lock_guard<std::mutex> lock(cache_mutex());
    cache()[host] = Entry{ips, std::chrono::steady_clock::now() +
                               std::chrono::seconds(ttl)};
}

inline std::vector<std::string> lookup(const std::string& host, std::string& err) {
    std::vector<std::string> ips;
    if (cache_get(lower(host), ips)) return ips;

    std::string name = lower(host);
    uint32_t ttl = kMaxTTL;

    // One query per family, each chased through its own CNAMEs. A name with
    // only an A record answers AAAA with NODATA, which is not an error.
    for (uint16_t type : {kTypeA, kTypeAAAA}) {
        std::vector<Record> records;
        if (!ask(name, type, records)) {
            err = "no answer from StarDNS at " + server().host + ":" +
                  std::to_string(server().port);
            return {};
        }
        std::string current = name;
        for (int hop = 0; hop < kMaxChase; hop++) {
            bool moved = false;
            for (const auto& r : records) {
                if (r.owner != current) continue;
                if (!r.ip.empty() && r.type == type) {
                    ips.push_back(r.ip);
                    if (r.ttl < ttl) ttl = r.ttl;
                } else if (!r.target.empty()) {
                    current = r.target;
                    moved = true;
                    break;
                }
            }
            if (!moved) break;
        }
    }

    if (ips.empty()) {
        err = host + " has no address record in ." + zone();
        return {};
    }
    cache_put(name, ips, ttl);

    // Once per name per TTL, not once per fetch — the cache keeps subresource
    // loads quiet.
    std::cerr << "[StarDNS] " << name << " -> ";
    for (size_t i = 0; i < ips.size(); i++) std::cerr << (i ? ", " : "") << ips[i];
    std::cerr << " (ttl " << ttl << "s)\n";
    return ips;
}

inline bool to_endpoint(const std::string& ip, uint16_t port, Endpoint& ep) {
    std::memset(&ep.addr, 0, sizeof ep.addr);
    sockaddr_in* v4 = (sockaddr_in*)&ep.addr;
    if (inet_pton(AF_INET, ip.c_str(), &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        v4->sin_port = htons(port);
        ep.len = sizeof(sockaddr_in);
        ep.family = AF_INET;
        return true;
    }
    std::memset(&ep.addr, 0, sizeof ep.addr);
    sockaddr_in6* v6 = (sockaddr_in6*)&ep.addr;
    if (inet_pton(AF_INET6, ip.c_str(), &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(port);
        ep.len = sizeof(sockaddr_in6);
        ep.family = AF_INET6;
        return true;
    }
    return false;
}

}  // namespace detail

/// True when `host` is a name this resolver owns, so callers can say where an
/// address came from.
inline bool is_starweb_name(const std::string& host) {
    return !detail::server().host.empty() && detail::in_zone(host);
}

/// Addresses to try, in order. In-zone names go to StarDNS; everything else
/// goes to getaddrinfo, which also covers IP literals and localhost.
inline std::vector<Endpoint> resolve(const std::string& host, uint16_t port,
                                     std::string& err) {
    std::vector<Endpoint> endpoints;

    if (is_starweb_name(host)) {
        for (const std::string& ip : detail::lookup(host, err)) {
            Endpoint ep;
            if (detail::to_endpoint(ip, port, ep)) endpoints.push_back(ep);
        }
        // Deliberately no fall-through to the system resolver: a .star name is
        // ours to answer, and asking the public DNS for one leaks the lookup.
        return endpoints;
    }

    addrinfo hints{}, *list = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    std::string port_str = std::to_string(port);
    int status = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &list);
    if (status != 0) {
        err = gai_strerror(status);
        return endpoints;
    }
    for (addrinfo* rp = list; rp != nullptr; rp = rp->ai_next) {
        Endpoint ep;
        std::memcpy(&ep.addr, rp->ai_addr, rp->ai_addrlen);
        ep.len = (socklen_t)rp->ai_addrlen;
        ep.family = rp->ai_family;
        endpoints.push_back(ep);
    }
    freeaddrinfo(list);
    return endpoints;
}

}  // namespace stardns
