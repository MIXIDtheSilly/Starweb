#include "globals.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

std::vector<Tab> tabs;
int active_tab_idx = 0;
int next_tab_id = 1;
bool is_window_maximized = false;
int restored_x = 100;
int restored_y = 100;
int restored_w = 1024;
int restored_h = 768;
std::mutex fetch_mutex;
ImFont* mono_font = nullptr;

Tab* find_tab_by_id(int tab_id) {
    for (auto& tab : tabs) {
        if (tab.id == tab_id) {
            return &tab;
        }
    }
    return nullptr;
}

#include <sstream>
std::string get_cache_filepath(const std::string& url) {
    size_t hash = std::hash<std::string>{}(url);
    std::stringstream ss;
    ss << "cache/media_" << std::hex << hash;
    auto dot = url.find_last_of('.');
    if (dot != std::string::npos) {
        std::string ext = url.substr(dot);
        auto qm = ext.find_first_of("?#");
        if (qm != std::string::npos) {
            ext = ext.substr(0, qm);
        }
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
        if (ext == ".mp4" || ext == ".mov" || ext == ".m4v" || ext == ".mp3" || ext == ".wav" || ext == ".aac" || ext == ".m4a") {
            ss << ext;
        } else {
            ss << ".tmp";
        }
    } else {
        ss << ".tmp";
    }
    return ss.str();
}

// Nothing else deletes from cache/, so without this it grows for the life of the
// install. Called once at startup, before any VideoPlayer opens a file: pruning
// mid-session could pull a file out from under a player still reading it.
//
// Only media_* is touched. If the browser is ever launched from a directory whose
// cache/ holds something else, this must not be the thing that eats it.
void prune_media_cache(std::uintmax_t max_bytes) {
    namespace fs = std::filesystem;
    std::error_code ec;

    struct Entry {
        fs::path path;
        std::uintmax_t size;
        fs::file_time_type mtime;
    };
    std::vector<Entry> entries;
    std::uintmax_t total = 0;

    for (fs::directory_iterator it("cache", ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (it->path().filename().string().rfind("media_", 0) != 0) continue;

        auto size = it->file_size(ec);
        if (ec) { ec.clear(); continue; }
        auto mtime = it->last_write_time(ec);
        if (ec) { ec.clear(); continue; }

        entries.push_back({ it->path(), size, mtime });
        total += size;
    }

    if (total <= max_bytes) return;

    // Oldest first, so the eviction loop drops least-recently-written entries until
    // the directory fits the budget again.
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.mtime < b.mtime; });

    for (const auto& e : entries) {
        if (total <= max_bytes) break;
        if (fs::remove(e.path, ec)) total -= e.size;
        ec.clear();  // a file we cannot remove is skipped, not fatal
    }
}
