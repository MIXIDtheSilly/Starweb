#pragma once
#include "types.hpp"

struct InputStyleGuard {
    InputStyleGuard(const CssStyle& merged);
    ~InputStyleGuard();
};

// Resolves a node's effective style: inherited properties from the parent, then
// the tag rule, class rule, and inline style (in ascending precedence).
CssStyle merge_node_style(const DomNode& node, const CssStyle& parent_style, Tab& tab);

void render_node(DomNode& node, const CssStyle& parent_style, bool& is_inline_flow, Tab& tab, int li_index = -1, float parent_accumulated_right = 0.0f);
void DrawSpinner(ImVec2 center, float radius, float thickness, const ImVec4& color);
void DrawBackArrowIcon(ImVec2 center, ImU32 color, float thickness = 2.0f);
void DrawForwardArrowIcon(ImVec2 center, ImU32 color, float thickness = 2.0f);
void DrawReloadIcon(ImVec2 center, float radius, ImU32 color, float thickness = 2.0f);
void DrawLockIcon(ImVec2 center, ImU32 color, bool closed, float size = 16.0f);
