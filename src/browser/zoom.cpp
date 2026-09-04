#include "zoom.hpp"
#include "globals.hpp"
#include "theme.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace zoom {
namespace {

constexpr float kSteps[] = {0.25f, 0.33f, 0.50f, 0.67f, 0.75f, 0.80f, 0.90f,
                            1.00f, 1.10f, 1.25f, 1.50f, 1.75f, 2.00f, 2.50f, 3.00f};
constexpr int kStepCount = (int)(sizeof(kSteps) / sizeof(kSteps[0]));

constexpr double kHold = 1.6;
constexpr double kFade = 0.5;

double g_shown_at = -1000.0;
ImVec2 g_panel_min, g_panel_max;
bool   g_panel_known = false;

int nearest_step(float z) {
    int best = 0;
    for (int i = 1; i < kStepCount; ++i) {
        if (std::fabs(kSteps[i] - z) < std::fabs(kSteps[best] - z)) best = i;
    }
    return best;
}

// Keyed paths flash the readout so a key at the end of the ladder still shows
// the scale instead of doing nothing visible.
bool set_zoom(Tab& tab, float z, bool show = true) {
    z = std::clamp(z, kSteps[0], kSteps[kStepCount - 1]);
    if (show) g_shown_at = ImGui::GetTime();
    if (std::fabs(z - tab.zoom) < 0.0005f) return false;
    tab.zoom = z;
    tab.vp_slack = 0.0f;  // measured at the old scale
    return true;
}

bool walk(Tab& tab, int dir, bool show = true) {
    int i = std::clamp(nearest_step(tab.zoom) + dir, 0, kStepCount - 1);
    return set_zoom(tab, kSteps[i], show);
}

ImU32 fade(ImU32 c, float a) {
    float ca = (float)((c >> IM_COL32_A_SHIFT) & 0xFF) * a;
    return (c & ~IM_COL32_A_MASK) | ((ImU32)(ca + 0.5f) << IM_COL32_A_SHIFT);
}

float alpha_now() {
    double age = ImGui::GetTime() - g_shown_at;
    if (age <= kHold) return 1.0f;
    if (age >= kHold + kFade) return 0.0f;
    return 1.0f - (float)((age - kHold) / kFade);
}

}  // namespace

bool handle_input(Tab& tab) {
    ImGuiIO& io = ImGui::GetIO();
    if (!io.KeyCtrl && !io.KeySuper) return false;

    if (ImGui::IsKeyPressed(ImGuiKey_Equal, false) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadAdd, false)) {
        return walk(tab, +1);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Minus, false) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, false)) {
        return walk(tab, -1);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_0, false) ||
        ImGui::IsKeyPressed(ImGuiKey_Keypad0, false)) {
        return set_zoom(tab, 1.0f);
    }
    // Ctrl only, never Cmd: ImGui drops wheel scrolling while Ctrl is held, so
    // the page does not scroll at the same time. Trackpad pinch arrives here too.
    if (io.KeyCtrl && io.MouseWheel != 0.0f && g_panel_known &&
        io.MousePos.x >= g_panel_min.x && io.MousePos.x <= g_panel_max.x &&
        io.MousePos.y >= g_panel_min.y && io.MousePos.y <= g_panel_max.y) {
        return walk(tab, io.MouseWheel > 0.0f ? +1 : -1);
    }
    return false;
}

void draw_badge(Tab& tab, const ImVec2& panel_min, const ImVec2& panel_max) {
    g_panel_min = panel_min;
    g_panel_max = panel_max;
    g_panel_known = true;

    const float a = alpha_now();
    if (a <= 0.002f) return;

    char pct[16];
    std::snprintf(pct, sizeof(pct), "%d%%", (int)std::lround(tab.zoom * 100.0f));

    const float pad = 10.0f, gap = 12.0f, inset = 12.0f;
    ImVec2 lbl_sz = ImGui::CalcTextSize(pct);
    ImVec2 rst_sz = ImGui::CalcTextSize("Reset");
    // Fixed slot for the number so the pill does not shuffle sideways.
    const float lbl_w = std::max(lbl_sz.x, 36.0f);
    const float pill_h = rst_sz.y + 8.0f;
    const float pill_w = rst_sz.x + 20.0f;
    const float w = pad + lbl_w + gap + pill_w + pad;
    const float h = pill_h + pad * 2.0f;

    ImGui::SetNextWindowPos(ImVec2(panel_max.x - inset - w, panel_min.y + inset));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##zoom_readout", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNavFocus);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 bmin = ImGui::GetWindowPos();
    const ImVec2 bmax = ImVec2(bmin.x + w, bmin.y + h);
    const float round = h * 0.34f;

    dl->AddRectFilled(bmin, bmax, fade(Theme::bar_bg, a * 0.97f), round);
    dl->AddRect(bmin, bmax, fade(Theme::outline_mid, a), round, 0, 1.0f);
    dl->AddText(ImVec2(bmin.x + pad, bmin.y + (h - lbl_sz.y) * 0.5f),
                fade(Theme::dt_text_on, a), pct);

    const ImVec2 pmin(bmax.x - pad - pill_w, bmin.y + pad);
    const ImVec2 pmax(bmax.x - pad, pmin.y + pill_h);
    ImGui::SetCursorScreenPos(pmin);
    const bool pressed = ImGui::InvisibleButton("##zoom_reset", ImVec2(pill_w, pill_h));
    const bool hot = ImGui::IsItemHovered();
    if (hot) dl->AddRectFilled(pmin, pmax, fade(Theme::dt_hover_bg, a), pill_h * 0.5f);
    dl->AddRect(pmin, pmax, fade(hot ? Theme::outline_bright : Theme::outline_mid, a),
                pill_h * 0.5f, 0, 1.0f);
    dl->AddText(ImVec2(pmin.x + (pill_w - rst_sz.x) * 0.5f,
                       pmin.y + (pill_h - rst_sz.y) * 0.5f),
                fade(hot ? Theme::dt_text_on : Theme::dt_text_off, a), "Reset");

    // Under the pointer it stays put, so Reset can be aimed at without racing the fade.
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
        g_shown_at = ImGui::GetTime();
    }
    if (pressed) set_zoom(tab, 1.0f);

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

bool wants_frames() { return alpha_now() > 0.002f; }

bool step(Tab& tab, int dir) { return walk(tab, dir, false); }

bool reset(Tab& tab) { return set_zoom(tab, 1.0f, false); }

int percent(const Tab& tab) { return (int)std::lround(tab.zoom * 100.0f); }

bool at_limit(const Tab& tab, int dir) {
    const int i = nearest_step(tab.zoom) + dir;
    return i < 0 || i >= kStepCount;
}

}  // namespace zoom
