#include "theme.hpp"
#include "globals.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <string>

namespace fs = std::filesystem;

namespace Theme {
namespace {

#define C IM_COL32_THEME

const Family kFamilies[] = {
    { "Nebula",
      { C(19,19,23,255), C(27,27,32,255), C(11,11,14,255), C(33,33,39,255), C(45,43,64,255),
        C(27,23,61,255), C(60,51,129,255), C(73,55,219,255), C(148,97,219,255),
        C(214,212,219,255), C(128,126,134,255) },
      { C(238,236,246,255), C(249,248,252,255), C(229,226,241,255), C(231,228,243,255), C(216,211,236,255),
        C(219,215,235,255), C(183,174,222,255), C(104,80,224,255), C(116,70,198,255),
        C(32,30,40,255), C(110,106,124,255) } },

    { "Ember",
      { C(24,18,16,255), C(33,25,22,255), C(15,11,10,255), C(43,32,28,255), C(61,42,35,255),
        C(58,28,19,255), C(120,58,36,255), C(219,96,47,255), C(230,126,64,255),
        C(219,210,205,255), C(136,124,117,255) },
      { C(243,236,231,255), C(251,247,244,255), C(235,226,218,255), C(237,228,220,255), C(228,214,203,255),
        C(228,216,206,255), C(222,176,146,255), C(202,84,38,255), C(192,90,36,255),
        C(42,32,26,255), C(124,108,98,255) } },

    { "Verdant",
      { C(16,22,19,255), C(23,31,27,255), C(10,14,12,255), C(29,39,34,255), C(38,55,46,255),
        C(20,48,35,255), C(43,97,71,255), C(52,176,118,255), C(74,190,130,255),
        C(208,216,211,255), C(122,134,127,255) },
      { C(235,242,237,255), C(247,251,248,255), C(226,237,230,255), C(228,239,232,255), C(213,231,219,255),
        C(217,232,222,255), C(166,210,185,255), C(28,144,91,255), C(30,142,92,255),
        C(26,38,31,255), C(100,118,108,255) } },

    { "Tide",
      { C(15,20,26,255), C(22,28,36,255), C(9,12,16,255), C(28,36,46,255), C(36,50,66,255),
        C(18,38,61,255), C(38,79,124,255), C(45,140,220,255), C(70,150,235,255),
        C(208,214,222,255), C(120,130,143,255) },
      { C(235,240,247,255), C(247,250,253,255), C(226,234,244,255), C(228,236,246,255), C(211,225,241,255),
        C(216,227,239,255), C(164,195,230,255), C(26,114,202,255), C(28,114,202,255),
        C(26,34,44,255), C(102,114,130,255) } },

    { "Rose",
      { C(24,16,21,255), C(33,23,30,255), C(14,9,13,255), C(43,30,39,255), C(60,40,53,255),
        C(56,22,40,255), C(116,46,82,255), C(212,66,130,255), C(228,94,148,255),
        C(222,208,216,255), C(136,120,130,255) },
      { C(245,235,241,255), C(252,247,250,255), C(238,225,234,255), C(240,227,236,255), C(232,211,225,255),
        C(232,217,227,255), C(228,171,199,255), C(192,46,110,255), C(190,56,114,255),
        C(42,28,36,255), C(124,102,114,255) } },

    { "Carbon",
      { C(18,18,18,255), C(26,26,26,255), C(11,11,11,255), C(33,33,33,255), C(48,48,48,255),
        C(38,38,38,255), C(76,76,76,255), C(146,146,150,255), C(160,160,168,255),
        C(214,214,216,255), C(130,130,134,255) },
      { C(237,237,237,255), C(248,248,248,255), C(228,228,228,255), C(230,230,230,255), C(216,216,216,255),
        C(220,220,220,255), C(192,192,192,255), C(112,112,116,255), C(92,92,98,255),
        C(30,30,30,255), C(110,110,114,255) } },
};

#undef C

constexpr int kCount = (int)(sizeof(kFamilies) / sizeof(kFamilies[0]));

int  g_family = 0;
bool g_light = false;

constexpr float kEase = 0.22f;  // seconds
Preset g_from = kFamilies[0].dark;
Preset g_to = kFamilies[0].dark;
float g_from_light = 0.0f, g_to_light = 0.0f;
float g_t = 1.0f;
unsigned g_revision = 0;

float ease(float t) { return t * t * (3.0f - 2.0f * t); }

fs::path store_path() { return app_dir() / "storage" / "theme"; }

int chan(ImU32 c, int shift) { return (int)((c >> shift) & 0xFF); }

ImVec4 vec4(ImU32 c) {
    return ImVec4(chan(c, IM_COL32_R_SHIFT) / 255.0f, chan(c, IM_COL32_G_SHIFT) / 255.0f,
                  chan(c, IM_COL32_B_SHIFT) / 255.0f, chan(c, IM_COL32_A_SHIFT) / 255.0f);
}

// Keeps x's alpha; only the colour travels toward y.
ImU32 mix(ImU32 x, ImU32 y, float t) {
    auto lerp = [&](int shift) {
        return (ImU32)(chan(x, shift) + (chan(y, shift) - chan(x, shift)) * t + 0.5f);
    };
    return (lerp(IM_COL32_R_SHIFT) << IM_COL32_R_SHIFT) | (lerp(IM_COL32_G_SHIFT) << IM_COL32_G_SHIFT) |
           (lerp(IM_COL32_B_SHIFT) << IM_COL32_B_SHIFT) | ((ImU32)chan(x, IM_COL32_A_SHIFT) << IM_COL32_A_SHIFT);
}

ImU32 with_a(ImU32 c, int a) {
    return (c & ~IM_COL32_A_MASK) | ((ImU32)a << IM_COL32_A_SHIFT);
}

void derive(const Preset& p, float lit);

Preset blend(const Preset& a, const Preset& b, float t) {
    return { mix(a.sheet, b.sheet, t), mix(a.ground, b.ground, t), mix(a.well, b.well, t),
             mix(a.hover, b.hover, t), mix(a.press, b.press, t), mix(a.line_dim, b.line_dim, t),
             mix(a.line_mid, b.line_mid, t), mix(a.line_bright, b.line_bright, t),
             mix(a.accent, b.accent, t), mix(a.ink, b.ink, t), mix(a.ink_dim, b.ink_dim, t) };
}

void push_style() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.Colors[ImGuiCol_WindowBg] = window_bg;
    style.Colors[ImGuiCol_ChildBg] = child_bg;
    style.Colors[ImGuiCol_PopupBg] = popup_bg;
    style.Colors[ImGuiCol_Border] = border;
    style.Colors[ImGuiCol_FrameBg] = frame_bg;
    style.Colors[ImGuiCol_FrameBgHovered] = frame_bg_hovered;
    style.Colors[ImGuiCol_FrameBgActive] = frame_bg_active;
    style.Colors[ImGuiCol_Header] = header;
    style.Colors[ImGuiCol_HeaderHovered] = header_hovered;
    style.Colors[ImGuiCol_HeaderActive] = header_active;
    style.Colors[ImGuiCol_Button] = button;
    style.Colors[ImGuiCol_ButtonHovered] = button_hovered;
    style.Colors[ImGuiCol_ButtonActive] = button_active;
    style.Colors[ImGuiCol_ScrollbarGrab] = scrollbar_grab;
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = scrollbar_grab_hovered;
    style.Colors[ImGuiCol_ScrollbarGrabActive] = scrollbar_grab_active;
    style.Colors[ImGuiCol_CheckMark] = checkmark;
    style.Colors[ImGuiCol_SliderGrab] = slider_grab;
    style.Colors[ImGuiCol_SliderGrabActive] = slider_grab_active;
    style.Colors[ImGuiCol_InputTextCursor] = input_text_cursor;
    style.Colors[ImGuiCol_Text] = text;
}

void derive(const Preset& p, float lit) {
    // Where "more contrast" points: a dark theme brightens, a light one darkens.
    const ImU32 far_side = mix(IM_COL32(255, 255, 255, 255), IM_COL32(0, 0, 0, 255), lit);
    const ImU32 hot = mix(p.accent, far_side, 0.18f);

    bar_bg = toolbar_bg = dt_recess = p.sheet;
    viewport_bg = dt_bg = p.ground;
    tab_hover_bg = plus_bg_hover = dt_hover_bg = p.hover;
    plus_bg_active = dt_press_bg = p.press;
    outline_dim = dt_grip = p.line_dim;
    outline_mid = p.line_mid;
    outline_bright = p.line_bright;
    tab_close_hover_bg = with_a(p.line_mid, 110);
    dt_row_selected = with_a(p.line_mid, 140);
    dt_row_hover = with_a(p.hover, 200);

    tab_text_on = icon_normal = plus_color_normal = p.ink;
    tab_text_off = dt_text_off = p.ink_dim;
    plus_color_hover = dt_text_on = mix(p.ink, far_side, 0.5f);
    icon_disabled = mix(p.ink_dim, p.sheet, 0.5f);
    lock_secure = mix(IM_COL32(113, 205, 132, 255), IM_COL32(32, 132, 66, 255), lit);
    lock_insecure = mix(IM_COL32(229, 115, 115, 255), IM_COL32(190, 60, 60, 255), lit);

    window_bg = vec4(p.sheet);
    child_bg = vec4(p.ground);
    popup_bg = vec4(with_a(p.ground, 250));
    omnibox_bg = dt_field_bg = vec4(p.well);
    border = vec4(with_a(p.line_mid, 140));

    frame_bg = vec4(p.hover);
    frame_bg_hovered = vec4(p.press);
    frame_bg_active = vec4(mix(p.press, p.line_mid, 0.5f));
    header = vec4(with_a(p.line_mid, 115));
    header_hovered = vec4(with_a(p.line_mid, 155));
    header_active = vec4(p.line_mid);
    scrollbar_grab = vec4(with_a(p.ink_dim, 150));
    scrollbar_grab_hovered = vec4(with_a(p.ink_dim, 180));
    scrollbar_grab_active = vec4(with_a(p.ink_dim, 220));

    text = input_text_cursor = dt_text = page_text = vec4(p.ink);
    dt_dim = vec4(p.ink_dim);
    dt_accent = vec4(hot);

    form_accent = vec4(p.accent);
    form_accent_hover = vec4(mix(p.accent, IM_COL32(0, 0, 0, 255), 0.16f));
    checkmark = slider_grab_active = spinner = vec4(hot);
    slider_grab = vec4(with_a(p.accent, 205));
    button = vec4(with_a(p.accent, 153));
    button_hovered = vec4(with_a(hot, 205));
    button_active = vec4(p.accent);

    push_style();
}

}  // namespace

