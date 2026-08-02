#pragma once
#include "types.hpp"
#include <cstddef>
#include <cstdint>
#include <string>

// Developer panel: a right-side dock over the page viewport, toggled with F12 or
// Cmd/Ctrl+Shift+I. State lives in a file-local map keyed by tab id rather than on
// Tab, for the same reason the script engines do: Tab is copied into its vector.
//
// The console and network stores are written from fetch worker threads and read on
// the render thread, so they are behind a mutex of their own. That mutex must never
// be held across a call into Lua: luaL_error longjmps past destructors.
namespace devtools {

enum class Level { Log, Warn, Error, Input, Result };

// Any thread.
void log(int tab_id, Level lvl, std::string text, std::string source = {}, int line = 0);
std::uint64_t net_begin(int tab_id, const std::string& url, const std::string& method,
                        const char* initiator);
void net_end(std::uint64_t rec, const FetchResult& res, std::size_t body_bytes);

// Render thread only.
void toggle(int tab_id);
bool is_open(int tab_id);
// Width the dock takes this frame, already clamped to the window; 0 when closed.
float dock_width(int tab_id, float shell_avail_w);
void drag_dock(float delta_x);
void draw(Tab& tab);
void draw_overlay(Tab& tab, ImVec2 vp_min, ImVec2 vp_max);
void on_navigate(int tab_id);
void on_tab_closed(int tab_id);
void set_open(int tab_id, bool open);
// Selects a panel by name ("elements", "console", "network", "sources", "metrics").
void set_panel(int tab_id, const std::string& name);
// Selects a node the way the picker would, by "#id", ".class" or tag name.
void select_node(Tab& tab, const std::string& query);

// True while the picker is armed: a page click selects a node instead of reaching
// the page.
bool pick_active(int tab_id);

// render_node reports each element's painted box here. `capturing` is a plain bool
// read so a page with the panel closed pays one branch per element.
bool capturing(int tab_id);
void note_box(int tab_id, std::uint64_t node_id, ImVec2 min, ImVec2 max);
void begin_frame(int tab_id);

// The Metrics panel animates, so it asks the idle loop to keep drawing.
bool wants_frames(int tab_id);
void note_idle_wait(double seconds);

}  // namespace devtools
