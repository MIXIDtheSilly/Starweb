#pragma once
#include "types.hpp"
#include <cstdint>
#include <vector>
#include <mutex>

extern std::vector<Tab> tabs;
extern int active_tab_idx;
extern int next_tab_id;
extern bool is_window_maximized;
extern int restored_x;
extern int restored_y;
extern int restored_w;
extern int restored_h;
extern std::mutex fetch_mutex;
extern ImFont* mono_font;

Tab* find_tab_by_id(int tab_id);
std::string get_cache_filepath(const std::string& url);
void prune_media_cache(std::uintmax_t max_bytes);
void script_dispatch_click(int tab_id, uint64_t node_id);
const std::vector<CanvasOp>* script_canvas_ops(int tab_id, uint64_t node_id);
void script_set_canvas_size(int tab_id, uint64_t node_id, float w, float h);