const Family* families() { return kFamilies; }
int family_count() { return kCount; }
int current() { return g_family; }
bool is_light() { return g_light; }

std::string request_header() {
    const Preset& p = target();
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "name=%s; scheme=%s; accent=#%02x%02x%02x; focus=#%02x%02x%02x",
                  kFamilies[g_family].name, g_light ? "light" : "dark",
                  (p.accent >> IM_COL32_R_SHIFT) & 0xFF,
                  (p.accent >> IM_COL32_G_SHIFT) & 0xFF,
                  (p.accent >> IM_COL32_B_SHIFT) & 0xFF,
                  (p.line_bright >> IM_COL32_R_SHIFT) & 0xFF,
                  (p.line_bright >> IM_COL32_G_SHIFT) & 0xFF,
                  (p.line_bright >> IM_COL32_B_SHIFT) & 0xFF);
    return buf;
}
bool animating() { return g_t < 1.0f; }
float light_amount() { return g_from_light + (g_to_light - g_from_light) * ease(g_t); }
const Preset& target() { return variant(kFamilies[g_family], g_light); }
unsigned revision() { return g_revision; }

void apply(int family, bool light) {
    if (family < 0 || family >= kCount) return;
    // Start from the colours on screen, so a switch mid-ease does not jump back.
    g_from = blend(g_from, g_to, ease(g_t));
    g_from_light = light_amount();
    g_to = variant(kFamilies[family], light);
    g_to_light = light ? 1.0f : 0.0f;
    g_t = 0.0f;
    g_family = family;
    g_light = light;
    ++g_revision;
    derive(g_from, g_from_light);
}

void tick(float dt) {
    if (g_t >= 1.0f) return;
    g_t = std::min(1.0f, g_t + dt / kEase);
    derive(blend(g_from, g_to, ease(g_t)), light_amount());
}

namespace {
void settle(int family, bool light) {
    apply(family, light);
    g_t = 1.0f;
    derive(g_to, g_to_light);
}
}  // namespace

void load() {
    std::ifstream in(store_path());
    std::string name, mode;
    if (in && in >> name) {
        in >> mode;
        for (int i = 0; i < kCount; ++i) {
            if (name == kFamilies[i].name) { settle(i, mode == "light"); return; }
        }
    }
    settle(0, false);
}

void save() {
    std::error_code ec;
    fs::create_directories(store_path().parent_path(), ec);
    std::ofstream out(store_path(), std::ios::trunc);
    if (out) out << kFamilies[g_family].name << " " << (g_light ? "light" : "dark") << "\n";
}

}  // namespace Theme
