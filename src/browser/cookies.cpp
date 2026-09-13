#include "cookies.hpp"
#include "script.hpp"
#include "storage.hpp"
#include "globals.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace fs = std::filesystem;

namespace cookies {
namespace {

struct Cookie {
    std::string name;
    std::string value;
    std::int64_t expires_at = 0;
    bool stwp_only = false;
};

using Jar = std::vector<Cookie>;

std::mutex g_mutex;
std::unordered_map<std::string, Jar> g_jars;
bool g_dirty = false;
std::uint64_t g_revision = 0;
bool g_loaded = false;
std::chrono::steady_clock::time_point g_last_flush{};
constexpr std::chrono::milliseconds kFlushInterval{1000};

std::int64_t now_secs() {
    return (std::int64_t)std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void touched_locked() {
    g_dirty = true;
    g_revision++;
}

bool expired(const Cookie& c, std::int64_t now) {
    return c.expires_at != 0 && c.expires_at <= now;
}

fs::path store_path() { return app_dir() / "storage" / "cookies.store"; }

constexpr const char* kMagic = "STARCOOKIES1";

void load_locked() {
    std::ifstream in(store_path(), std::ios::binary);
    if (!in) return;

    std::string magic;
    if (!std::getline(in, magic) || magic != kMagic) return;

    const std::int64_t now = now_secs();
    for (;;) {
        std::string header;
        if (!std::getline(in, header)) break;
        unsigned long long olen = 0, nlen = 0, vlen = 0;
        long long expires = 0;
        int stwp_only = 0;
        if (std::sscanf(header.c_str(), "%llu %llu %llu %lld %d",
                        &olen, &nlen, &vlen, &expires, &stwp_only) != 5) break;
        if (nlen > kMaxNameBytes || vlen > kMaxValueBytes || olen > 1024) break;

        std::string origin((std::size_t)olen, '\0');
        std::string name((std::size_t)nlen, '\0');
        std::string value((std::size_t)vlen, '\0');
        if (olen && !in.read(&origin[0], (std::streamsize)olen)) break;
        if (nlen && !in.read(&name[0], (std::streamsize)nlen)) break;
        if (vlen && !in.read(&value[0], (std::streamsize)vlen)) break;
        in.get();

        Cookie c{std::move(name), std::move(value), (std::int64_t)expires, stwp_only != 0};
        if (expired(c, now)) continue;
        Jar& jar = g_jars[origin];
        if (jar.size() >= kMaxCookiesPerOrigin) continue;
        jar.push_back(std::move(c));
    }
}

void ensure_loaded_locked() {
    if (g_loaded) return;
    g_loaded = true;
    load_locked();
}

void save_locked() {
    std::error_code ec;
    fs::create_directories(store_path().parent_path(), ec);

    fs::path final_path = store_path();
    fs::path tmp = final_path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << kMagic << "\n";
        const std::int64_t now = now_secs();
        for (const auto& [origin, jar] : g_jars) {
            for (const Cookie& c : jar) {
                if (c.expires_at == 0 || expired(c, now)) continue;
                out << origin.size() << " " << c.name.size() << " " << c.value.size()
                    << " " << c.expires_at << " " << (c.stwp_only ? 1 : 0) << "\n";
                out.write(origin.data(), (std::streamsize)origin.size());
                out.write(c.name.data(), (std::streamsize)c.name.size());
                out.write(c.value.data(), (std::streamsize)c.value.size());
                out << "\n";
            }
        }
        out.flush();
        if (!out) { fs::remove(tmp, ec); return; }
    }
    fs::rename(tmp, final_path, ec);
    if (ec) fs::remove(tmp, ec);
}

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && (unsigned char)s[b] <= ' ') ++b;
    while (e > b && (unsigned char)s[e - 1] <= ' ') --e;
    return s.substr(b, e - b);
}

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool valid_name(const std::string& n) {
    if (n.empty() || n.size() > kMaxNameBytes) return false;
    for (unsigned char c : n) {
        if (c <= ' ' || c == 0x7f) return false;
        if (c == ';' || c == '=' || c == ',' || (char)c == kSetCookieSeparator) return false;
    }
    return true;
}

