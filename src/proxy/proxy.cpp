#include "proxy.hpp"

#include "../common/conn.hpp"
#include "../common/static_files.hpp"
#include "../common/stwp_msg.hpp"
#include "router.hpp"
#include "upstream.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <mutex>
#include <set>
#include <vector>

namespace proxy {
namespace {

constexpr size_t kMaxHead = 16 * 1024;
constexpr int kClientReadTimeoutMs = 10000;
constexpr int kClientSendTimeoutMs = 60000;
constexpr size_t kChunk = 64 * 1024;
constexpr const char* kServerName = "stwp_proxy";

std::atomic<bool> g_shutting_down{false};
std::atomic<uint64_t> g_generation{0};
std::atomic<int> g_active{0};

std::mutex g_idle_mutex;
std::set<net::socket_t> g_idle;

bool enter_idle(net::socket_t fd) {
    std::lock_guard<std::mutex> lock(g_idle_mutex);
    if (g_shutting_down) return false;
    g_idle.insert(fd);
    return true;
}

void leave_idle(net::socket_t fd) {
    std::lock_guard<std::mutex> lock(g_idle_mutex);
    g_idle.erase(fd);
}

using Clock = std::chrono::steady_clock;

enum class ReadStatus { Ok, Closed, Timeout, Error, TooLarge };

ReadStatus read_more(Conn& c, std::string& buf, int timeout_ms) {
    net::set_recv_timeout_ms(c.fd(), timeout_ms);
    char tmp[16384];
    net::ssize_t_ n = c.read(tmp, sizeof(tmp));
    if (n == 0) return ReadStatus::Closed;
    if (n < 0) return net::last_error_was_timeout() ? ReadStatus::Timeout : ReadStatus::Error;
    buf.append(tmp, (size_t)n);
    return ReadStatus::Ok;
}

ReadStatus read_head(Conn& c, std::string& buf, size_t& head_len, size_t& sep_len,
                     int idle_ms, int active_ms) {
    for (;;) {
        head_len = find_header_end(buf, sep_len);
        if (head_len != std::string::npos) return head_len > kMaxHead ? ReadStatus::TooLarge : ReadStatus::Ok;
        if (buf.size() > kMaxHead) return ReadStatus::TooLarge;
        ReadStatus rs = read_more(c, buf, buf.empty() ? idle_ms : active_ms);
        if (rs != ReadStatus::Ok) return rs;
    }
}

const char* status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 413: return "Content Too Large";
        case 416: return "Range Not Satisfiable";
        case 421: return "Misdirected Request";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        case 505: return "Version Not Supported";
        default: return "Unknown";
    }
}

bool wants_keep_alive(const std::unordered_map<std::string, std::string>& headers) {
    auto it = headers.find("connection");
    if (it == headers.end()) return false;
    std::string v = it->second;
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return std::tolower(c); });
    return v.find("keep-alive") != std::string::npos && v.find("close") == std::string::npos;
}

struct Exchange {
    std::string request = "-";
    std::string host = "-";
    std::string upstream = "-";
    int status = 0;
    uint64_t bytes = 0;
    Clock::time_point start = Clock::now();
};

class Session {
public:
    Session(std::shared_ptr<Runtime> rt, std::unique_ptr<Conn> client, std::string ip,
            uint16_t port, bool star)
        : rt_(std::move(rt)), client_(std::move(client)), ip_(std::move(ip)), port_(port), star_(star) {}

    void run() {
        while (handle_one()) {
        }
    }

private:
    std::shared_ptr<Runtime> rt_;
    std::unique_ptr<Conn> client_;
    std::string ip_;
    uint16_t port_;
    bool star_;
    std::string buf_;

    StwpRequest req_;
    Exchange ex_;
    std::string host_name_;
    bool keep_alive_ = false;
    bool head_only_ = false;

    bool still_current() const {
        return !shutting_down() && rt_->generation == g_generation.load();
    }

