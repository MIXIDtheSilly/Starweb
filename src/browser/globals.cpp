#include "globals.hpp"

std::vector<Tab> tabs;
int active_tab_idx = 0;
int next_tab_id = 1;
bool is_window_maximized = false;
int restored_x = 100;
int restored_y = 100;
int restored_w = 1024;
int restored_h = 768;
std::mutex fetch_mutex;

Tab* find_tab_by_id(int tab_id) {
    for (auto& tab : tabs) {
        if (tab.id == tab_id) {
            return &tab;
        }
    }
    return nullptr;
}
