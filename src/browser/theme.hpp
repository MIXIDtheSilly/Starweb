#pragma once
#include "imgui.h"

namespace Theme {
    constexpr ImVec4 window_bg               = ImVec4(0.11f, 0.11f, 0.11f, 1.00f);
    constexpr ImVec4 child_bg                = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    constexpr ImVec4 popup_bg                = ImVec4(0.15f, 0.15f, 0.15f, 0.98f);
    constexpr ImVec4 border                  = ImVec4(0.30f, 0.30f, 0.30f, 0.40f);
    
    constexpr ImVec4 frame_bg                = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
    constexpr ImVec4 frame_bg_hovered        = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
    constexpr ImVec4 frame_bg_active         = ImVec4(0.28f, 0.28f, 0.28f, 1.00f);
    
    constexpr ImVec4 header                  = ImVec4(0.30f, 0.30f, 0.30f, 0.45f);
    constexpr ImVec4 header_hovered          = ImVec4(0.40f, 0.40f, 0.40f, 0.60f);
    constexpr ImVec4 header_active           = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    
    constexpr ImVec4 button                  = ImVec4(0.70f, 0.55f, 0.90f, 0.60f);
    constexpr ImVec4 button_hovered          = ImVec4(0.75f, 0.60f, 0.95f, 0.80f);
    constexpr ImVec4 button_active           = ImVec4(0.65f, 0.45f, 0.85f, 1.00f);
    
    constexpr ImVec4 scrollbar_grab          = ImVec4(0.35f, 0.35f, 0.35f, 0.60f);
    constexpr ImVec4 scrollbar_grab_hovered  = ImVec4(0.45f, 0.45f, 0.45f, 0.70f);
    constexpr ImVec4 scrollbar_grab_active   = ImVec4(0.55f, 0.55f, 0.55f, 1.00f);
    
    constexpr ImVec4 checkmark               = ImVec4(0.75f, 0.60f, 0.95f, 1.00f);
    constexpr ImVec4 slider_grab             = ImVec4(0.70f, 0.55f, 0.90f, 0.80f);
    constexpr ImVec4 slider_grab_active      = ImVec4(0.75f, 0.60f, 0.95f, 1.00f);
    
    constexpr ImVec4 input_text_cursor       = ImVec4(0.95f, 0.95f, 0.95f, 1.00f);
    constexpr ImVec4 text                    = ImVec4(0.95f, 0.95f, 0.95f, 1.00f);

    // Accent used to tint the Chromium-style form controls (checkbox fill,
    // radio dot, slider, focus, date/time picker highlights).
    constexpr ImVec4 form_accent             = ImVec4(0.58f, 0.38f, 0.86f, 1.00f); // ~148,97,219
    constexpr ImVec4 form_accent_hover       = ImVec4(0.49f, 0.30f, 0.78f, 1.00f); // ~125,77,199
    
    constexpr ImVec4 btn_hover_highlight     = ImVec4(0.70f, 0.55f, 0.90f, 0.20f);
    constexpr ImVec4 btn_active_highlight    = ImVec4(0.70f, 0.55f, 0.90f, 0.35f);
    
    constexpr ImVec4 spinner                 = ImVec4(0.75f, 0.60f, 0.95f, 1.00f);
    constexpr ImVec4 omnibox_bg              = ImVec4(0.11f, 0.11f, 0.11f, 1.00f);

    #define IM_COL32_THEME(r,g,b,a) (((ImU32)(a)<<24)|((ImU32)(b)<<16)|((ImU32)(g)<<8)|((ImU32)(r)))
    
    constexpr ImU32 bar_bg                   = IM_COL32_THEME(24, 24, 24, 255);
    constexpr ImU32 toolbar_bg               = IM_COL32_THEME(40, 40, 40, 255);
    constexpr ImU32 viewport_bg              = IM_COL32_THEME(30, 30, 30, 255);
    
    constexpr ImU32 tab_active_bg            = IM_COL32_THEME(40, 40, 40, 255);
    constexpr ImU32 tab_hover_bg             = IM_COL32_THEME(50, 50, 50, 255);
    constexpr ImU32 tab_inactive_bg          = IM_COL32_THEME(28, 28, 28, 255);
    constexpr ImU32 tab_accent_stripe        = IM_COL32_THEME(186, 140, 245, 255);
    constexpr ImU32 tab_divider              = IM_COL32_THEME(120, 120, 120, 255);
    
    constexpr ImU32 plus_bg_normal           = IM_COL32_THEME(40, 40, 40, 255);
    constexpr ImU32 plus_bg_hover            = IM_COL32_THEME(60, 60, 60, 255);
    constexpr ImU32 plus_bg_active           = IM_COL32_THEME(75, 75, 75, 255);
    constexpr ImU32 plus_color_normal        = IM_COL32_THEME(180, 180, 180, 255);
    constexpr ImU32 plus_color_hover         = IM_COL32_THEME(240, 240, 240, 255);
    
    constexpr ImU32 border_separator         = IM_COL32_THEME(80, 80, 80, 255);
}
