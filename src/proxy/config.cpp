#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace proxy {
namespace {

constexpr int kMaxIncludeDepth = 16;

struct Node {
    std::string name;
    std::vector<std::string> args;
    std::vector<Node> children;
    bool block = false;
    std::string file;
    int line = 0;
};

struct Token {
    enum Kind { Word, Open, Close, Semi } kind;
    std::string text;
    int line;
};

std::string at(const std::string& file, int line) {
    return file + ":" + std::to_string(line) + ": ";
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool tokenize(const std::string& file, const std::string& src, std::vector<Token>& out,
              std::string& err) {
    int line = 1;
    size_t i = 0, n = src.size();
    while (i < n) {
        char c = src[i];
        if (c == '\n') { ++line; ++i; continue; }
        if (std::isspace((unsigned char)c)) { ++i; continue; }
        if (c == '#') { while (i < n && src[i] != '\n') ++i; continue; }
        if (c == '{') { out.push_back({Token::Open, "{", line}); ++i; continue; }
        if (c == '}') { out.push_back({Token::Close, "}", line}); ++i; continue; }
        if (c == ';') { out.push_back({Token::Semi, ";", line}); ++i; continue; }

        if (c == '"' || c == '\'') {
            int start_line = line;
            std::string text;
            bool closed = false;
            for (++i; i < n; ++i) {
                char d = src[i];
                if (d == c) { closed = true; ++i; break; }
                if (d == '\\' && i + 1 < n) {
                    char e = src[++i];
                    text += e == 'n' ? '\n' : e == 't' ? '\t' : e;
                    continue;
                }
                if (d == '\n') ++line;
                text += d;
            }
            if (!closed) { err = at(file, start_line) + "unterminated string"; return false; }
            out.push_back({Token::Word, text, start_line});
            continue;
        }

        std::string text;
        while (i < n && !std::isspace((unsigned char)src[i]) &&
               src[i] != '{' && src[i] != '}' && src[i] != ';') {
            text += src[i++];
        }
        out.push_back({Token::Word, text, line});
    }
    return true;
}

bool wildmatch(const char* pat, const char* s) {
    if (*pat == '\0') return *s == '\0';
    if (*pat == '*') return wildmatch(pat + 1, s) || (*s && wildmatch(pat, s + 1));
    if (*s && (*pat == '?' || *pat == *s)) return wildmatch(pat + 1, s + 1);
    return false;
}

class Parser {
public:
    std::string err;

    bool parse_file(const std::string& path, std::vector<Node>& out, int depth) {
        if (depth > kMaxIncludeDepth) { err = path + ": includes nested too deeply"; return false; }
        std::ifstream in(path, std::ios::binary);
        if (!in) { err = "cannot open " + path; return false; }
        std::stringstream ss;
        ss << in.rdbuf();

        std::vector<Token> toks;
        if (!tokenize(path, ss.str(), toks, err)) return false;
        size_t i = 0;
        return parse_block(path, toks, i, out, false, depth);
    }

private:
    bool parse_block(const std::string& file, const std::vector<Token>& toks, size_t& i,
                     std::vector<Node>& out, bool in_block, int depth) {
        while (i < toks.size()) {
            const Token& t = toks[i];
            if (t.kind == Token::Close) {
                if (!in_block) { err = at(file, t.line) + "unexpected \"}\""; return false; }
                ++i;
                return true;
            }
            if (t.kind != Token::Word) {
                err = at(file, t.line) + "unexpected \"" + t.text + "\"";
                return false;
            }

            Node node;
            node.name = t.text;
            node.file = file;
            node.line = t.line;
            for (++i; i < toks.size() && toks[i].kind == Token::Word; ++i) {
                node.args.push_back(toks[i].text);
            }
            if (i == toks.size()) {
                err = at(file, node.line) + "unexpected end of file, expecting \";\" or \"{\"";
                return false;
            }
            if (toks[i].kind == Token::Semi) {
                ++i;
            } else if (toks[i].kind == Token::Open) {
                ++i;
                node.block = true;
                if (!parse_block(file, toks, i, node.children, true, depth)) return false;
            } else {
                err = at(file, node.line) + "directive \"" + node.name + "\" is not terminated by \";\"";
                return false;
            }

            if (node.name == "include" && !node.block) {
                if (node.args.size() != 1) {
                    err = at(file, node.line) + "invalid number of arguments in \"include\" directive";
                    return false;
                }
                std::vector<std::string> files;
                if (!expand_include(node, files)) return false;
                for (const auto& f : files) {
                    if (!parse_file(f, out, depth + 1)) return false;
                }
                continue;
            }
            out.push_back(std::move(node));
        }
        if (in_block) {
            int last = toks.empty() ? 1 : toks.back().line;
            err = at(file, last) + "unexpected end of file, expecting \"}\"";
            return false;
        }
        return true;
    }

    bool expand_include(const Node& n, std::vector<std::string>& files) {
        fs::path pattern = n.args[0];
        if (pattern.is_relative()) pattern = fs::path(n.file).parent_path() / pattern;
        std::string name = pattern.filename().string();
        fs::path dir = pattern.parent_path();
        if (dir.string().find_first_of("*?") != std::string::npos) {
            err = at(n.file, n.line) + "wildcards are only allowed in the file name of \"include\"";
            return false;
        }

        if (name.find_first_of("*?") == std::string::npos) {
            std::error_code ec;
            if (!fs::is_regular_file(pattern, ec)) {
                err = at(n.file, n.line) + "include file \"" + pattern.string() + "\" not found";
                return false;
            }
            files.push_back(pattern.string());
            return true;
        }

        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(dir.empty() ? "." : dir, ec)) {
            std::string fname = entry.path().filename().string();
            if (entry.is_regular_file(ec) && wildmatch(name.c_str(), fname.c_str())) {
                files.push_back(entry.path().string());
            }
        }
        std::sort(files.begin(), files.end());
        return true;
    }
};

const std::set<std::string>& all_directives() {
    static const std::set<std::string> names = {
        "error_log", "stwp", "access_log", "keepalive_timeout", "client_max_body_size",
        "tls_trusted_certificate", "upstream", "server", "keepalive", "listen",
        "server_name", "tls_certificate", "tls_certificate_key", "location", "proxy_pass",
        "proxy_set_header", "proxy_connect_timeout", "proxy_read_timeout", "root", "index",
        "return",
    };
    return names;
}

const std::set<std::string>& known_variables() {
    static const std::set<std::string> names = {
        "remote_addr", "host", "scheme", "request_uri", "proxy_host", "server_port",
    };
    return names;
}

bool all_digits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
}

