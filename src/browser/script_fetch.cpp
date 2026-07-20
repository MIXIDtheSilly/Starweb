#include "script_fetch.hpp"
#include "script.hpp"
#include "fetcher.hpp"
#include "../common/url_parser.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace {

constexpr int    kMaxDepth        = 64;
constexpr int    kMaxInflight     = 6;
constexpr size_t kMaxRequestBody  = 1u * 1024u * 1024u;
constexpr size_t kMaxResponse     = 8u * 1024u * 1024u;
constexpr size_t kMaxHeaders      = 32;
constexpr size_t kMaxHeaderLen    = 1024;
constexpr size_t kMaxUrlLen       = 4096;

// ---------------------------------------------------------------- JSON decode

struct JsonReader {
    const char* p;
    const char* end;
    lua_State*  L;
    int         depth = 0;
    std::string err;

    void skip_ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }
    bool fail(const char* what) {
        if (err.empty()) err = what;
        return false;
    }
    bool value();
    bool string_lit();
    bool number();
    bool array();
    bool object();
};

void append_utf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

bool hex4(const char* s, const char* end, unsigned& out) {
    if (end - s < 4) return false;
    out = 0;
    for (int i = 0; i < 4; ++i) {
        char c = s[i];
        unsigned d;
        if (c >= '0' && c <= '9')      d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        out = (out << 4) | d;
    }
    return true;
}

bool JsonReader::string_lit() {
    if (p >= end || *p != '"') return fail("expected string");
    ++p;
    std::string out;
    while (true) {
        if (p >= end) return fail("unterminated string");
        unsigned char c = (unsigned char)*p;
        if (c == '"') { ++p; break; }
        if (c == '\\') {
            ++p;
            if (p >= end) return fail("unterminated escape");
            char e = *p++;
            switch (e) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    unsigned cp;
                    if (!hex4(p, end, cp)) return fail("bad \\u escape");
                    p += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        unsigned lo;
                        if (end - p >= 6 && p[0] == '\\' && p[1] == 'u' &&
                            hex4(p + 2, end, lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            p += 6;
                        } else {
                            cp = 0xFFFD;
                        }
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        cp = 0xFFFD;
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: return fail("bad escape");
            }
            continue;
        }
        if (c < 0x20) return fail("control character in string");
        out += (char)c;
        ++p;
    }
    lua_pushlstring(L, out.data(), out.size());
    return true;
}

bool JsonReader::number() {
    const char* start = p;
    if (p < end && (*p == '-' || *p == '+')) ++p;
    while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' ||
                       *p == '+' || *p == '-')) {
        ++p;
    }
    if (p == start) return fail("expected number");
    std::string text(start, p - start);
    char* stop = nullptr;
    double d = std::strtod(text.c_str(), &stop);
    if (!stop || *stop != '\0' || !std::isfinite(d)) return fail("bad number");
    lua_pushnumber(L, d);
    return true;
}

bool JsonReader::array() {
    ++p;  // '['
    lua_newtable(L);
    skip_ws();
    if (p < end && *p == ']') { ++p; return true; }
    for (int i = 1;; ++i) {
        if (!value()) return false;
        lua_rawseti(L, -2, i);
        skip_ws();
        if (p >= end) return fail("unterminated array");
        if (*p == ',') { ++p; skip_ws(); continue; }
        if (*p == ']') { ++p; return true; }
        return fail("expected ',' or ']'");
    }
}

bool JsonReader::object() {
    ++p;  // '{'
    lua_newtable(L);
    skip_ws();
    if (p < end && *p == '}') { ++p; return true; }
    while (true) {
        skip_ws();
        if (!string_lit()) return false;
        skip_ws();
        if (p >= end || *p != ':') return fail("expected ':'");
        ++p;
        skip_ws();
        if (!value()) return false;
        lua_rawset(L, -3);
        skip_ws();
        if (p >= end) return fail("unterminated object");
        if (*p == ',') { ++p; continue; }
        if (*p == '}') { ++p; return true; }
        return fail("expected ',' or '}'");
    }
}

bool JsonReader::value() {
    skip_ws();
    if (p >= end) return fail("unexpected end of input");
    if (depth >= kMaxDepth) return fail("nesting too deep");
    if (!lua_checkstack(L, 4)) return fail("out of stack");

    char c = *p;
    if (c == '{' || c == '[') {
        ++depth;
        bool ok = (c == '{') ? object() : array();
        --depth;
        return ok;
    }
    if (c == '"') return string_lit();
    if (end - p >= 4 && std::memcmp(p, "true", 4) == 0) {
        p += 4; lua_pushboolean(L, 1); return true;
    }
    if (end - p >= 5 && std::memcmp(p, "false", 5) == 0) {
        p += 5; lua_pushboolean(L, 0); return true;
    }
    if (end - p >= 4 && std::memcmp(p, "null", 4) == 0) {
        p += 4; lua_pushlightuserdata(L, nullptr); return true;
    }
    return number();
}

