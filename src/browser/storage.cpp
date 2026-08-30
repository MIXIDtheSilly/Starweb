#include "storage.hpp"
#include "script.hpp"
#include "globals.hpp"
#include "../common/url_parser.hpp"

#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace fs = std::filesystem;

namespace storage {
namespace {

// Insertion-ordered, so `key(i)` answers the same across a page's lifetime.
struct Area {
    std::vector<std::pair<std::string, std::string>> items;
    std::size_t bytes = 0;
    bool dirty = false;

    std::pair<std::string, std::string>* find(const std::string& k) {
        for (auto& kv : items) {
            if (kv.first == k) return &kv;
        }
        return nullptr;
    }
};

std::unordered_map<std::string, Area> g_areas;
std::uint64_t g_revision = 0;
std::chrono::steady_clock::time_point g_last_flush{};
constexpr std::chrono::milliseconds kFlushInterval{1000};

std::string file_name_for(const std::string& origin) {
    std::string safe;
    safe.reserve(origin.size());
    for (unsigned char c : origin) {
        safe += (std::isalnum(c) || c == '.' || c == '-') ? (char)c : '_';
    }
    if (safe.size() > 64) safe.resize(64);
    char suffix[24];
    std::snprintf(suffix, sizeof(suffix), "_%016llx",
                  (unsigned long long)std::hash<std::string>{}(origin));
    return safe + suffix + ".store";
}

fs::path store_dir() { return app_dir() / "storage"; }
fs::path path_for(const std::string& origin) { return store_dir() / file_name_for(origin); }

// Length-prefixed, since keys and values may hold newlines or raw bytes.
constexpr const char* kMagic = "STARSTORE1";

void load(const std::string& origin, Area& a) {
    std::ifstream in(path_for(origin), std::ios::binary);
    if (!in) return;

    std::string magic;
    if (!std::getline(in, magic) || magic != kMagic) return;

    for (;;) {
        std::string header;
        if (!std::getline(in, header)) break;
        unsigned long long klen = 0, vlen = 0;
        if (std::sscanf(header.c_str(), "%llu %llu", &klen, &vlen) != 2) break;
        if (klen > kMaxKeyBytes || vlen > kMaxValueBytes) break;
        if (a.items.size() >= kMaxKeys) break;
        if (a.bytes + klen + vlen > kMaxOriginBytes) break;

        std::string key((std::size_t)klen, '\0'), val((std::size_t)vlen, '\0');
        if (klen && !in.read(&key[0], (std::streamsize)klen)) break;
        if (vlen && !in.read(&val[0], (std::streamsize)vlen)) break;
        in.get();  // the newline closing the record

        a.bytes += (std::size_t)(klen + vlen);
        a.items.emplace_back(std::move(key), std::move(val));
    }
}

// Temp file plus rename, so a crash mid-write leaves the previous contents.
void save(const std::string& origin, const Area& a) {
    std::error_code ec;
    fs::create_directories(store_dir(), ec);

    fs::path final_path = path_for(origin);
    fs::path tmp = final_path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << kMagic << "\n";
        for (const auto& [k, v] : a.items) {
            out << k.size() << " " << v.size() << "\n";
            out.write(k.data(), (std::streamsize)k.size());
            out.write(v.data(), (std::streamsize)v.size());
            out << "\n";
        }
        out.flush();
        if (!out) { fs::remove(tmp, ec); return; }
    }
    fs::rename(tmp, final_path, ec);
    if (ec) fs::remove(tmp, ec);
}

const char* kStorageMT = "StarStorage";

Area& area_by_origin(const std::string& origin) {
    auto it = g_areas.find(origin);
    if (it == g_areas.end()) {
        it = g_areas.emplace(origin, Area{}).first;
        load(origin, it->second);
    }
    return it->second;
}

std::string origin_of(lua_State* L) {
    ScriptEngine* eng = engine_from_lua(L);
    return eng ? origin_for_url(eng->current_url()) : std::string();
}

Area* area_for(lua_State* L) {
    std::string origin = origin_of(L);
    return origin.empty() ? nullptr : &area_by_origin(origin);
}

Area* checked_area(lua_State* L) {
    Area* a = area_for(L);
    if (!a) luaL_error(L, "localStorage is unavailable for this document");
    return a;
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
    return luaL_testudata(L, 1, kStorageMT) ? 1 : 0;
}

int st_getItem(lua_State* L) {
    Area* a = checked_area(L);
    std::string key = to_str(L, arg_base(L) + 1);
    if (auto* kv = a->find(key)) {
        lua_pushlstring(L, kv->second.data(), kv->second.size());
    } else {
        lua_pushnil(L);
    }
    return 1;
}

int st_setItem(lua_State* L) {
    Area* a = checked_area(L);
    int base = arg_base(L);
    std::string key = to_str(L, base + 1);
    std::string val = to_str(L, base + 2);

    if (key.size() > kMaxKeyBytes) {
        return luaL_error(L, "localStorage key exceeds %d bytes", (int)kMaxKeyBytes);
    }
    if (val.size() > kMaxValueBytes) {
        return luaL_error(L, "localStorage value exceeds %d bytes", (int)kMaxValueBytes);
    }

    auto* kv = a->find(key);
    std::size_t was = kv ? kv->first.size() + kv->second.size() : 0;
    std::size_t now = key.size() + val.size();
    if (a->bytes - was + now > kMaxOriginBytes) {
        return luaL_error(L, "localStorage quota exceeded for this origin");
    }
    if (!kv && a->items.size() >= kMaxKeys) {
        return luaL_error(L, "localStorage holds the maximum of %d keys", (int)kMaxKeys);
    }

    if (kv) {
        kv->second = std::move(val);
    } else {
        a->items.emplace_back(std::move(key), std::move(val));
    }
    a->bytes = a->bytes - was + now;
    a->dirty = true;
    g_revision++;
    return 0;
}

int st_removeItem(lua_State* L) {
    checked_area(L);  // raises when the document has no origin
    storage::remove(origin_of(L), to_str(L, arg_base(L) + 1));
    return 0;
}

int st_clear(lua_State* L) {
    checked_area(L);
    storage::clear(origin_of(L));
    return 0;
}

// 1-based like every other sequence this engine hands to Lua, not web 0-based.
int st_key(lua_State* L) {
    Area* a = checked_area(L);
    lua_Integer i = luaL_checkinteger(L, arg_base(L) + 1);
    if (i < 1 || (std::size_t)i > a->items.size()) {
        lua_pushnil(L);
    } else {
        const std::string& k = a->items[(std::size_t)i - 1].first;
        lua_pushlstring(L, k.data(), k.size());
    }
    return 1;
}

const luaL_Reg kMethods[] = {
    {"getItem",    &st_getItem},
    {"setItem",    &st_setItem},
    {"removeItem", &st_removeItem},
    {"clear",      &st_clear},
    {"key",        &st_key},
    {nullptr, nullptr},
};

// A method name wins over a stored key, then `length`, then the store itself.
int storage_index(lua_State* L) {
    const char* key = luaL_checkstring(L, 2);
    for (const luaL_Reg* m = kMethods; m->name; ++m) {
        if (std::strcmp(m->name, key) == 0) {
            lua_pushcfunction(L, m->func);
            return 1;
        }
    }
    if (std::strcmp(key, "length") == 0) {
        Area* a = checked_area(L);
        lua_pushinteger(L, (lua_Integer)a->items.size());
        return 1;
    }
    return st_getItem(L);
}

int storage_newindex(lua_State* L) {
    if (lua_isnoneornil(L, 3)) return st_removeItem(L);
    return st_setItem(L);
}

int storage_len(lua_State* L) {
    Area* a = checked_area(L);
    lua_pushinteger(L, (lua_Integer)a->items.size());
    return 1;
}

}  // namespace

