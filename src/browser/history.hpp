#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace history {

struct Visit {
    std::string url;
    std::string title;
    std::int64_t at = 0;  // unix seconds
};

constexpr std::size_t kMaxVisits = 512;

// In the order they were visited, oldest first.
const std::vector<Visit>& visits();

// Repeating the newest entry refreshes it in place instead of stacking. An empty
// `favicon_bytes` leaves whatever icon the site already had.
void record(const std::string& url, const std::string& title,
            const std::string& favicon_bytes = {});

// The site's icon as served, or null. Kept per origin.
const std::string* favicon(const std::string& url);

// Rate-limited unless `force`, which the shutdown path passes.
void flush(bool force = false);

}  // namespace history