bool parse_duration(const std::string& s, int& ms) {
    size_t k = 0;
    while (k < s.size() && std::isdigit((unsigned char)s[k])) ++k;
    if (k == 0 || k > 9) return false;
    long long v = std::stoll(s.substr(0, k));
    std::string unit = s.substr(k);
    long long mult;
    if (unit.empty() || unit == "s") mult = 1000;
    else if (unit == "ms") mult = 1;
    else if (unit == "m") mult = 60 * 1000;
    else if (unit == "h") mult = 3600 * 1000;
    else return false;
    if (v * mult > 24LL * 3600 * 1000) return false;
    ms = (int)(v * mult);
    return true;
}

bool parse_size(const std::string& s, uint64_t& bytes) {
    size_t k = 0;
    while (k < s.size() && std::isdigit((unsigned char)s[k])) ++k;
    if (k == 0 || k > 12) return false;
    uint64_t v = std::stoull(s.substr(0, k));
    std::string unit = lower(s.substr(k));
    if (unit.empty()) bytes = v;
    else if (unit == "k") bytes = v << 10;
    else if (unit == "m") bytes = v << 20;
    else if (unit == "g") bytes = v << 30;
    else return false;
    return true;
}

bool parse_port(const std::string& s, uint16_t& port) {
    if (!all_digits(s) || s.size() > 5) return false;
    int v = std::stoi(s);
    if (v < 1 || v > 65535) return false;
    port = (uint16_t)v;
    return true;
}