    bool handle_one() {
        const Config& cfg = rt_->cfg;
        req_ = StwpRequest{};
        ex_ = Exchange{};
        host_name_.clear();
        keep_alive_ = false;
        head_only_ = false;

        size_t head_len = 0, sep_len = 0;
        bool idle = buf_.empty();
        if (idle && !enter_idle(client_->fd())) return false;
        ReadStatus rs = read_head(*client_, buf_, head_len, sep_len, cfg.keepalive_timeout_ms, kClientReadTimeoutMs);
        if (idle) leave_idle(client_->fd());
        if (rs == ReadStatus::TooLarge) return finish(respond(431, "Request head too large.", false));
        if (rs != ReadStatus::Ok) return false;
        ex_.start = Clock::now();

        bool parsed = parse_request_headers(std::string_view(buf_).substr(0, head_len), req_);
        buf_.erase(0, head_len + sep_len);
        if (!parsed || req_.path[0] != '/') return finish(respond(400, "Malformed request.", false));
        ex_.request = req_.method + " " + req_.path + " " + req_.version;

        if (req_.version != "STWP/1.0") return finish(respond(505, "This server speaks STWP/1.0 only.", false));

        uint64_t body_len = 0;
        auto cl = req_.headers.find("content-length");
        if (cl != req_.headers.end() && !parse_content_length(cl->second, body_len)) {
            return finish(respond(400, "Invalid Content-Length.", false));
        }
        if (body_len > cfg.client_max_body_size) return finish(respond(413, "Request body too large.", false));
        while (buf_.size() < body_len) {
            if (read_more(*client_, buf_, kClientReadTimeoutMs) != ReadStatus::Ok) return false;
        }
        req_.body = buf_.substr(0, (size_t)body_len);
        buf_.erase(0, (size_t)body_len);

        head_only_ = req_.method == "HEAD";
        keep_alive_ = wants_keep_alive(req_.headers) && still_current();
        if (auto host = req_.headers.find("host"); host != req_.headers.end()) {
            ex_.host = host->second;
            host_name_ = normalize_host(host->second);
        }

        const Server* srv = pick_server(cfg, port_, host_name_);
        if (const TlsInfo* tls = client_->tls_info(); tls && !tls->sni.empty() &&
            pick_server(cfg, port_, normalize_host(tls->sni)) != srv) {
            return finish(respond(421, "The TLS server name does not match Host.", false));
        }
        std::string path = req_.path.substr(0, req_.path.find_first_of("?#"));
        const Location* loc = srv ? pick_location(*srv, path) : nullptr;

        if (!loc) return finish(respond(404, "Not Found", keep_alive_));
        if (loc->return_code) return finish(respond(loc->return_code, loc->return_body, keep_alive_));
        if (!loc->root.empty()) return finish(serve_static(*loc));
        return finish(forward(*loc));
    }

    bool serve_static(const Location& loc) {
        if (req_.method != "GET") return respond(405, "Only GET is supported here.", keep_alive_);
        StwpResponse res;
        res.headers["Server"] = kServerName;
        auto result = static_files::send_file(*client_, loc.root, req_.path, req_, std::move(res),
                                              keep_alive_ && still_current(), loc.index);
        ex_.status = result.status;
        ex_.bytes = result.body_bytes;
        return result.keep_alive;
    }