// Returns the decoded value, or nil + message.
int l_json_decode(lua_State* L) {
    size_t len = 0;
    const char* s = luaL_checklstring(L, 1, &len);
    JsonReader r{s, s + len, L};
    int base = lua_gettop(L);
    if (!r.value()) {
        lua_settop(L, base);
        lua_pushnil(L);
        lua_pushstring(L, r.err.empty() ? "invalid JSON" : r.err.c_str());
        return 2;
    }
    r.skip_ws();
    if (r.p != r.end) {
        lua_settop(L, base);
        lua_pushnil(L);
        lua_pushstring(L, "trailing data after JSON value");
        return 2;
    }
    return 1;
}

// ---------------------------------------------------------------- JSON encode

void encode_string(std::string& out, const char* s, size_t len) {
    out += '"';
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (c < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    out += '"';
}

// A table counts as an array when its keys are exactly 1..n.
bool is_array(lua_State* L, int idx, lua_Integer& n) {
    n = 0;
    lua_Integer count = 0;
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        if (lua_type(L, -2) != LUA_TNUMBER || !lua_isinteger(L, -2)) {
            lua_pop(L, 2);
            return false;
        }
        lua_Integer k = lua_tointeger(L, -2);
        if (k < 1) { lua_pop(L, 2); return false; }
        if (k > n) n = k;
        ++count;
        lua_pop(L, 1);
    }
    return n == count;
}

bool encode_value(lua_State* L, int idx, std::string& out, int depth, std::string& err) {
    if (depth >= kMaxDepth) { err = "nesting too deep"; return false; }
    if (!lua_checkstack(L, 4)) { err = "out of stack"; return false; }

    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
        case LUA_TNIL:
            out += "null";
            return true;
        case LUA_TBOOLEAN:
            out += lua_toboolean(L, idx) ? "true" : "false";
            return true;
        case LUA_TLIGHTUSERDATA:
            if (lua_touserdata(L, idx) == nullptr) { out += "null"; return true; }
            err = "cannot encode userdata";
            return false;
        case LUA_TNUMBER: {
            double d = lua_tonumber(L, idx);
            if (!std::isfinite(d)) { err = "cannot encode inf/nan"; return false; }
            char buf[40];
            if (lua_isinteger(L, idx)) {
                std::snprintf(buf, sizeof(buf), "%lld", (long long)lua_tointeger(L, idx));
            } else {
                std::snprintf(buf, sizeof(buf), "%.17g", d);
            }
            out += buf;
            return true;
        }
        case LUA_TSTRING: {
            size_t len = 0;
            const char* s = lua_tolstring(L, idx, &len);
            encode_string(out, s, len);
            return true;
        }
        case LUA_TTABLE: {
            lua_Integer n = 0;
            if (is_array(L, idx, n)) {
                out += '[';
                for (lua_Integer i = 1; i <= n; ++i) {
                    if (i > 1) out += ',';
                    lua_rawgeti(L, idx, i);
                    bool ok = encode_value(L, -1, out, depth + 1, err);
                    lua_pop(L, 1);
                    if (!ok) return false;
                }
                out += ']';
                return true;
            }
            out += '{';
            bool first = true;
            lua_pushnil(L);
            while (lua_next(L, idx)) {
                if (lua_type(L, -2) == LUA_TSTRING) {
                    if (!first) out += ',';
                    first = false;
                    size_t klen = 0;
                    const char* k = lua_tolstring(L, -2, &klen);
                    encode_string(out, k, klen);
                    out += ':';
                    if (!encode_value(L, -1, out, depth + 1, err)) {
                        lua_pop(L, 2);
                        return false;
                    }
                } else if (lua_type(L, -2) == LUA_TNUMBER) {
                    if (!first) out += ',';
                    first = false;
                    lua_pushvalue(L, -2);
                    size_t klen = 0;
                    const char* k = lua_tolstring(L, -1, &klen);
                    encode_string(out, k, klen);
                    lua_pop(L, 1);
                    out += ':';
                    if (!encode_value(L, -1, out, depth + 1, err)) {
                        lua_pop(L, 2);
                        return false;
                    }
                }
                lua_pop(L, 1);
            }
            out += '}';
            return true;
        }
        default:
            err = "cannot encode this value";
            return false;
    }
}

int l_json_encode(lua_State* L) {
    luaL_checkany(L, 1);
    std::string out, err;
    if (!encode_value(L, 1, out, 0, err)) {
        lua_pushnil(L);
        lua_pushstring(L, err.c_str());
        return 2;
    }
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

// ---------------------------------------------------------------- fetch gates

bool has_ctl(const std::string& s) {
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7F) return true;
    return false;
}