bool valid_hostname(const std::string& h) {
    return !h.empty() && std::all_of(h.begin(), h.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '.' || c == '-' || c == '_' || c == ':';
    });
}

bool parse_host_port(const std::string& s, std::string& host, uint16_t& port, bool& has_port) {
    has_port = false;
    std::string rest;
    if (!s.empty() && s[0] == '[') {
        auto close = s.find(']');
        if (close == std::string::npos) return false;
        host = s.substr(1, close - 1);
        rest = s.substr(close + 1);
        if (!rest.empty() && rest[0] != ':') return false;
    } else {
        auto colon = s.rfind(':');
        if (colon != std::string::npos && s.find(':') != colon) return false;
        host = s.substr(0, colon);
        if (colon != std::string::npos) rest = s.substr(colon);
    }
    if (!rest.empty()) {
        if (!parse_port(rest.substr(1), port)) return false;
        has_port = true;
    }
    host = lower(host);
    return valid_hostname(host);
}

std::string format_host(const std::string& host) {
    return host.find(':') != std::string::npos ? "[" + host + "]" : host;
}

struct Inherited {
    std::vector<HeaderRule> headers;
    int connect_ms = 5000;
    int read_ms = 60000;
};

class Builder {
public:
    Builder(Config& cfg, std::string& err) : cfg_(cfg), err_(err) {}

    bool build(const std::vector<Node>& top) {
        const Node* stwp = nullptr;
        for (const Node& n : top) {
            if (n.name == "error_log") {
                if (!simple(n, 1, 1)) return false;
                cfg_.error_log = n.args[0];
            } else if (n.name == "stwp") {
                if (!block(n, 0, 0)) return false;
                if (stwp) return fail(n, "\"stwp\" directive is duplicate");
                stwp = &n;
            } else {
                return misplaced(n);
            }
        }
        if (!stwp) { err_ = cfg_.path + ": no \"stwp\" block"; return false; }
        return build_stwp(*stwp) && validate();
    }

private:
    Config& cfg_;
    std::string& err_;
    std::vector<std::map<std::string, const Node*>> server_nodes_;
    const Node* ca_node_ = nullptr;
    const Node* first_star_pass_ = nullptr;

    bool fail(const Node& n, const std::string& msg) {
        err_ = at(n.file, n.line) + msg;
        return false;
    }

    bool misplaced(const Node& n) {
        if (all_directives().count(n.name)) return fail(n, "\"" + n.name + "\" directive is not allowed here");
        return fail(n, "unknown directive \"" + n.name + "\"");
    }

    bool arity(const Node& n, size_t min, size_t max) {
        if (n.args.size() < min || n.args.size() > max) {
            return fail(n, "invalid number of arguments in \"" + n.name + "\" directive");
        }
        return true;
    }

    bool simple(const Node& n, size_t min, size_t max) {
        if (n.block) return fail(n, "directive \"" + n.name + "\" takes no block");
        return arity(n, min, max);
    }

    bool block(const Node& n, size_t min, size_t max) {
        if (!n.block) return fail(n, "directive \"" + n.name + "\" has no opening \"{\"");
        return arity(n, min, max);
    }

    bool once(std::map<std::string, const Node*>& seen, const Node& n) {
        if (seen.count(n.name)) return fail(n, "\"" + n.name + "\" directive is duplicate");
        seen[n.name] = &n;
        return true;
    }

    bool duration(const Node& n, int& ms) {
        if (!parse_duration(n.args[0], ms)) return fail(n, "invalid time \"" + n.args[0] + "\" in \"" + n.name + "\"");
        return true;
    }