    bool finish(bool keep_going) {
        if (ex_.status != 0) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - ex_.start).count();
            rt_->access.write(ip_ + " [" + local_time("%d/%b/%Y:%H:%M:%S %z") + "] \"" + log_safe(ex_.request) +
                              "\" " + std::to_string(ex_.status) + " " + std::to_string(ex_.bytes) + " \"" +
                              log_safe(ex_.host) + "\" upstream=" + ex_.upstream + " " + std::to_string(ms) + "ms");
        }
        return keep_going;
    }

    bool respond(int code, const std::string& body, bool keep_alive) {
        keep_alive = keep_alive && still_current();
        StwpResponse res;
        res.status_code = code;
        res.status_text = status_text(code);
        res.headers["Server"] = kServerName;
        res.headers["Content-Type"] = "text/plain";
        res.headers["Content-Length"] = std::to_string(body.size());
        res.headers["Connection"] = keep_alive ? "keep-alive" : "close";
        if (!head_only_) res.body = body;
        ex_.status = code;
        ex_.bytes = res.body.size();
        std::string wire = res.serialize();
        return write_all(*client_, wire.data(), wire.size()) && keep_alive;
    }

    void log_upstream(const std::string& detail) {
        log_error(*rt_, "upstream " + ex_.upstream + ": " + detail + ", client " + ip_ + ", request \"" +
                            log_safe(ex_.request) + "\"");
    }

    bool upstream_failed(int code, const std::string& detail) {
        log_upstream(detail);
        return respond(code, status_text(code), keep_alive_);
    }

    std::string expand(const std::string& value, const Location& loc) const {
        std::string out;
        for (size_t i = 0; i < value.size(); ++i) {
            if (value[i] != '$') {
                out += value[i];
                continue;
            }
            size_t j = i + 1;
            while (j < value.size() && (std::islower((unsigned char)value[j]) || value[j] == '_')) ++j;
            std::string var = value.substr(i + 1, j - i - 1);
            if (var == "remote_addr") out += ip_;
            else if (var == "host") out += host_name_;
            else if (var == "scheme") out += star_ ? "star" : "moon";
            else if (var == "request_uri") out += req_.path;
            else if (var == "proxy_host") out += loc.pass.proxy_host;
            else if (var == "server_port") out += std::to_string(port_);
            i = j - 1;
        }
        return out;
    }

    std::string upstream_request(const Location& loc, bool pooled) const {
        StwpRequest up;
        up.method = req_.method;
        up.version = req_.version;
        if (!loc.pass.has_uri) {
            up.path = req_.path;
        } else if (loc.match == Match::Exact) {
            auto q = req_.path.find('?');
            up.path = loc.pass.uri + (q == std::string::npos ? "" : req_.path.substr(q));
        } else {
            up.path = loc.pass.uri + req_.path.substr(loc.path.size());
        }

        up.headers = req_.headers;
        up.headers.erase("connection");
        up.headers.erase("content-length");

        std::string& chain = up.headers["star-forwarded-for"];
        chain = chain.empty() ? ip_ : chain + ", " + ip_;
        up.headers["star-forwarded-proto"] = star_ ? "star" : "moon";
        if (ex_.host != "-") up.headers["star-forwarded-host"] = ex_.host;

        for (const HeaderRule& rule : loc.set_headers) {
            std::string v = expand(rule.value, loc);
            if (v.empty()) up.headers.erase(rule.name);
            else up.headers[rule.name] = v;
        }

        up.headers["connection"] = pooled ? "keep-alive" : "close";
        if (!req_.body.empty() || req_.headers.count("content-length")) {
            up.headers["content-length"] = std::to_string(req_.body.size());
        }
        up.body = req_.body;
        return up.serialize();
    }

    bool forward(const Location& loc) {
        UpstreamGroup& group = *rt_->groups[(size_t)loc.pass.upstream];
        const Upstream& upstream = group.config();
        TlsContext* tls = loc.pass.star ? rt_->client_tls.get() : nullptr;
        bool pooled = upstream.keepalive > 0;
        bool idempotent = req_.method == "GET" || req_.method == "HEAD";
        std::string wire = upstream_request(loc, pooled);

        std::vector<bool> tried(upstream.peers.size(), false);
        for (int peer = group.pick(tried); peer >= 0; peer = group.pick(tried)) {
            tried[(size_t)peer] = true;
            const Peer& p = upstream.peers[(size_t)peer];
            ex_.upstream = peer_name(p);

            std::unique_ptr<Conn> conn = group.take_idle(peer);
            bool reused = conn != nullptr;
            std::string ubuf, err;
            size_t head_len = 0, sep_len = 0;
            bool sent = false;
            ReadStatus rs = ReadStatus::Error;

            for (;;) {
                if (!conn && !(conn = dial(p, tls, loc.connect_timeout_ms, err))) break;
                net::set_send_timeout_ms(conn->fd(), loc.read_timeout_ms);
                sent = write_all(*conn, wire.data(), wire.size());
                if (sent) rs = read_head(*conn, ubuf, head_len, sep_len, loc.read_timeout_ms, loc.read_timeout_ms);
                bool stale = reused && ubuf.empty() &&
                             (!sent || (idempotent && (rs == ReadStatus::Closed || rs == ReadStatus::Error)));
                if (!stale) break;
                conn.reset();
                reused = false;
            }

            if (!conn) {
                group.report(peer, false);
                log_upstream(err);
                continue;
            }
            if (!sent || rs != ReadStatus::Ok) {
                group.report(peer, false);
                if (!sent) return upstream_failed(502, "sending the request failed");
                if (rs == ReadStatus::Timeout) return upstream_failed(504, "timed out waiting for the response");
                if (rs == ReadStatus::TooLarge) return upstream_failed(502, "response head too large");
                return upstream_failed(502, "closed without responding");
            }

            group.report(peer, true);
            bool reusable = false;
            bool keep = relay(*conn, ubuf, head_len, sep_len, reusable);
            if (reusable && pooled) group.park(peer, std::move(conn));
            return keep;
        }
        return respond(502, status_text(502), keep_alive_);
    }

    bool relay(Conn& up, std::string& ubuf, size_t head_len, size_t sep_len, bool& reusable) {
        reusable = false;
        StwpResponse res;
        if (!parse_response_headers(std::string_view(ubuf).substr(0, head_len), res) || res.version != "STWP/1.0") {
            return upstream_failed(502, "malformed response head");
        }
        ubuf.erase(0, head_len + sep_len);

        uint64_t declared = 0;
        auto cl = res.headers.find("content-length");
        bool has_len = cl != res.headers.end();
        if (has_len && !parse_content_length(cl->second, declared)) {
            return upstream_failed(502, "invalid Content-Length in response");
        }
        bool until_close = !has_len && !head_only_;
        uint64_t body_len = head_only_ ? 0 : declared;
        bool keep = keep_alive_ && !until_close && still_current();
        bool upstream_keep = wants_keep_alive(res.headers);

        res.headers["connection"] = keep ? "keep-alive" : "close";
        std::string head = res.serialize();
        ex_.status = res.status_code;
        if (!write_all(*client_, head.data(), head.size())) return false;

        uint64_t sent = 0;
        if (!ubuf.empty() && (until_close || body_len > 0)) {
            size_t take = until_close ? ubuf.size() : (size_t)std::min<uint64_t>(ubuf.size(), body_len);
            if (!write_all(*client_, ubuf.data(), take)) return false;
            sent += take;
            ubuf.erase(0, take);
        }

        std::vector<char> chunk(kChunk);
        while (until_close || sent < body_len) {
            size_t want = until_close ? chunk.size() : (size_t)std::min<uint64_t>(chunk.size(), body_len - sent);
            net::ssize_t_ n = up.read(chunk.data(), want);
            if (n == 0 && until_close) break;
            if (n <= 0) {
                ex_.bytes = sent;
                log_error(*rt_, "upstream " + ex_.upstream + ": " +
                                    (n < 0 && net::last_error_was_timeout() ? "timed out" : "closed") +
                                    " after " + std::to_string(sent) + " of " + std::to_string(body_len) +
                                    " body bytes, client " + ip_ + ", request \"" + log_safe(ex_.request) + "\"");
                return false;
            }
            if (!write_all(*client_, chunk.data(), (size_t)n)) {
                ex_.bytes = sent;
                return false;
            }
            sent += (uint64_t)n;
        }
        ex_.bytes = sent;
        reusable = !until_close && !head_only_ && upstream_keep && ubuf.empty();
        return keep;
    }
};