// RFC 7230 token characters; anything else could smuggle a second header.
bool valid_header_name(const std::string& s) {
    if (s.empty() || s.size() > kMaxHeaderLen) return false;
    for (unsigned char c : s) {
        if (std::isalnum(c)) continue;
        if (std::strchr("!#$%&'*+-.^_`|~", c) == nullptr) return false;
    }
    return true;
}

bool forbidden_header(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    static const char* kBlocked[] = {
        "host", "origin", "connection", "content-length", "user-agent", "referer", nullptr,
    };
    for (const char** b = kBlocked; *b; ++b)
        if (name == *b) return true;
    return false;
}

bool allowed_method(const std::string& m) {
    static const char* kOk[] = {"GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", nullptr};
    for (const char** k = kOk; *k; ++k)
        if (m == *k) return true;
    return false;
}

std::string origin_of(const ParsedURL& u) {
    int def = u.scheme == "star" ? 8490 : 8090;
    std::string s = u.scheme + "://" + format_host(u.host);
    if (u.port != def) s += ":" + std::to_string(u.port);
    return s;
}

int fail_call(lua_State* L, const char* msg) {
    return luaL_error(L, "%s", msg);
}

struct PendingFetch {
    std::string url;
    std::string origin;
    bool        needs_cors = false;
    RequestOptions opt;
};

void run_worker(std::shared_ptr<FetchInbox> inbox, PendingFetch pf, int ref) {
    std::thread([inbox = std::move(inbox), pf = std::move(pf), ref]() {
        FetchResult res = perform_request(pf.url, pf.opt);

        FetchDone d;
        d.ref = ref;
        if (!res.success) {
            d.error = res.error_message.empty() ? "request failed" : res.error_message;
        } else if (pf.needs_cors) {
            auto it = res.headers.find("access-control-allow-origin");
            if (it == res.headers.end() || (it->second != "*" && it->second != pf.origin)) {
                // The body never reaches the page: without this a script could read
                // any host the user can route to, including their own localhost.
                d.error = "blocked by cross-origin policy: " + pf.url;
            }
        }
        if (d.error.empty() && res.success) {
            d.ok = true;
            d.status = res.status_code;
            d.status_text = res.status_text;
            d.headers = std::move(res.headers);
            d.body = std::move(res.body);
            d.secure = res.is_secure;
        }

        std::function<void()> wake;
        {
            std::lock_guard<std::mutex> lk(inbox->m);
            --inbox->inflight;
            if (inbox->cancelled) return;
            inbox->done.push_back(std::move(d));
            wake = inbox->wake;
        }
        if (wake) wake();
    }).detach();
}

int l_res_json(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "body");
    if (!lua_isstring(L, -1)) {
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_pushstring(L, "response has no body");
        return 2;
    }
    lua_replace(L, 1);
    lua_settop(L, 1);
    return l_json_decode(L);
}