bool valid_value(const std::string& v) {
    if (v.size() > kMaxValueBytes) return false;
    for (unsigned char c : v) {
        if (c < ' ' || c == 0x7f) return false;
        if (c == ';' || (char)c == kSetCookieSeparator) return false;
    }
    return true;
}

Jar::iterator find_in(Jar& jar, const std::string& name) {
    return std::find_if(jar.begin(), jar.end(),
                        [&](const Cookie& c) { return c.name == name; });
}

void drop_expired_locked(Jar& jar) {
    const std::int64_t now = now_secs();
    jar.erase(std::remove_if(jar.begin(), jar.end(),
                             [&](const Cookie& c) { return expired(c, now); }),
              jar.end());
}

void store_locked(const std::string& origin, Cookie c, bool had_max_age,
                  std::int64_t max_age) {
    Jar& jar = g_jars[origin];
    auto it = find_in(jar, c.name);

    if (had_max_age && max_age <= 0) {
        if (it != jar.end()) { jar.erase(it); touched_locked(); }
        return;
    }
    c.expires_at = had_max_age ? now_secs() + max_age : 0;

    if (it != jar.end()) {
        *it = std::move(c);
    } else {
        if (jar.size() >= kMaxCookiesPerOrigin) return;
        jar.push_back(std::move(c));
    }
    touched_locked();
}

void parse_one_locked(const std::string& origin, const std::string& piece) {
    std::string text = trim(piece);
    if (text.empty()) return;

    std::size_t semi = text.find(';');
    std::string pair = trim(text.substr(0, semi));
    std::size_t eq = pair.find('=');
    if (eq == std::string::npos) return;

    Cookie c;
    c.name = trim(pair.substr(0, eq));
    c.value = trim(pair.substr(eq + 1));
    if (!valid_name(c.name) || !valid_value(c.value)) return;

    bool had_max_age = false;
    std::int64_t max_age = 0;

    while (semi != std::string::npos) {
        std::size_t start = semi + 1;
        semi = text.find(';', start);
        std::string attr = trim(text.substr(start, semi == std::string::npos
                                                       ? std::string::npos
                                                       : semi - start));
        if (attr.empty()) continue;
        std::size_t aeq = attr.find('=');
        std::string key = lower(trim(attr.substr(0, aeq)));
        std::string val = aeq == std::string::npos ? "" : trim(attr.substr(aeq + 1));

        if (key == "stwponly") {
            c.stwp_only = true;
        } else if (key == "max-age") {
            try {
                max_age = std::stoll(val);
                had_max_age = true;
            } catch (...) {
                return;
            }
        }
    }

    store_locked(origin, std::move(c), had_max_age, max_age);
}

}  // namespace

std::string header_for(const std::string& origin) {
    if (origin.empty()) return {};
    std::lock_guard<std::mutex> lk(g_mutex);
    ensure_loaded_locked();

    auto it = g_jars.find(origin);
    if (it == g_jars.end()) return {};
    drop_expired_locked(it->second);

    std::string out;
    for (const Cookie& c : it->second) {
        if (!out.empty()) out += "; ";
        out += c.name;
        out += "=";
        out += c.value;
    }
    return out;
}

void apply_set_cookie(const std::string& origin, const std::string& value) {
    if (origin.empty() || value.empty()) return;
    std::lock_guard<std::mutex> lk(g_mutex);
    ensure_loaded_locked();

    std::size_t start = 0;
    for (;;) {
        std::size_t sep = value.find(kSetCookieSeparator, start);
        parse_one_locked(origin, value.substr(start, sep == std::string::npos
                                                         ? std::string::npos
                                                         : sep - start));
        if (sep == std::string::npos) break;
        start = sep + 1;
    }
}