// Unread request bytes at close() make the kernel send RST, destroying an error response not yet read.
void linger_close(Conn& conn) {
    conn.shutdown_write();
    net::socket_t fd = conn.fd();
    net::set_recv_timeout_ms(fd, 500);
    char sink[16384];
    for (size_t drained = 0; drained < (1u << 20);) {
        net::ssize_t_ n = ::recv(fd, sink, sizeof(sink), 0);
        if (n <= 0) break;
        drained += (size_t)n;
    }
}

} // namespace

std::shared_ptr<Runtime> build_runtime(Config cfg, std::string& err) {
    auto rt = std::make_shared<Runtime>();
    rt->cfg = std::move(cfg);
    const Config& c = rt->cfg;
    if (!rt->error.open(c.error_log, stderr, err)) return nullptr;
    if (!rt->access.open(c.access_log, stdout, err)) return nullptr;

    rt->server_tls.resize(c.servers.size());
    for (size_t i = 0; i < c.servers.size(); ++i) {
        const Server& srv = c.servers[i];
        if (srv.cert.empty()) continue;
        std::string tls_err;
        rt->server_tls[i] = TlsContext::make_server(srv.cert, srv.key, tls_err);
        if (!rt->server_tls[i]) {
            err = srv.file + ":" + std::to_string(srv.line) + ": " + tls_err;
            return nullptr;
        }
    }

    Runtime* raw = rt.get();
    for (const Server& srv : c.servers) {
        for (const Listen& l : srv.listens) {
            if (!l.star || rt->port_tls.count(l.port)) continue;
            const Server* def = pick_server(c, l.port, "");
            std::string tls_err;
            auto entry = TlsContext::make_server(def->cert, def->key, tls_err);
            if (!entry) {
                err = def->file + ":" + std::to_string(def->line) + ": " + tls_err;
                return nullptr;
            }
            uint16_t port = l.port;
            entry->set_sni_selector([raw, port](const std::string& name) -> TlsContext* {
                const Server* chosen = pick_server(raw->cfg, port, normalize_host(name));
                return chosen ? raw->server_tls[(size_t)(chosen - raw->cfg.servers.data())].get() : nullptr;
            });
            rt->port_tls[port] = std::move(entry);
        }
    }

    for (const Upstream& u : c.upstreams) rt->groups.push_back(std::make_unique<UpstreamGroup>(u));

    bool star_upstream = std::any_of(c.servers.begin(), c.servers.end(), [](const Server& srv) {
        return std::any_of(srv.locations.begin(), srv.locations.end(),
                           [](const Location& loc) { return loc.proxy && loc.pass.star; });
    });
    if (star_upstream) {
        rt->client_tls = TlsContext::make_client(c.trusted_ca, err);
        if (!rt->client_tls) return nullptr;
    }
    return rt;
}