int l_fetch(lua_State* L) {
    ScriptEngine* eng = engine_from_lua(L);
    if (!eng) return fail_call(L, "fetch unavailable");

    size_t url_len = 0;
    const char* raw_url = luaL_checklstring(L, 1, &url_len);
    if (url_len > kMaxUrlLen) return fail_call(L, "fetch: URL too long");

    int opts_idx = 0, cb_idx = 0;
    if (lua_isfunction(L, 2)) {
        cb_idx = 2;
    } else if (lua_istable(L, 2) && lua_isfunction(L, 3)) {
        opts_idx = 2;
        cb_idx = 3;
    } else {
        return fail_call(L, "fetch(url[, options], callback)");
    }

    std::string url = resolve_url(eng->current_url(), std::string(raw_url, url_len));
    if (has_ctl(url)) return fail_call(L, "fetch: illegal characters in URL");

    auto target = parse_url(url);
    auto page = parse_url(eng->current_url());
    if (!target) return fail_call(L, "fetch: only moon:// and star:// URLs are allowed");
    if (!page) return fail_call(L, "fetch: page has no origin");

    if (page->scheme == "star" && target->scheme == "moon")
        return fail_call(L, "fetch: a star:// page cannot request moon:// URLs");

    PendingFetch pf;
    pf.url = url;
    pf.origin = origin_of(*page);
    pf.needs_cors = origin_of(*target) != pf.origin;

    pf.opt.max_response_bytes = kMaxResponse;
    pf.opt.headers.emplace_back("Origin", pf.origin);

    if (opts_idx) {
        lua_getfield(L, opts_idx, "method");
        if (lua_isstring(L, -1)) {
            std::string m = lua_tostring(L, -1);
            std::transform(m.begin(), m.end(), m.begin(),
                           [](unsigned char c) { return std::toupper(c); });
            if (!allowed_method(m)) return fail_call(L, "fetch: method not allowed");
            pf.opt.method = m;
        }
        lua_pop(L, 1);

        lua_getfield(L, opts_idx, "timeout");
        if (lua_isnumber(L, -1)) {
            double t = lua_tonumber(L, -1);
            pf.opt.timeout_secs = (int)std::min(30.0, std::max(1.0, t));
        }
        lua_pop(L, 1);

        lua_getfield(L, opts_idx, "json");
        if (!lua_isnil(L, -1)) {
            std::string body, err;
            if (!encode_value(L, -1, body, 0, err))
                return luaL_error(L, "fetch: json option: %s", err.c_str());
            if (body.size() > kMaxRequestBody) return fail_call(L, "fetch: body too large");
            pf.opt.body = std::move(body);
            pf.opt.headers.emplace_back("Content-Type", "application/json");
        }
        lua_pop(L, 1);

        lua_getfield(L, opts_idx, "body");
        if (lua_isstring(L, -1)) {
            if (!pf.opt.body.empty()) return fail_call(L, "fetch: pass body or json, not both");
            size_t blen = 0;
            const char* b = lua_tolstring(L, -1, &blen);
            if (blen > kMaxRequestBody) return fail_call(L, "fetch: body too large");
            pf.opt.body.assign(b, blen);
        }
        lua_pop(L, 1);

        lua_getfield(L, opts_idx, "headers");
        if (lua_istable(L, -1)) {
            int htab = lua_gettop(L);
            lua_pushnil(L);
            while (lua_next(L, htab)) {
                if (lua_type(L, -2) == LUA_TSTRING && lua_isstring(L, -1)) {
                    std::string name = lua_tostring(L, -2);
                    std::string value = lua_tostring(L, -1);
                    if (!valid_header_name(name))
                        return luaL_error(L, "fetch: bad header name %s", name.c_str());
                    if (forbidden_header(name))
                        return luaL_error(L, "fetch: header %s cannot be set", name.c_str());
                    if (value.size() > kMaxHeaderLen || has_ctl(value))
                        return luaL_error(L, "fetch: bad header value for %s", name.c_str());
                    if (pf.opt.headers.size() >= kMaxHeaders)
                        return fail_call(L, "fetch: too many headers");
                    pf.opt.headers.emplace_back(std::move(name), std::move(value));
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }

    if (pf.opt.method == "GET" || pf.opt.method == "HEAD") pf.opt.body.clear();

    auto inbox = eng->fetch_inbox();
    if (!inbox) return fail_call(L, "fetch unavailable");

    // The ref comes first because luaL_ref can raise, and raising after the slot is
    // taken would leak it. Nothing below here may raise while the lock is held:
    // lua_error longjmps, so a lock_guard in scope would never be destroyed.
    lua_pushvalue(L, cb_idx);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    bool accepted = false;
    {
        std::lock_guard<std::mutex> lk(inbox->m);
        if (inbox->inflight < kMaxInflight) {
            ++inbox->inflight;
            accepted = true;
        }
    }
    if (!accepted) {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        return fail_call(L, "fetch: too many requests in flight");
    }

    run_worker(inbox, std::move(pf), ref);
    return 0;
}

} // namespace

void install_fetch_api(lua_State* L) {
    lua_pushcfunction(L, &l_fetch);
    lua_setglobal(L, "fetch");

    lua_newtable(L);
    lua_pushcfunction(L, &l_json_encode); lua_setfield(L, -2, "encode");
    lua_pushcfunction(L, &l_json_decode); lua_setfield(L, -2, "decode");
    lua_pushlightuserdata(L, nullptr);    lua_setfield(L, -2, "null");
    lua_setglobal(L, "json");
}

void push_fetch_response(lua_State* L, const FetchDone& d) {
    lua_createtable(L, 0, 6);
    lua_pushinteger(L, d.status);                       lua_setfield(L, -2, "status");
    lua_pushlstring(L, d.status_text.data(), d.status_text.size());
    lua_setfield(L, -2, "statusText");
    lua_pushboolean(L, d.status >= 200 && d.status < 300); lua_setfield(L, -2, "ok");
    lua_pushboolean(L, d.secure);                       lua_setfield(L, -2, "secure");
    lua_pushlstring(L, d.body.data(), d.body.size());   lua_setfield(L, -2, "body");
    lua_pushcfunction(L, &l_res_json);                  lua_setfield(L, -2, "json");

    lua_createtable(L, 0, (int)d.headers.size());
    for (const auto& [name, value] : d.headers) {
        lua_pushlstring(L, value.data(), value.size());
        lua_setfield(L, -2, name.c_str());
    }
    lua_setfield(L, -2, "headers");
}
