#pragma once
#include "types.hpp"

struct InputStyleGuard {
    InputStyleGuard(const CssStyle& merged);
    ~InputStyleGuard();
};

void render_node(DomNode& node, const CssStyle& parent_style, bool& is_inline_flow, Tab& tab, int li_index = -1, float parent_accumulated_right = 0.0f);
void DrawSpinner(ImVec2 center, float radius, float thickness, const ImVec4& color);
void DrawBackArrowIcon(ImVec2 center, ImU32 color, float thickness = 2.0f);
void DrawForwardArrowIcon(ImVec2 center, ImU32 color, float thickness = 2.0f);
void DrawReloadIcon(ImVec2 center, float radius, ImU32 color, float thickness = 2.0f);