void log_error(Runtime& rt, const std::string& message) {
    rt.error.write(local_time("%Y/%m/%d %H:%M:%S") + " [error] " + message);
}

void serve_connection(std::shared_ptr<Runtime> rt, net::socket_t fd, std::string client_ip,
                      uint16_t port, bool star) {
    struct Active {
        Active() { ++g_active; }
        ~Active() { --g_active; }
    } active;
    net::set_nodelay(fd);
    net::set_send_timeout_ms(fd, kClientSendTimeoutMs);

    std::unique_ptr<Conn> conn;
    if (star) {
        net::set_recv_timeout_ms(fd, kClientReadTimeoutMs);
        std::string err = "no TLS context for port " + std::to_string(port);
        auto entry = rt->port_tls.find(port);
        std::unique_ptr<TlsConn> tconn;
        if (entry != rt->port_tls.end()) tconn = TlsConn::accept(*entry->second, fd, err);
        if (!tconn) {
            log_error(*rt, "TLS handshake with client " + client_ip + " on port " + std::to_string(port) +
                               " failed: " + err);
            net::close(fd);
            return;
        }
        conn = std::move(tconn);
    } else {
        conn = std::make_unique<PlainConn>(fd);
    }

    Conn& client = *conn;
    Session session(std::move(rt), std::move(conn), std::move(client_ip), port, star);
    session.run();
    linger_close(client);
}

void log_notice(Runtime& rt, const std::string& message) {
    rt.error.write(local_time("%Y/%m/%d %H:%M:%S") + " [notice] " + message);
}

void activate(Runtime& rt) { rt.generation = ++g_generation; }

bool shutting_down() { return g_shutting_down.load(); }

void begin_shutdown() {
    {
        std::lock_guard<std::mutex> lock(g_idle_mutex);
        g_shutting_down = true;
    }
    close_idle_connections();
}

void close_idle_connections() {
    std::lock_guard<std::mutex> lock(g_idle_mutex);
    for (net::socket_t fd : g_idle) net::shutdown_both(fd);
}

int active_connections() { return g_active.load(); }

} // namespace proxy
