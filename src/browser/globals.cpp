#include "globals.hpp"
#include <algorithm>
#include <cctype>

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