std::string origin_for_url(const std::string& url) {
    auto p = parse_url(url);
    if (!p) return {};
    return p->scheme + "://" + format_host(p->host) + ":" + std::to_string(p->port);
}

const Entries* entries(const std::string& origin) {
    if (origin.empty()) return nullptr;
    return &area_by_origin(origin).items;
}

void remove(const std::string& origin, const std::string& key) {
    if (origin.empty()) return;
    Area& a = area_by_origin(origin);
    for (auto it = a.items.begin(); it != a.items.end(); ++it) {
        if (it->first != key) continue;
        a.bytes -= it->first.size() + it->second.size();
        a.items.erase(it);
        a.dirty = true;
        g_revision++;
        break;
    }
}

void clear(const std::string& origin) {
    if (origin.empty()) return;
    Area& a = area_by_origin(origin);
    if (a.items.empty()) return;
    a.items.clear();
    a.bytes = 0;
    a.dirty = true;
    g_revision++;
}

std::uint64_t revision() { return g_revision; }

void flush(bool force) {
    auto now = std::chrono::steady_clock::now();
    if (!force && now - g_last_flush < kFlushInterval) return;
    for (auto& [origin, area] : g_areas) {
        if (!area.dirty) continue;
        save(origin, area);
        area.dirty = false;
    }
    g_last_flush = now;
}

}  // namespace storage

void install_storage_api(lua_State* L) {
    luaL_newmetatable(L, storage::kStorageMT);
    lua_pushcfunction(L, &storage::storage_index);    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, &storage::storage_newindex); lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, &storage::storage_len);      lua_setfield(L, -2, "__len");
    lua_pushboolean(L, 0);                            lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);

    lua_newuserdatauv(L, 1, 0);
    luaL_setmetatable(L, storage::kStorageMT);
    lua_setglobal(L, "localStorage");
}