std::vector<Entry> entries(const std::string& origin) {
    if (origin.empty()) return {};
    std::lock_guard<std::mutex> lk(g_mutex);
    ensure_loaded_locked();

    auto it = g_jars.find(origin);
    if (it == g_jars.end()) return {};
    drop_expired_locked(it->second);

    std::vector<Entry> out;
    out.reserve(it->second.size());
    for (const Cookie& c : it->second) {
        out.push_back(Entry{c.name, c.value, c.expires_at, c.stwp_only});
    }
    return out;
}

void remove(const std::string& origin, const std::string& name) {
    std::lock_guard<std::mutex> lk(g_mutex);
    ensure_loaded_locked();
    auto jt = g_jars.find(origin);
    if (jt == g_jars.end()) return;
    auto it = find_in(jt->second, name);
    if (it == jt->second.end()) return;
    jt->second.erase(it);
    touched_locked();
}

void clear(const std::string& origin) {
    std::lock_guard<std::mutex> lk(g_mutex);
    ensure_loaded_locked();
    auto jt = g_jars.find(origin);
    if (jt == g_jars.end() || jt->second.empty()) return;
    jt->second.clear();
    touched_locked();
}

std::uint64_t revision() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_revision;
}

void flush(bool force) {
    std::lock_guard<std::mutex> lk(g_mutex);
    auto now = std::chrono::steady_clock::now();
    if (!force && now - g_last_flush < kFlushInterval) return;
    if (g_dirty) {
        save_locked();
        g_dirty = false;
    }
    g_last_flush = now;
}

namespace {

const char* kCookiesMT = "StarCookies";

std::string origin_of(lua_State* L) {
    ScriptEngine* eng = engine_from_lua(L);
    return eng ? storage::origin_for_url(eng->current_url()) : std::string();
}

std::string checked_origin(lua_State* L) {
    std::string origin = origin_of(L);
    if (origin.empty()) luaL_error(L, "cookies are unavailable for this document");
    return origin;
}

std::string to_str(lua_State* L, int idx) {
    std::size_t len = 0;
    const char* s = luaL_tolstring(L, idx, &len);
    std::string out(s, len);
    lua_pop(L, 1);
    return out;
}

// Dot calls pass no self, colon calls do; both reach here.
int arg_base(lua_State* L) {
    return luaL_testudata(L, 1, kCookiesMT) ? 1 : 0;
}

std::vector<std::pair<std::string, std::string>> visible(lua_State* L) {
    std::string origin = checked_origin(L);
    std::lock_guard<std::mutex> lk(g_mutex);
    ensure_loaded_locked();

    std::vector<std::pair<std::string, std::string>> out;
    auto it = g_jars.find(origin);
    if (it == g_jars.end()) return out;
    drop_expired_locked(it->second);
    for (const Cookie& c : it->second) {
        if (!c.stwp_only) out.emplace_back(c.name, c.value);
    }
    return out;
}

int ck_getItem(lua_State* L) {
    std::string key = to_str(L, arg_base(L) + 1);
    for (const auto& [n, v] : visible(L)) {
        if (n == key) { lua_pushlstring(L, v.data(), v.size()); return 1; }
    }
    lua_pushnil(L);
    return 1;
}

int ck_setItem(lua_State* L) {
    std::string origin = checked_origin(L);
    int base = arg_base(L);
    std::string name = to_str(L, base + 1);
    std::string value = to_str(L, base + 2);

    if (!valid_name(name)) {
        return luaL_error(L, "invalid cookie name");
    }
    if (!valid_value(value)) {
        return luaL_error(L, "invalid cookie value, or it exceeds %d bytes",
                          (int)kMaxValueBytes);
    }

    bool had_max_age = false;
    std::int64_t max_age = 0;
    if (lua_istable(L, base + 3)) {
        lua_getfield(L, base + 3, "maxAge");
        if (!lua_isnoneornil(L, -1)) {
            max_age = (std::int64_t)luaL_checkinteger(L, -1);
            had_max_age = true;
        }
        lua_pop(L, 1);
    }

    // luaL_error longjmps past the lock guard, so errors are raised after unlocking.
    bool is_stwp_only = false, full = false;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        ensure_loaded_locked();
        Jar& jar = g_jars[origin];
        auto it = find_in(jar, name);
        if (it != jar.end() && it->stwp_only) {
            is_stwp_only = true;
        } else if (it == jar.end() && jar.size() >= kMaxCookiesPerOrigin) {
            full = true;
        } else {
            store_locked(origin, Cookie{name, std::move(value), 0, false},
                         had_max_age, max_age);
        }
    }
    if (is_stwp_only) return luaL_error(L, "cookie '%s' is StwpOnly", name.c_str());
    if (full) {
        return luaL_error(L, "this origin holds the maximum of %d cookies",
                          (int)kMaxCookiesPerOrigin);
    }
    return 0;
}

