#pragma once
#include "types.hpp"
#include "imgui.h"

// Ctrl/Cmd with +, -, 0 or the wheel walks the active tab along a fixed ladder
// of scales, with a readout that fades back out.
namespace zoom {

// True when the scale actually moved, so the caller can request reflow frames.
bool handle_input(Tab& tab);

// Also records the panel rect that Ctrl+wheel tests the pointer against.
void draw_badge(Tab& tab, const ImVec2& panel_min, const ImVec2& panel_max);

bool wants_frames();

// Menu controls. Silent: the row already shows the scale.
bool step(Tab& tab, int dir);
bool reset(Tab& tab);
int  percent(const Tab& tab);
bool at_limit(const Tab& tab, int dir);

}
