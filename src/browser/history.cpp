// One flat log next to the origin stores, rewritten whole on each flush. It is
// capped at a few hundred entries, so an append-only file would buy nothing.
#include "history.hpp"
#include "globals.hpp"
#include "storage.hpp"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <unordered_map>

namespace fs = std::filesystem;

namespace history {
namespace {

constexpr const char* kMagic = "STARHISTORY1";
constexpr std::size_t kMaxUrlBytes = 2048;
constexpr std::size_t kMaxTitleBytes = 512;
constexpr std::chrono::milliseconds kFlushInterval{1000};

std::vector<Visit> g_visits;
bool g_loaded = false;
bool g_dirty = false;
std::chrono::steady_clock::time_point g_last_flush{};

constexpr std::size_t kMaxIconBytes = 256u * 1024u;

fs::path store_dir() { return app_dir() / "storage"; }
fs::path store_path() { return store_dir() / "browsing.history"; }
fs::path icon_dir() { return store_dir() / "favicons"; }

// Cached by origin. An entry present but empty means the disk had nothing.
std::unordered_map<std::string, std::string> g_icons;

fs::path icon_path(const std::string& origin) {
    std::string safe;
    safe.reserve(origin.size());
    for (unsigned char c : origin) {
        safe += (std::isalnum(c) || c == '.' || c == '-') ? (char)c : '_';
    }
    if (safe.size() > 64) safe.resize(64);
    char suffix[24];
    std::snprintf(suffix, sizeof(suffix), "_%016llx",
                  (unsigned long long)std::hash<std::string>{}(origin));
    return icon_dir() / (safe + suffix + ".icon");
}

// Cut back to a lead byte, so a clipped title is still valid UTF-8.
std::string clip_utf8(const std::string& s, std::size_t limit) {
    if (s.size() <= limit) return s;
    std::size_t cut = limit;
    while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) cut--;
    return s.substr(0, cut);
}

// Length-prefixed, since a title may hold anything at all.
void load() {
    g_loaded = true;
    std::ifstream in(store_path(), std::ios::binary);
    if (!in) return;

    std::string magic;
    if (!std::getline(in, magic) || magic != kMagic) return;

    for (;;) {
        std::string header;
        if (!std::getline(in, header)) break;
        unsigned long long ulen = 0, tlen = 0;
        long long at = 0;
        if (std::sscanf(header.c_str(), "%llu %llu %lld", &ulen, &tlen, &at) != 3) break;
        if (ulen > kMaxUrlBytes || tlen > kMaxTitleBytes) break;

        Visit v;
        v.url.resize((std::size_t)ulen);
        v.title.resize((std::size_t)tlen);
        v.at = (std::int64_t)at;
        if (ulen && !in.read(&v.url[0], (std::streamsize)ulen)) break;
        if (tlen && !in.read(&v.title[0], (std::streamsize)tlen)) break;
        in.get();  // the newline closing the record

        g_visits.push_back(std::move(v));
    }
    if (g_visits.size() > kMaxVisits) {
        g_visits.erase(g_visits.begin(), g_visits.end() - kMaxVisits);
    }
}

// Temp file plus rename, so a crash mid-write leaves the previous contents.
void save() {
    std::error_code ec;
    fs::create_directories(store_dir(), ec);

    fs::path final_path = store_path();
    fs::path tmp = final_path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << kMagic << "\n";
        for (const Visit& v : g_visits) {
            out << v.url.size() << " " << v.title.size() << " " << v.at << "\n";
            out.write(v.url.data(), (std::streamsize)v.url.size());
            out.write(v.title.data(), (std::streamsize)v.title.size());
            out << "\n";
        }
        out.flush();
        if (!out) { fs::remove(tmp, ec); return; }
    }
    fs::rename(tmp, final_path, ec);
    if (ec) fs::remove(tmp, ec);
}

}  // namespace

const std::vector<Visit>& visits() {
    if (!g_loaded) load();
    return g_visits;
}

void record(const std::string& url, const std::string& title,
            const std::string& favicon_bytes) {
    if (!g_loaded) load();
    if (url.empty() || url.size() > kMaxUrlBytes) return;

    // Written straight out, not on the flush timer: a site serves its icon once.
    if (!favicon_bytes.empty() && favicon_bytes.size() <= kMaxIconBytes) {
        std::string origin = storage::origin_for_url(url);
        if (!origin.empty() && g_icons[origin] != favicon_bytes) {
            g_icons[origin] = favicon_bytes;
            std::error_code ec;
            fs::create_directories(icon_dir(), ec);
            std::ofstream out(icon_path(origin), std::ios::binary | std::ios::trunc);
            if (out) out.write(favicon_bytes.data(), (std::streamsize)favicon_bytes.size());
        }
    }

    const std::int64_t now = (std::int64_t)std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string clipped = clip_utf8(title, kMaxTitleBytes);

    if (!g_visits.empty() && g_visits.back().url == url) {
        g_visits.back().title = std::move(clipped);
        g_visits.back().at = now;
    } else {
        g_visits.push_back(Visit{url, std::move(clipped), now});
        if (g_visits.size() > kMaxVisits) g_visits.erase(g_visits.begin());
    }
    g_dirty = true;
}

const std::string* favicon(const std::string& url) {
    std::string origin = storage::origin_for_url(url);
    if (origin.empty()) return nullptr;
    auto it = g_icons.find(origin);
    if (it == g_icons.end()) {
        std::string bytes;
        std::ifstream in(icon_path(origin), std::ios::binary);
        if (in) {
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        it = g_icons.emplace(std::move(origin), std::move(bytes)).first;
    }
    return it->second.empty() ? nullptr : &it->second;
}

void clear() {
    g_loaded = true;
    g_visits.clear();
    g_icons.clear();
    g_dirty = false;
    std::error_code ec;
    fs::remove(store_path(), ec);
    fs::remove_all(icon_dir(), ec);
}

void flush(bool force) {
    if (!g_dirty) return;
    auto now = std::chrono::steady_clock::now();
    if (!force && now - g_last_flush < kFlushInterval) return;
    g_last_flush = now;
    g_dirty = false;
    save();
}

}  // namespace history