int ck_removeItem(lua_State* L) {
    std::string origin = checked_origin(L);
    std::string name = to_str(L, arg_base(L) + 1);

    bool is_stwp_only = false;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        ensure_loaded_locked();
        auto jt = g_jars.find(origin);
        if (jt == g_jars.end()) return 0;
        auto it = find_in(jt->second, name);
        if (it == jt->second.end()) return 0;
        if (it->stwp_only) {
            is_stwp_only = true;
        } else {
            jt->second.erase(it);
            touched_locked();
        }
    }
    if (is_stwp_only) return luaL_error(L, "cookie '%s' is StwpOnly", name.c_str());
    return 0;
}

int ck_clear(lua_State* L) {
    std::string origin = checked_origin(L);
    std::lock_guard<std::mutex> lk(g_mutex);
    ensure_loaded_locked();
    auto jt = g_jars.find(origin);
    if (jt == g_jars.end()) return 0;
    Jar& jar = jt->second;
    std::size_t before = jar.size();
    jar.erase(std::remove_if(jar.begin(), jar.end(),
                             [](const Cookie& c) { return !c.stwp_only; }),
              jar.end());
    if (jar.size() != before) touched_locked();
    return 0;
}

int ck_key(lua_State* L) {
    lua_Integer i = luaL_checkinteger(L, arg_base(L) + 1);
    auto items = visible(L);
    if (i < 1 || (std::size_t)i > items.size()) {
        lua_pushnil(L);
    } else {
        const std::string& k = items[(std::size_t)i - 1].first;
        lua_pushlstring(L, k.data(), k.size());
    }
    return 1;
}

const luaL_Reg kMethods[] = {
    {"getItem",    &ck_getItem},
    {"setItem",    &ck_setItem},
    {"removeItem", &ck_removeItem},
    {"clear",      &ck_clear},
    {"key",        &ck_key},
    {nullptr, nullptr},
};

int cookies_index(lua_State* L) {
    const char* key = luaL_checkstring(L, 2);
    for (const luaL_Reg* m = kMethods; m->name; ++m) {
        if (std::strcmp(m->name, key) == 0) {
            lua_pushcfunction(L, m->func);
            return 1;
        }
    }
    if (std::strcmp(key, "length") == 0) {
        lua_pushinteger(L, (lua_Integer)visible(L).size());
        return 1;
    }
    return ck_getItem(L);
}

int cookies_newindex(lua_State* L) {
    if (lua_isnoneornil(L, 3)) return ck_removeItem(L);
    return ck_setItem(L);
}

int cookies_len(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)visible(L).size());
    return 1;
}

}  // namespace
}  // namespace cookies

void install_cookies_api(lua_State* L) {
    luaL_newmetatable(L, cookies::kCookiesMT);
    lua_pushcfunction(L, &cookies::cookies_index);    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, &cookies::cookies_newindex); lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, &cookies::cookies_len);      lua_setfield(L, -2, "__len");
    lua_pushboolean(L, 0);                            lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);

    lua_newuserdatauv(L, 1, 0);
    luaL_setmetatable(L, cookies::kCookiesMT);
    lua_setglobal(L, "cookies");
}