    // Returns 1 if n was one of the inheritable proxy settings, 0 if not, -1 on error.
    int inheritable(const Node& n, Inherited& inh, bool& headers_reset,
                    std::map<std::string, const Node*>& seen) {
        if (n.name == "proxy_set_header") {
            if (!simple(n, 2, 2)) return -1;
            if (!headers_reset) { inh.headers.clear(); headers_reset = true; }
            HeaderRule rule{lower(n.args[0]), n.args[1]};
            bool name_ok = std::all_of(rule.name.begin(), rule.name.end(), [](unsigned char c) {
                return std::isalnum(c) || c == '-' || c == '_';
            });
            if (rule.name.empty() || !name_ok) { fail(n, "invalid header name \"" + n.args[0] + "\""); return -1; }
            if (rule.name == "content-length" || rule.name == "connection") {
                { fail(n, "\"" + rule.name + "\" is managed by the proxy and cannot be set"); return -1; }
            }
            if (!check_variables(n, rule.value)) return -1;
            for (auto& existing : inh.headers) {
                if (existing.name == rule.name) { fail(n, "header \"" + rule.name + "\" is set twice"); return -1; }
            }
            inh.headers.push_back(rule);
            return 1;
        }
        if (n.name == "proxy_connect_timeout" || n.name == "proxy_read_timeout") {
            if (!simple(n, 1, 1) || !once(seen, n)) return -1;
            int& target = n.name == "proxy_connect_timeout" ? inh.connect_ms : inh.read_ms;
            if (!duration(n, target)) return -1;
            if (target == 0) { fail(n, "\"" + n.name + "\" must be greater than 0"); return -1; }
            return 1;
        }
        return 0;
    }

    bool check_variables(const Node& n, const std::string& value) {
        for (size_t i = value.find('$'); i != std::string::npos; i = value.find('$', i + 1)) {
            size_t j = i + 1;
            while (j < value.size() && (std::islower((unsigned char)value[j]) || value[j] == '_')) ++j;
            std::string var = value.substr(i + 1, j - i - 1);
            if (var.empty()) return fail(n, "invalid variable name in \"" + value + "\"");
            if (!known_variables().count(var)) return fail(n, "unknown \"" + var + "\" variable");
        }
        return true;
    }

    bool build_stwp(const Node& stwp) {
        std::map<std::string, const Node*> seen;
        Inherited inh;
        bool headers_reset = false;
        std::vector<const Node*> servers;

        for (const Node& n : stwp.children) {
            int r = inheritable(n, inh, headers_reset, seen);
            if (r < 0) return false;
            if (r > 0) continue;

            if (n.name == "access_log") {
                if (!simple(n, 1, 1) || !once(seen, n)) return false;
                cfg_.access_log = n.args[0];
            } else if (n.name == "keepalive_timeout") {
                if (!simple(n, 1, 1) || !once(seen, n) || !duration(n, cfg_.keepalive_timeout_ms)) return false;
            } else if (n.name == "client_max_body_size") {
                if (!simple(n, 1, 1) || !once(seen, n)) return false;
                if (!parse_size(n.args[0], cfg_.client_max_body_size)) {
                    return fail(n, "invalid size \"" + n.args[0] + "\" in \"client_max_body_size\"");
                }
            } else if (n.name == "tls_trusted_certificate") {
                if (!simple(n, 1, 1) || !once(seen, n)) return false;
                cfg_.trusted_ca = n.args[0];
                ca_node_ = &n;
            } else if (n.name == "upstream") {
                if (!block(n, 1, 1) || !build_upstream(n)) return false;
            } else if (n.name == "server") {
                if (!block(n, 0, 0)) return false;
                servers.push_back(&n);
            } else {
                return misplaced(n);
            }
        }

        if (cfg_.trusted_ca.empty()) {
            const char* env = std::getenv("STARWEB_CA");
            cfg_.trusted_ca = env ? env : "certs/starweb_root.pem";
        }

        if (servers.empty()) { err_ = cfg_.path + ": no \"server\" blocks"; return false; }
        for (const Node* s : servers) {
            if (!build_server(*s, inh)) return false;
        }
        return true;
    }

