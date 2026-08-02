#pragma once
#include "types.hpp"

struct InputStyleGuard {
    InputStyleGuard(const CssStyle& merged);
    ~InputStyleGuard();
};

// Resolves a node's effective style: inherited properties from the parent, then
// the tag rule, class rule, and inline style (in ascending precedence).
CssStyle merge_node_style(const DomNode& node, const CssStyle& parent_style, Tab& tab);

// True for `position: absolute` and `position: fixed`, both of which paint at
// their own offset and take no part in the surrounding layout.
bool is_positioned(const CssStyle& style);

void render_node(DomNode& node, const CssStyle& parent_style, bool& is_inline_flow, Tab& tab, int li_index = -1, float parent_accumulated_right = 0.0f);
void DrawSpinner(ImVec2 center, float radius, float thickness, const ImVec4& color);
// Lucide icons, drawn from their 24x24 viewBox and centred on `center`. `size`
// is the viewBox's edge, so the visible glyph is smaller than it (an arrow fills
// 14 of the 24 units, an X only 12). thickness <= 0 takes Lucide's own 2/24.
void DrawBackArrowIcon(ImVec2 center, ImU32 color, float size = 16.0f, float thickness = 0.0f);
void DrawForwardArrowIcon(ImVec2 center, ImU32 color, float size = 16.0f, float thickness = 0.0f);
void DrawReloadIcon(ImVec2 center, ImU32 color, float size = 16.0f, float thickness = 0.0f);
void DrawPlusIcon(ImVec2 center, ImU32 color, float size = 16.0f, float thickness = 0.0f);
void DrawXIcon(ImVec2 center, ImU32 color, float size = 16.0f, float thickness = 0.0f);
void DrawLockIcon(ImVec2 center, ImU32 color, bool closed, float size = 16.0f, float thickness = 0.0f);
void DrawInspectIcon(ImVec2 center, ImU32 color, float size = 16.0f);
void DrawChevronRightIcon(ImVec2 center, ImU32 color, float size = 16.0f);
void DrawBanIcon(ImVec2 center, ImU32 color, float size = 16.0f);
void DrawScrollIcon(ImVec2 center, ImU32 color, float size = 16.0f);
void DrawTriangleAlertIcon(ImVec2 center, ImU32 color, float size = 16.0f);
void DrawCircleXIcon(ImVec2 center, ImU32 color, float size = 16.0f);
