#pragma once
#include "types.hpp"
#include <cstdint>
#include <filesystem>
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

// Extra faces a page can select with `font-family`. Names are matched loosely
// (case, spaces and hyphens are ignored) and a comma list is tried left to
// right, so an unknown family just falls through to the default UI font.
void register_page_font(const std::string& family, ImFont* font);
ImFont* font_for_family(const std::string& family);
// Registration order, spelled the way they were registered; the lookup map keys
// on the normalised form.
const std::vector<std::pair<std::string, ImFont*>>& page_font_faces();

// Multiplier on every CSS pixel, set once per frame from the active tab. The
// viewport itself is not scaled, so `vw`/`vh` keep meaning the real box.
extern float page_zoom;
inline float zpx(float css_px) { return css_px * page_zoom; }

// Size of the page viewport for the frame being drawn, which is what `vw` and
// `vh` resolve against. Set once per frame before the DOM is walked.
extern float page_viewport_w;
extern float page_viewport_h;

// The same box before the vh slack is taken off it. A positioned element is out
// of the flow, so it cannot be what pushed the page past its viewport, and it
// measures against the real thing rather than the corrected height.
extern float page_viewport_w_full;
extern float page_viewport_h_full;

// Where a positioned box measures its offsets from, both set alongside the sizes
// above. The document origin is the top-left of the page content, so it scrolls
// with it; the viewport origin is the top-left of the visible box and does not.
extern ImVec2 page_document_origin;
extern ImVec2 page_viewport_origin;

// Directory holding the running executable. Bundled resources are looked up
// relative to this rather than to the working directory, which is wherever the
// user happened to launch from.
const std::filesystem::path& app_dir();

Tab* find_tab_by_id(int tab_id);
// The tab's live page interpreter, or null if it has none. Devtools uses it for the
// console prompt and the script counters; the engines themselves stay private to
// browser.cpp, which owns their lifetime.
class ScriptEngine;
ScriptEngine* script_engine_for(int tab_id);
std::string get_cache_filepath(const std::string& url);
void prune_media_cache(std::uintmax_t max_bytes);
void script_dispatch_click(int tab_id, uint64_t node_id);
bool script_has_click_handler(int tab_id, uint64_t node_id);
void script_dispatch_input(int tab_id, uint64_t node_id);
const std::vector<CanvasOp>* script_canvas_ops(int tab_id, uint64_t node_id);
void script_set_canvas_size(int tab_id, uint64_t node_id, float w, float h);
void script_set_canvas_hover(int tab_id, uint64_t node_id, float x, float y);
void script_set_canvas_font(int tab_id, uint64_t node_id, ImFont* font, float size);