    bool build_upstream(const Node& u) {
        for (const auto& existing : cfg_.upstreams) {
            if (lower(existing.name) == lower(u.args[0])) return fail(u, "duplicate upstream \"" + u.args[0] + "\"");
        }
        Upstream up;
        up.name = u.args[0];
        std::map<std::string, const Node*> seen;

        for (const Node& n : u.children) {
            if (n.name == "server") {
                if (!simple(n, 1, 4)) return false;
                Peer p;
                bool has_port = false;
                if (!parse_host_port(n.args[0], p.host, p.port, has_port)) {
                    return fail(n, "invalid address \"" + n.args[0] + "\"");
                }
                if (!has_port) return fail(n, "upstream server \"" + n.args[0] + "\" has no port");
                for (size_t k = 1; k < n.args.size(); ++k) {
                    const std::string& a = n.args[k];
                    auto eq = a.find('=');
                    std::string key = a.substr(0, eq), val = eq == std::string::npos ? "" : a.substr(eq + 1);
                    if (key == "weight" && all_digits(val) && val.size() < 4 && std::stoi(val) > 0) {
                        p.weight = std::stoi(val);
                    } else if (key == "max_fails" && all_digits(val) && val.size() < 4) {
                        p.max_fails = std::stoi(val);
                    } else if (key == "fail_timeout" && parse_duration(val, p.fail_timeout_ms)) {
                    } else {
                        return fail(n, "invalid parameter \"" + a + "\"");
                    }
                }
                up.peers.push_back(p);
            } else if (n.name == "keepalive") {
                if (!simple(n, 1, 1) || !once(seen, n)) return false;
                if (!all_digits(n.args[0]) || n.args[0].size() > 4) {
                    return fail(n, "invalid value \"" + n.args[0] + "\" in \"keepalive\"");
                }
                up.keepalive = std::stoi(n.args[0]);
            } else {
                return misplaced(n);
            }
        }
        if (up.peers.empty()) return fail(u, "no servers are inside upstream \"" + up.name + "\"");
        cfg_.upstreams.push_back(std::move(up));
        return true;
    }

    bool build_server(const Node& s, Inherited inh) {
        Server srv;
        srv.file = s.file;
        srv.line = s.line;
        std::map<std::string, const Node*> seen;
        bool headers_reset = false;
        std::vector<const Node*> locations;

        for (const Node& n : s.children) {
            int r = inheritable(n, inh, headers_reset, seen);
            if (r < 0) return false;
            if (r > 0) continue;

            if (n.name == "listen") {
                if (!simple(n, 1, 3)) return false;
                Listen l;
                if (!parse_port(n.args[0], l.port)) return fail(n, "invalid port \"" + n.args[0] + "\" in \"listen\"");
                for (size_t k = 1; k < n.args.size(); ++k) {
                    if (n.args[k] == "star") l.star = true;
                    else if (n.args[k] == "default_server") l.default_server = true;
                    else return fail(n, "invalid parameter \"" + n.args[k] + "\"");
                }
                for (const auto& other : srv.listens) {
                    if (other.port == l.port) return fail(n, "duplicate listen on port " + n.args[0]);
                }
                srv.listens.push_back(l);
                seen["listen:" + std::to_string(l.port)] = &n;
            } else if (n.name == "server_name") {
                if (!simple(n, 1, 64)) return false;
                for (const auto& raw : n.args) {
                    std::string name = lower(raw);
                    if (!name.empty() && name.back() == '.') name.pop_back();
                    auto star = name.find('*');
                    bool ok = valid_hostname(name.substr(star == 0 ? 1 : 0)) &&
                              (star == std::string::npos || (star == 0 && name.size() > 2 && name[1] == '.'));
                    if (!ok || name.find('*', 1) != std::string::npos) {
                        return fail(n, "invalid server name \"" + raw + "\"");
                    }
                    srv.names.push_back(name);
                }
            } else if (n.name == "tls_certificate" || n.name == "tls_certificate_key") {
                if (!simple(n, 1, 1) || !once(seen, n)) return false;
                (n.name == "tls_certificate" ? srv.cert : srv.key) = n.args[0];
            } else if (n.name == "location") {
                if (!block(n, 1, 2)) return false;
                locations.push_back(&n);
            } else {
                return misplaced(n);
            }
        }

        for (const Node* l : locations) {
            if (!build_location(*l, inh, srv)) return false;
        }
        seen["server"] = &s;
        cfg_.servers.push_back(std::move(srv));
        server_nodes_.push_back(std::move(seen));
        return true;
    }

