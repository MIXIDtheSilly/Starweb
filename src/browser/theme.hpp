#pragma once
#include "imgui.h"

namespace Theme {
    constexpr ImVec4 window_bg               = ImVec4(0.075f, 0.075f, 0.090f, 1.00f);
    constexpr ImVec4 child_bg                = ImVec4(0.106f, 0.106f, 0.125f, 1.00f);
    constexpr ImVec4 popup_bg                = ImVec4(0.094f, 0.094f, 0.118f, 0.98f);
    constexpr ImVec4 border                  = ImVec4(0.24f, 0.20f, 0.51f, 0.55f);
    
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
    
    constexpr ImVec4 spinner                 = ImVec4(0.75f, 0.60f, 0.95f, 1.00f);
    // The omnibox is a hole in the chrome, not a raised field: it sits below the
    // sheet rather than on it, and the outline says where it is.
    constexpr ImVec4 omnibox_bg              = ImVec4(0.043f, 0.043f, 0.055f, 1.00f);

    // Devtools dock.
    constexpr ImVec4 dt_text                 = ImVec4(0.94f, 0.94f, 0.94f, 1.00f);
    constexpr ImVec4 dt_dim                  = ImVec4(0.67f, 0.67f, 0.70f, 1.00f);
    constexpr ImVec4 dt_accent               = ImVec4(0.55f, 0.47f, 0.96f, 1.00f);
    constexpr ImVec4 dt_field_bg             = omnibox_bg;

    #define IM_COL32_THEME(r,g,b,a) (((ImU32)(a)<<24)|((ImU32)(b)<<16)|((ImU32)(g)<<8)|((ImU32)(r)))

    // Chrome: one flat sheet, strip and toolbar sharing the window's own colour.
    // Deliberately not pure black, which read as a hole rather than a surface.
    constexpr ImU32 bar_bg                   = IM_COL32_THEME(19, 19, 23, 255);
    constexpr ImU32 toolbar_bg               = IM_COL32_THEME(19, 19, 23, 255);
    constexpr ImU32 viewport_bg              = IM_COL32_THEME(27, 27, 32, 255);

    // Border ramp: one hue at three strengths (dim/mid/bright). A control moves
    // up the ramp instead of filling: dim at rest, mid selected, bright focused.
    constexpr ImU32 outline_dim              = IM_COL32_THEME(27, 23, 61, 255);   // #1B173D
    constexpr ImU32 outline_mid              = IM_COL32_THEME(60, 51, 129, 255);  // #3C3381
    constexpr ImU32 outline_bright           = IM_COL32_THEME(73, 55, 219, 255);  // #4937DB

    // Tabs
    // An inactive tab has no box at all, so hover is the only fill in the strip.
    constexpr ImU32 tab_hover_bg             = IM_COL32_THEME(33, 33, 39, 255);
    constexpr ImU32 tab_text_on              = IM_COL32_THEME(214, 212, 219, 255); // #D6D4DB
    constexpr ImU32 tab_text_off             = IM_COL32_THEME(128, 126, 134, 255);
    constexpr ImU32 tab_close_hover_bg       = IM_COL32_THEME(60, 51, 129, 110);

    constexpr ImU32 plus_bg_hover            = IM_COL32_THEME(33, 33, 39, 255);
    constexpr ImU32 plus_bg_active           = IM_COL32_THEME(45, 43, 64, 255);
    constexpr ImU32 plus_color_normal        = IM_COL32_THEME(214, 212, 219, 255);
    constexpr ImU32 plus_color_hover         = IM_COL32_THEME(240, 240, 240, 255);

    // Toolbar
    constexpr ImU32 icon_normal              = IM_COL32_THEME(214, 212, 219, 255);
    constexpr ImU32 icon_disabled            = IM_COL32_THEME(70, 70, 78, 255);
    constexpr ImU32 lock_secure              = IM_COL32_THEME(113, 205, 132, 255); // #71CD84
    constexpr ImU32 lock_insecure            = IM_COL32_THEME(229, 115, 115, 255);

    // Devtools. Three tones plus the border ramp: panel, recess (a pane or an
    // active control), hover.
    constexpr ImU32 dt_bg                    = viewport_bg;
    constexpr ImU32 dt_recess                = bar_bg;
    constexpr ImU32 dt_hover_bg              = tab_hover_bg;
    constexpr ImU32 dt_press_bg              = plus_bg_active;
    constexpr ImU32 dt_grip                  = outline_dim;
    constexpr ImU32 dt_text_on               = IM_COL32_THEME(240, 240, 240, 255);
    constexpr ImU32 dt_text_off              = IM_COL32_THEME(140, 138, 150, 255);
    constexpr ImU32 dt_row_selected          = IM_COL32_THEME(60, 51, 129, 140);
    constexpr ImU32 dt_row_hover             = IM_COL32_THEME(33, 33, 39, 200);
}
