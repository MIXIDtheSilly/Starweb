#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct lua_State;

namespace storage {

constexpr std::size_t kMaxKeyBytes    = 1024;
constexpr std::size_t kMaxValueBytes  = 1u * 1024u * 1024u;
constexpr std::size_t kMaxOriginBytes = 5u * 1024u * 1024u;
constexpr std::size_t kMaxKeys        = 4096;

// Rate-limited unless `force`, which the shutdown path passes.
void flush(bool force = false);

using Entries = std::vector<std::pair<std::string, std::string>>;

std::string origin_for_url(const std::string& url);

// Null only for an empty origin; the pointer holds until the next write.
const Entries* entries(const std::string& origin);

void remove(const std::string& origin, const std::string& key);
void clear(const std::string& origin);

// Bumped on every write, so a cache can notice an in-place overwrite.
std::uint64_t revision();

}  // namespace storage

void install_storage_api(lua_State* L);