    bool build_location(const Node& n, Inherited inh, Server& srv) {
        Location loc;
        loc.file = n.file;
        loc.line = n.line;
        if (n.args.size() == 2) {
            const std::string& mod = n.args[0];
            if (mod == "=") loc.match = Match::Exact;
            else if (mod == "^~") loc.match = Match::Prefix;
            else if (mod == "~" || mod == "~*") return fail(n, "regex locations are not supported");
            else return fail(n, "invalid location modifier \"" + mod + "\"");
        }
        loc.path = n.args.back();
        if (loc.path.empty() || loc.path[0] != '/') return fail(n, "location \"" + loc.path + "\" must start with \"/\"");
        for (const auto& other : srv.locations) {
            if (other.match == loc.match && other.path == loc.path) {
                return fail(n, "duplicate location \"" + loc.path + "\"");
            }
        }

        std::map<std::string, const Node*> seen;
        bool headers_reset = false;
        int handlers = 0;

        for (const Node& c : n.children) {
            int r = inheritable(c, inh, headers_reset, seen);
            if (r < 0) return false;
            if (r > 0) continue;

            if (c.name == "proxy_pass") {
                if (!simple(c, 1, 1) || !once(seen, c) || !build_proxy_pass(c, loc)) return false;
                ++handlers;
            } else if (c.name == "root") {
                if (!simple(c, 1, 1) || !once(seen, c)) return false;
                loc.root = c.args[0];
                while (loc.root.size() > 1 && loc.root.back() == '/') loc.root.pop_back();
                ++handlers;
            } else if (c.name == "index") {
                if (!simple(c, 1, 1) || !once(seen, c)) return false;
                if (c.args[0].find('/') != std::string::npos) return fail(c, "invalid index \"" + c.args[0] + "\"");
                loc.index = c.args[0];
            } else if (c.name == "return") {
                if (!simple(c, 1, 2) || !once(seen, c)) return false;
                if (!all_digits(c.args[0]) || c.args[0].size() != 3 || c.args[0][0] < '1' || c.args[0][0] > '5') {
                    return fail(c, "invalid return code \"" + c.args[0] + "\"");
                }
                loc.return_code = std::stoi(c.args[0]);
                if (c.args.size() == 2) loc.return_body = c.args[1];
                ++handlers;
            } else {
                return misplaced(c);
            }
        }

        if (handlers == 0) return fail(n, "location \"" + loc.path + "\" has no \"proxy_pass\", \"root\" or \"return\"");
        if (handlers > 1) return fail(n, "location \"" + loc.path + "\" has more than one of \"proxy_pass\", \"root\" and \"return\"");

        loc.set_headers = inh.headers;
        loc.connect_timeout_ms = inh.connect_ms;
        loc.read_timeout_ms = inh.read_ms;
        srv.locations.push_back(std::move(loc));
        return true;
    }

