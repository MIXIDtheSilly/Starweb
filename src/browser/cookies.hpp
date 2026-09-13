#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct lua_State;

namespace cookies {

constexpr std::size_t kMaxNameBytes        = 256;
constexpr std::size_t kMaxValueBytes       = 4096;
constexpr std::size_t kMaxCookiesPerOrigin = 64;

constexpr char kSetCookieSeparator = '|';

std::string header_for(const std::string& origin);

void apply_set_cookie(const std::string& origin, const std::string& value);

void flush(bool force = false);

struct Entry {
    std::string name;
    std::string value;
    std::int64_t expires_at = 0;  // unix seconds, 0 for a session cookie
    bool stwp_only = false;
};

std::vector<Entry> entries(const std::string& origin);

void remove(const std::string& origin, const std::string& name);
void clear(const std::string& origin);

std::uint64_t revision();

}  // namespace cookies

void install_cookies_api(lua_State* L);