    bool build_proxy_pass(const Node& n, Location& loc) {
        const std::string& url = n.args[0];
        ProxyPass& pp = loc.pass;
        std::string rest;
        if (url.rfind("moon://", 0) == 0) {
            rest = url.substr(7);
        } else if (url.rfind("star://", 0) == 0) {
            pp.star = true;
            rest = url.substr(7);
        } else if (url.find("://") != std::string::npos) {
            return fail(n, "proxy_pass accepts only moon:// and star:// URLs");
        } else {
            return fail(n, "invalid URL prefix in \"" + url + "\"");
        }

        auto slash = rest.find('/');
        if (slash != std::string::npos) {
            pp.has_uri = true;
            pp.uri = rest.substr(slash);
            rest = rest.substr(0, slash);
        }

        std::string host;
        uint16_t port = 0;
        bool has_port = false;
        if (!parse_host_port(rest, host, port, has_port)) return fail(n, "invalid host in \"" + url + "\"");

        if (!has_port) {
            for (size_t i = 0; i < cfg_.upstreams.size(); ++i) {
                if (!cfg_.upstreams[i].implicit && lower(cfg_.upstreams[i].name) == host) {
                    pp.upstream = (int)i;
                    pp.proxy_host = cfg_.upstreams[i].name;
                }
            }
        }
        if (pp.upstream < 0) {
            if (!has_port) port = pp.star ? 8490 : 8090;
            std::string key = format_host(host) + ":" + std::to_string(port);
            for (size_t i = 0; i < cfg_.upstreams.size(); ++i) {
                if (cfg_.upstreams[i].implicit && cfg_.upstreams[i].name == key) pp.upstream = (int)i;
            }
            if (pp.upstream < 0) {
                Upstream up;
                up.name = key;
                up.implicit = true;
                up.peers.push_back(Peer{host, port});
                cfg_.upstreams.push_back(up);
                pp.upstream = (int)cfg_.upstreams.size() - 1;
            }
            pp.proxy_host = has_port ? key : format_host(host);
        }

        loc.proxy = true;
        if (pp.star && !first_star_pass_) first_star_pass_ = &n;
        return true;
    }

    bool file_exists(const std::string& path) {
        std::error_code ec;
        return fs::is_regular_file(path, ec);
    }

    bool validate() {
        struct PortUse { bool star; const Node* node; int defaults = 0; std::map<std::string, bool> names; };
        std::map<uint16_t, PortUse> ports;

        for (size_t si = 0; si < cfg_.servers.size(); ++si) {
            const Server& srv = cfg_.servers[si];
            auto& nodes = server_nodes_[si];
            const Node& snode = *nodes["server"];
            if (srv.listens.empty()) return fail(snode, "server has no \"listen\"");

            bool star = false;
            for (const Listen& l : srv.listens) {
                const Node& lnode = *nodes["listen:" + std::to_string(l.port)];
                star = star || l.star;
                auto it = ports.find(l.port);
                if (it == ports.end()) {
                    it = ports.emplace(l.port, PortUse{l.star, &lnode, 0, {}}).first;
                } else if (it->second.star != l.star) {
                    return fail(lnode, "port " + std::to_string(l.port) + " is used both with and without \"star\"");
                }
                if (l.default_server && ++it->second.defaults > 1) {
                    return fail(lnode, "a duplicate default server for port " + std::to_string(l.port));
                }
                for (const auto& name : srv.names) {
                    if (it->second.names.count(name)) {
                        return fail(snode, "conflicting server name \"" + name + "\" on port " + std::to_string(l.port));
                    }
                    it->second.names[name] = true;
                }
            }

            if (star) {
                if (srv.cert.empty()) return fail(snode, "server listens with \"star\" but has no \"tls_certificate\"");
                if (srv.key.empty()) return fail(snode, "server listens with \"star\" but has no \"tls_certificate_key\"");
            }
            if (!srv.cert.empty() && !file_exists(srv.cert)) {
                return fail(*nodes["tls_certificate"], "certificate \"" + srv.cert + "\" not found");
            }
            if (!srv.key.empty() && !file_exists(srv.key)) {
                return fail(*nodes["tls_certificate_key"], "certificate key \"" + srv.key + "\" not found");
            }
        }

        if (first_star_pass_ && !file_exists(cfg_.trusted_ca)) {
            return fail(ca_node_ ? *ca_node_ : *first_star_pass_,
                        "trusted certificate \"" + cfg_.trusted_ca + "\" not found");
        }
        return true;
    }
};

std::string quote(const std::string& s) {
    bool plain = !s.empty() && std::none_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isspace(c) || c == ';' || c == '{' || c == '}' || c == '#' || c == '"' || c == '\'' || c == '\\';
    });
    if (plain) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (c == '\n') { out += "\\n"; continue; }
        out += c;
    }
    return out + "\"";
}

} // namespace

bool load_config(const std::string& path, Config& out, std::string& err) {
    Parser parser;
    std::vector<Node> top;
    if (!parser.parse_file(path, top, 0)) {
        err = parser.err;
        return false;
    }
    Config cfg;
    cfg.path = path;
    Builder builder(cfg, err);
    if (!builder.build(top)) return false;
    out = std::move(cfg);
    return true;
}

std::string dump_config(const Config& cfg) {
    std::ostringstream o;
    if (!cfg.error_log.empty()) o << "error_log " << quote(cfg.error_log) << ";\n";
    o << "stwp {\n";
    if (!cfg.access_log.empty()) o << "    access_log " << quote(cfg.access_log) << ";\n";
    o << "    keepalive_timeout " << cfg.keepalive_timeout_ms << "ms;\n";
    o << "    client_max_body_size " << cfg.client_max_body_size << ";\n";
    o << "    tls_trusted_certificate " << quote(cfg.trusted_ca) << ";\n";

    for (const Upstream& up : cfg.upstreams) {
        o << "\n    upstream " << quote(up.name) << " {" << (up.implicit ? "  # implicit" : "") << "\n";
        for (const Peer& p : up.peers) {
            o << "        server " << format_host(p.host) << ":" << p.port << " weight=" << p.weight
              << " max_fails=" << p.max_fails << " fail_timeout=" << p.fail_timeout_ms << "ms;\n";
        }
        o << "        keepalive " << up.keepalive << ";\n    }\n";
    }

    for (const Server& srv : cfg.servers) {
        o << "\n    server {\n";
        for (const Listen& l : srv.listens) {
            o << "        listen " << l.port << (l.star ? " star" : "") << (l.default_server ? " default_server" : "") << ";\n";
        }
        if (!srv.names.empty()) {
            o << "        server_name";
            for (const auto& n : srv.names) o << " " << n;
            o << ";\n";
        }
        if (!srv.cert.empty()) o << "        tls_certificate " << quote(srv.cert) << ";\n";
        if (!srv.key.empty()) o << "        tls_certificate_key " << quote(srv.key) << ";\n";

        for (const Location& loc : srv.locations) {
            o << "\n        location " << (loc.match == Match::Exact ? "= " : "") << quote(loc.path) << " {\n";
            if (loc.proxy) {
                o << "            proxy_pass " << (loc.pass.star ? "star://" : "moon://") << loc.pass.proxy_host
                  << loc.pass.uri << ";  # upstream " << quote(cfg.upstreams[loc.pass.upstream].name) << "\n";
                for (const auto& h : loc.set_headers) {
                    o << "            proxy_set_header " << h.name << " " << quote(h.value) << ";\n";
                }
                o << "            proxy_connect_timeout " << loc.connect_timeout_ms << "ms;\n";
                o << "            proxy_read_timeout " << loc.read_timeout_ms << "ms;\n";
            } else if (!loc.root.empty()) {
                o << "            root " << quote(loc.root) << ";\n";
                o << "            index " << quote(loc.index) << ";\n";
            } else {
                o << "            return " << loc.return_code;
                if (!loc.return_body.empty()) o << " " << quote(loc.return_body);
                o << ";\n";
            }
            o << "        }\n";
        }
        o << "    }\n";
    }
    o << "}\n";
    return o.str();
}

} // namespace proxy
