#include "renderer.hpp"
#include "parser.hpp"
#include "fetcher.hpp"
#include "globals.hpp"
#include "media_source.hpp"
#include "theme.hpp"
#include "media_player.hpp"
#include "layout.hpp"
#include "../common/url_parser.hpp"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cfloat>
#include <algorithm>
#include <filesystem>
#include <vector>

#ifdef __APPLE__
// Native macOS open panel, implemented in media_player_mac.mm.
std::string PlatformOpenFileDialog();
#endif

// Both field guards below fall back to these, so an <input> and a <select> that
// specify no padding are the same height standing next to each other.
static constexpr float kFieldPadX = 4.0f;
static constexpr float kFieldPadY = 2.0f;

InputStyleGuard::InputStyleGuard(const CssStyle& merged) {
    float rounding = merged.border_radius >= 0.0f ? merged.border_radius : 0.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, rounding);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, merged.border_width >= 0.0f ? merged.border_width : 1.0f);

    float pad_x = merged.padding_left > 0.0f ? merged.padding_left : kFieldPadX;
    float pad_y = merged.padding_top > 0.0f ? merged.padding_top : kFieldPadY;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(pad_x, pad_y));
    
    ImVec4 frame_bg = merged.has_bg ? merged.bg_color : ImVec4(0.16f, 0.16f, 0.18f, 1.00f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, frame_bg);
    
    ImVec4 frame_bg_hovered = merged.has_bg 
        ? ImVec4(frame_bg.x * 0.95f, frame_bg.y * 0.95f, frame_bg.z * 0.95f, frame_bg.w) 
        : ImVec4(0.22f, 0.20f, 0.26f, 1.00f);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, frame_bg_hovered);
    
    ImVec4 frame_bg_active = merged.has_bg 
        ? ImVec4(frame_bg.x * 0.90f, frame_bg.y * 0.90f, frame_bg.z * 0.90f, frame_bg.w) 
        : ImVec4(0.28f, 0.24f, 0.35f, 1.00f);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, frame_bg_active);
    
    ImVec4 text_color = merged.has_color ? merged.color : ImVec4(0.95f, 0.95f, 0.95f, 1.00f);
    ImGui::PushStyleColor(ImGuiCol_Text, text_color);
    
    ImVec4 border_color = merged.has_border_color ? merged.border_color : ImVec4(0.24f, 0.20f, 0.35f, 0.60f);
    ImGui::PushStyleColor(ImGuiCol_Border, border_color);
    
    ImGui::PushStyleColor(ImGuiCol_InputTextCursor, text_color);
}

InputStyleGuard::~InputStyleGuard() {
    ImGui::PopStyleColor(6);
    ImGui::PopStyleVar(3);
}

// The surrounding UI is dark, but native <input> widgets in Chrome render with a
// light "form control" skin, so these helpers reproduce that look and store widget
// state back on the DomNode so it survives across frames.
namespace Chrome {
    static ImU32 accent()      { return ImGui::ColorConvertFloat4ToU32(Theme::form_accent); }
    static ImU32 accentHover() { return ImGui::ColorConvertFloat4ToU32(Theme::form_accent_hover); }
    static const ImU32 kBorder      = IM_COL32(118, 118, 118, 255);
    static const ImU32 kBorderHover = IM_COL32( 80,  80,  80, 255);
    static const ImU32 kWhite       = IM_COL32(255, 255, 255, 255);
    static const ImU32 kInk         = IM_COL32( 20,  20,  20, 255);

    static bool Checkbox(const std::string& id, bool* v) {
        ImGui::PushID(id.c_str());
        float sz = std::round(ImGui::GetFontSize() * 0.82f); // Chrome checkbox ~13px
        if (sz < 12.0f) sz = 12.0f;
        ImVec2 pos = ImGui::GetCursorScreenPos();
        bool pressed = ImGui::InvisibleButton("cb", ImVec2(sz, sz));
        if (pressed) *v = !*v;
        bool hovered = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 pmax(pos.x + sz, pos.y + sz);
        float r = 2.0f;
        if (*v) {
            dl->AddRectFilled(pos, pmax, hovered ? accentHover() : accent(), r);
            ImVec2 a(pos.x + sz * 0.22f, pos.y + sz * 0.52f);
            ImVec2 b(pos.x + sz * 0.42f, pos.y + sz * 0.73f);
            ImVec2 c(pos.x + sz * 0.78f, pos.y + sz * 0.28f);
            dl->AddLine(a, b, kWhite, 1.8f);
            dl->AddLine(b, c, kWhite, 1.8f);
        } else {
            dl->AddRectFilled(pos, pmax, kWhite, r);
            dl->AddRect(pos, pmax, hovered ? kBorderHover : kBorder, r, 0, 1.5f);
        }
        ImGui::PopID();
        return pressed;
    }

    static bool Radio(const std::string& id, bool active) {
        ImGui::PushID(id.c_str());
        float sz = std::round(ImGui::GetFontSize() * 0.82f); // Chrome radio ~13px
        if (sz < 12.0f) sz = 12.0f;
        ImVec2 pos = ImGui::GetCursorScreenPos();
        bool pressed = ImGui::InvisibleButton("rb", ImVec2(sz, sz));
        bool hovered = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 center(pos.x + sz * 0.5f, pos.y + sz * 0.5f);
        float rad = sz * 0.5f - 1.0f;
        if (active) {
            dl->AddCircleFilled(center, rad, hovered ? accentHover() : accent(), 24);
            dl->AddCircleFilled(center, rad * 0.42f, kWhite, 24);
        } else {
            dl->AddCircleFilled(center, rad, kWhite, 24);
            dl->AddCircle(center, rad, hovered ? kBorderHover : kBorder, 24, 1.5f);
        }
        ImGui::PopID();
        return pressed;
    }

    static bool Button(const std::string& label, const std::string& id, ImVec2 size) {
        ImGui::PushID(id.c_str());
        ImVec2 text_sz = ImGui::CalcTextSize(label.c_str());
        float w = size.x > 0.0f ? size.x : text_sz.x + 14.0f;
        float h = size.y > 0.0f ? size.y : text_sz.y + 6.0f;
        ImVec2 pos = ImGui::GetCursorScreenPos();
        bool pressed = ImGui::InvisibleButton("bt", ImVec2(w, h));
        bool hovered = ImGui::IsItemHovered();
        bool held = ImGui::IsItemActive();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 pmax(pos.x + w, pos.y + h);
        float r = 3.0f;
        ImU32 bg = held   ? IM_COL32(218, 218, 218, 255)
                 : hovered ? IM_COL32(230, 230, 230, 255)
                           : IM_COL32(239, 239, 239, 255);
        dl->AddRectFilled(pos, pmax, bg, r);
        dl->AddRect(pos, pmax, hovered ? kBorderHover : kBorder, r, 0, 1.0f);
        ImVec2 tp(pos.x + (w - text_sz.x) * 0.5f, pos.y + (h - text_sz.y) * 0.5f);
        dl->AddText(tp, kInk, label.c_str());
        ImGui::PopID();
        return pressed;
    }

    static int dowSun(int y, int m, int d) { // 0=Sun .. 6=Sat
        static const int t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
        if (m < 3) y -= 1;
        return (y + y/4 - y/100 + y/400 + t[(m-1+12)%12] + d) % 7;
    }
    static int daysInMonth(int y, int m) {
        static const int dm[] = {31,28,31,30,31,30,31,31,30,31,30,31};
        if (m == 2 && ((y%4==0 && y%100!=0) || y%400==0)) return 29;
        return dm[(m-1+12)%12];
    }
    static const char* monthName(int m) {
        static const char* n[] = {"January","February","March","April","May","June",
                                  "July","August","September","October","November","December"};
        return n[(m-1+12)%12];
    }

    // The InvisibleButton stays the "last item", so callers can anchor a popup or a spinner to it.
    static bool ValueField(const std::string& id, const std::string& text, bool placeholder, float width) {
        ImGui::PushID(id.c_str());
        ImVec2 pos = ImGui::GetCursorScreenPos();
        float h = ImGui::GetFontSize() + 8.0f;
        bool clicked = ImGui::InvisibleButton("vf", ImVec2(width, h));
        bool hovered = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 pmax(pos.x + width, pos.y + h);
        dl->AddRectFilled(pos, pmax, kWhite, 2.0f);
        dl->AddRect(pos, pmax, hovered ? kBorderHover : kBorder, 2.0f, 0, 1.0f);
        ImVec2 tsz = ImGui::CalcTextSize(text.c_str());
        dl->PushClipRect(ImVec2(pos.x + 4, pos.y), ImVec2(pmax.x - 4, pmax.y), true);
        dl->AddText(ImVec2(pos.x + 6.0f, pos.y + (h - tsz.y) * 0.5f),
                    placeholder ? IM_COL32(115,115,115,255) : kInk, text.c_str());
        dl->PopClipRect();
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::PopID();
        return clicked;
    }

    static bool NavArrow(const char* id, bool forward, float box) {
        ImGui::PushID(id);
        ImVec2 pos = ImGui::GetCursorScreenPos();
        bool clicked = ImGui::InvisibleButton("na", ImVec2(box, box));
        bool hovered = ImGui::IsItemHovered();
        ImVec2 c(pos.x + box * 0.5f, pos.y + box * 0.5f);
        ImU32 col = hovered ? accent() : IM_COL32(70, 70, 70, 255);
        if (forward) DrawForwardArrowIcon(c, col, 2.0f);
        else         DrawBackArrowIcon(c, col, 2.0f);
        ImGui::PopID();
        return clicked;
    }

    static int Spinner(const std::string& id, float height) {
        ImGui::PushID(id.c_str());
        float w = 17.0f;
        ImVec2 pos = ImGui::GetCursorScreenPos();
        int result = 0;
        bool upHov = false, dnHov = false;
        ImGui::BeginGroup();
        if (ImGui::InvisibleButton("up", ImVec2(w, height * 0.5f))) result = 1;
        upHov = ImGui::IsItemHovered();
        ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + height * 0.5f));
        if (ImGui::InvisibleButton("dn", ImVec2(w, height * 0.5f))) result = -1;
        dnHov = ImGui::IsItemHovered();
        ImGui::EndGroup();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 pmax(pos.x + w, pos.y + height);
        dl->AddRectFilled(pos, pmax, kWhite, 2.0f);
        dl->AddRect(pos, pmax, kBorder, 2.0f, 0, 1.0f);
        dl->AddLine(ImVec2(pos.x, pos.y + height * 0.5f), ImVec2(pmax.x, pos.y + height * 0.5f), kBorder, 1.0f);
        float cx = pos.x + w * 0.5f;
        float uy = pos.y + height * 0.30f, dy = pos.y + height * 0.70f;
        dl->AddTriangleFilled(ImVec2(cx-3, uy+2), ImVec2(cx+3, uy+2), ImVec2(cx, uy-2), upHov ? accent() : kInk);
        dl->AddTriangleFilled(ImVec2(cx-3, dy-2), ImVec2(cx+3, dy-2), ImVec2(cx, dy+2), dnHov ? accent() : kInk);
        ImGui::PopID();
        return result;
    }

    static void addDays(int& y, int& m, int& d, int delta) {
        d += delta;
        while (d < 1)                 { if (--m < 1) { m = 12; y--; } d += daysInMonth(y, m); }
        while (d > daysInMonth(y, m)) { d -= daysInMonth(y, m); if (++m > 12) { m = 1; y++; } }
    }

    static std::unordered_map<std::string,int>& viewYear() { static std::unordered_map<std::string,int> m; return m; }
    static std::unordered_map<std::string,int>& viewMon()  { static std::unordered_map<std::string,int> m; return m; }

    static void styleAccentButtons() {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(Theme::form_accent.x, Theme::form_accent.y, Theme::form_accent.z, 0.18f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(Theme::form_accent.x, Theme::form_accent.y, Theme::form_accent.z, 0.32f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.12f,0.12f,0.12f,1.0f));
    }
    static void unstyleAccentButtons() { ImGui::PopStyleColor(4); }

    // `uid` keys the currently-viewed month, since the view can differ from the selected date.
    static bool CalendarGrid(const std::string& uid, int selY, int selM, int selD,
                             int& outY, int& outM, int& outD) {
        int& vy = viewYear()[uid];
        int& vm = viewMon()[uid];
        if (vy == 0) { vy = (selY > 0 ? selY : 2026); vm = (selM > 0 ? selM : 1); }

        bool picked = false;
        float navBox = ImGui::GetFontSize() + 6.0f;
        if (NavArrow("pm", false, navBox)) { if (--vm < 1) { vm = 12; vy--; } }
        ImGui::SameLine();
        char hdr[32]; std::snprintf(hdr, sizeof hdr, "%s %d", monthName(vm), vy);
        float tw = ImGui::CalcTextSize(hdr).x;
        float regL = ImGui::GetWindowContentRegionMin().x, regR = ImGui::GetWindowContentRegionMax().x;
        ImGui::SetCursorPosX((regL + regR - tw) * 0.5f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(hdr);
        ImGui::SameLine();
        ImGui::SetCursorPosX(regR - navBox);
        if (NavArrow("nm", true, navBox)) { if (++vm > 12) { vm = 1; vy++; } }

        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(1, 1));
        if (ImGui::BeginTable("cal", 7, ImGuiTableFlags_SizingFixedFit)) {
            const char* wd[] = {"Su","Mo","Tu","We","Th","Fr","Sa"};
            for (int i = 0; i < 7; i++) { ImGui::TableNextColumn(); ImGui::TextDisabled("%s", wd[i]); }
            int first = dowSun(vy, vm, 1);
            int dim = daysInMonth(vy, vm);
            int cell = 1;
            for (int idx = 0; idx < 42; idx++) {
                ImGui::TableNextColumn();
                if (idx < first || cell > dim) { ImGui::TextUnformatted(" "); continue; }
                int day = cell++;
                bool isSel = (vy == selY && vm == selM && day == selD);
                ImGui::PushID(idx);
                if (isSel) {
                    ImGui::PushStyleColor(ImGuiCol_Button, Theme::form_accent);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::form_accent);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1,1,1,1));
                } else {
                    styleAccentButtons();
                }
                char lbl[4]; std::snprintf(lbl, sizeof lbl, "%d", day);
                if (ImGui::Button(lbl, ImVec2(26, 22))) { outY = vy; outM = vm; outD = day; picked = true; }
                if (isSel) ImGui::PopStyleColor(3); else unstyleAccentButtons();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();
        return picked;
    }

    static bool ClockPicker(int& h24, int& mn) {
        bool changed = false;
        int h12 = h24 % 12; if (h12 == 0) h12 = 12;
        bool pm = h24 >= 12;
        float rowH = ImGui::GetFontSize() + 8.0f;
        auto field = [&](const char* label, int& val, int lo, int hi) {
            ImGui::PushID(label);
            ImVec2 pos = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 pmax(pos.x + 34, pos.y + rowH);
            dl->AddRectFilled(pos, pmax, kWhite, 2.0f);
            dl->AddRect(pos, pmax, kBorder, 2.0f, 0, 1.0f);
            char b[8]; std::snprintf(b, sizeof b, "%02d", val);
            ImVec2 tsz = ImGui::CalcTextSize(b);
            dl->AddText(ImVec2(pos.x + (34 - tsz.x) * 0.5f, pos.y + (rowH - tsz.y) * 0.5f), kInk, b);
            ImGui::Dummy(ImVec2(34, rowH));
            ImGui::SameLine(0, 2);
            int s = Spinner("sp", rowH);
            if (s) { val += s; if (val > hi) val = lo; if (val < lo) val = hi; changed = true; }
            ImGui::PopID();
        };
        ImGui::AlignTextToFramePadding();
        field("h", h12, 1, 12);
        ImGui::SameLine(); ImGui::AlignTextToFramePadding(); ImGui::Text(":");
        ImGui::SameLine(); field("m", mn, 0, 59);
        ImGui::SameLine();
        styleAccentButtons();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, (rowH - ImGui::GetFontSize()) * 0.5f));
        if (ImGui::Button(pm ? "PM" : "AM", ImVec2(38, rowH))) { pm = !pm; changed = true; }
        ImGui::PopStyleVar();
        unstyleAccentButtons();
        if (changed) h24 = (h12 % 12) + (pm ? 12 : 0);
        return changed;
    }
}

struct ChromeFieldGuard {
    ChromeFieldGuard(const CssStyle& m) {
        float rounding = m.border_radius >= 0.0f ? m.border_radius : 2.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, rounding);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, m.border_width > 0.0f ? m.border_width : 1.0f);
        float pad_x = m.padding_left > 0.0f ? m.padding_left : kFieldPadX;
        float pad_y = m.padding_top  > 0.0f ? m.padding_top  : kFieldPadY;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(pad_x, pad_y));

        ImVec4 bg = m.has_bg ? m.bg_color : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, bg);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, bg);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, bg);

        ImVec4 txt = m.has_color ? m.color : ImVec4(0.08f, 0.08f, 0.08f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, txt);
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImVec4(0.45f, 0.45f, 0.45f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Border, m.has_border_color ? m.border_color : ImVec4(0.46f, 0.46f, 0.46f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_InputTextCursor, ImVec4(0.08f, 0.08f, 0.08f, 1.0f));
    }
    ~ChromeFieldGuard() {
        ImGui::PopStyleColor(7);
        ImGui::PopStyleVar(3);
    }
};

static std::string lower_str(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

static void reset_form_controls(DomNode& n) {
    if (n.tag == "input" || n.tag == "textarea" || n.tag == "select") {
        n.value = n.default_value;
        n.checked = n.default_checked;
    }
    for (auto& c : n.children) reset_form_controls(c);
}

// Gather "name = value" for every successful control, as a browser would build
// a form submission. Radio state lives in the tab's group map.
static void collect_form_data(const DomNode& n, Tab& tab, std::string& out) {
    if (n.tag == "input" && !n.name.empty()) {
        std::string t = lower_str(n.type);
        if (t == "submit" || t == "reset" || t == "button") {
            // not submitted
        } else if (t == "checkbox") {
            if (n.checked) out += n.name + " = " + (n.value.empty() ? "on" : n.value) + "\n";
        } else if (t == "radio") {
            auto it = tab.radio_selection.find(n.name);
            if (it != tab.radio_selection.end() && it->second == (uintptr_t)&n)
                out += n.name + " = " + (n.value.empty() ? "on" : n.value) + "\n";
        } else {
            out += n.name + " = " + n.value + "\n";
        }
    } else if ((n.tag == "textarea" || n.tag == "select") && !n.name.empty()) {
        out += n.name + " = " + n.value + "\n";
    }
    for (auto& c : n.children) collect_form_data(c, tab, out);
}

// Inline text-formatting tags that render their own text_content with decoration.
static bool is_inline_text_tag(const std::string& tag) {
    return tag == "b" || tag == "strong" || tag == "i" || tag == "em" ||
           tag == "u" || tag == "code" || tag == "mark" || tag == "small" ||
           tag == "del" || tag == "s" || tag == "strike" || tag == "ins" ||
           tag == "label" || tag == "cite" || tag == "q" || tag == "abbr" ||
           tag == "kbd" || tag == "samp" || tag == "var" || tag == "sub" || tag == "sup";
}

bool is_inline_element(const DomNode& node, const CssStyle& merged) {
    if (merged.display == "inline" || merged.display == "inline-block") return true;
    if (merged.display == "block") return false;

    if (node.tag == "span" || node.tag == "a" || node.tag == "button" ||
        node.tag == "input" || node.tag == "select" || node.tag == "option") {
        return true;
    }
    if (is_inline_text_tag(node.tag)) return true;
    return false;
}

std::string get_media_source(const DomNode& node) {
    if (!node.src.empty()) {
        return node.src;
    }
    for (const auto& child : node.children) {
        if (child.tag == "source" && !child.src.empty()) {
            return child.src;
        }
    }
    return "";
}

// Build a closed polygon path with rounded corners (Lucide-style rounded joins).
static void BuildRoundedPolyPath(ImDrawList* draw_list, const ImVec2* pts, int n, float r) {
    draw_list->PathClear();
    for (int i = 0; i < n; i++) {
        ImVec2 cur  = pts[i];
        ImVec2 prev = pts[(i + n - 1) % n];
        ImVec2 next = pts[(i + 1) % n];
        ImVec2 d1(prev.x - cur.x, prev.y - cur.y);
        ImVec2 d2(next.x - cur.x, next.y - cur.y);
        float l1 = sqrtf(d1.x * d1.x + d1.y * d1.y);
        float l2 = sqrtf(d2.x * d2.x + d2.y * d2.y);
        if (l1 > 0.0f) { d1.x /= l1; d1.y /= l1; }
        if (l2 > 0.0f) { d2.x /= l2; d2.y /= l2; }
        float rr = std::min(r, std::min(l1, l2) * 0.5f);
        ImVec2 p_in (cur.x + d1.x * rr, cur.y + d1.y * rr);
        ImVec2 p_out(cur.x + d2.x * rr, cur.y + d2.y * rr);
        draw_list->PathLineTo(p_in);
        draw_list->PathBezierQuadraticCurveTo(cur, p_out, 8);
    }
}

void DrawPlayIcon(ImDrawList* draw_list, ImVec2 center, ImU32 color, float size) {
    // Lucide "play": right-pointing triangle outline with rounded corners.
    ImVec2 pts[3] = {
        ImVec2(center.x - size * 0.45f, center.y - size * 0.62f),
        ImVec2(center.x - size * 0.45f, center.y + size * 0.62f),
        ImVec2(center.x + size * 0.72f, center.y),
    };
    BuildRoundedPolyPath(draw_list, pts, 3, size * 0.22f);
    draw_list->PathStroke(color, ImDrawFlags_Closed, std::max(1.4f, size * 0.16f));
}

void DrawPauseIcon(ImDrawList* draw_list, ImVec2 center, ImU32 color, float size) {
    // Lucide "pause": two rounded vertical bar outlines.
    float thickness = std::max(1.4f, size * 0.16f);
    float bar_w = size * 0.42f;
    float bar_h = size * 1.40f;
    float gap   = bar_w * 0.40f;
    float r     = bar_w * 0.24f;
    draw_list->AddRect(ImVec2(center.x - gap - bar_w, center.y - bar_h * 0.5f),
                       ImVec2(center.x - gap,         center.y + bar_h * 0.5f), color, r, 0, thickness);
    draw_list->AddRect(ImVec2(center.x + gap,         center.y - bar_h * 0.5f),
                       ImVec2(center.x + gap + bar_w, center.y + bar_h * 0.5f), color, r, 0, thickness);
}

void DrawSpeakerIcon(ImDrawList* draw_list, ImVec2 center, ImU32 color, float size, bool is_muted) {
    // Lucide "volume-2": rounded speaker outline plus two concentric sound-wave arcs.
    float thickness = std::max(1.4f, size * 0.15f);
    float box_x  = center.x - size * 0.95f;   // left edge of the base box
    float neck_x = center.x - size * 0.45f;   // where box meets the cone
    float cone_x = center.x + size * 0.05f;   // cone's wide (right) opening
    float box_h  = size * 0.40f;              // half-height of neck/box
    float cone_h = size * 0.85f;              // half-height of cone opening

    ImVec2 pts[6] = {
        ImVec2(cone_x, center.y - cone_h),
        ImVec2(neck_x, center.y - box_h),
        ImVec2(box_x,  center.y - box_h),
        ImVec2(box_x,  center.y + box_h),
        ImVec2(neck_x, center.y + box_h),
        ImVec2(cone_x, center.y + cone_h),
    };
    BuildRoundedPolyPath(draw_list, pts, 6, size * 0.16f);
    draw_list->PathStroke(color, ImDrawFlags_Closed, thickness);

    if (is_muted) {
        float x0 = center.x + size * 0.60f;
        float xs = size * 0.40f;
        draw_list->AddLine(ImVec2(x0 - xs, center.y - xs), ImVec2(x0 + xs, center.y + xs), color, thickness);
        draw_list->AddLine(ImVec2(x0 - xs, center.y + xs), ImVec2(x0 + xs, center.y - xs), color, thickness);
    } else {
        ImVec2 arc_c(center.x + size * 0.05f, center.y);
        draw_list->PathClear();
        draw_list->PathArcTo(arc_c, size * 0.50f, -0.60f, 0.60f, 10);
        draw_list->PathStroke(color, 0, thickness);
        draw_list->PathClear();
        draw_list->PathArcTo(arc_c, size * 0.85f, -0.60f, 0.60f, 12);
        draw_list->PathStroke(color, 0, thickness);
    }
}

void DrawFullscreenIcon(ImDrawList* draw_list, ImVec2 center, ImU32 color, float size) {
    float r = size * 0.45f;
    float gap = size * 0.25f;
    float thickness = 1.5f;
    draw_list->AddLine(ImVec2(center.x - r, center.y - r), ImVec2(center.x - r + gap, center.y - r), color, thickness);
    draw_list->AddLine(ImVec2(center.x - r, center.y - r), ImVec2(center.x - r, center.y - r + gap), color, thickness);
    draw_list->AddLine(ImVec2(center.x + r, center.y - r), ImVec2(center.x + r - gap, center.y - r), color, thickness);
    draw_list->AddLine(ImVec2(center.x + r, center.y - r), ImVec2(center.x + r, center.y - r + gap), color, thickness);
    draw_list->AddLine(ImVec2(center.x - r, center.y + r), ImVec2(center.x - r + gap, center.y + r), color, thickness);
    draw_list->AddLine(ImVec2(center.x - r, center.y + r), ImVec2(center.x - r, center.y + r - gap), color, thickness);
    draw_list->AddLine(ImVec2(center.x + r, center.y + r), ImVec2(center.x + r - gap, center.y + r), color, thickness);
    draw_list->AddLine(ImVec2(center.x + r, center.y + r), ImVec2(center.x + r, center.y + r - gap), color, thickness);
}

static const ImU32 kMediaIconColor   = IM_COL32(240, 240, 245, 255);
static const ImU32 kMediaTrackColor  = IM_COL32(120, 120, 120, 200);
static const ImU32 kMediaFillColor   = IM_COL32(255, 255, 255, 255);
// Between the track and the played fill, so buffered reads as "ahead of where you
// are" without competing with the playhead.
static const ImU32 kMediaBufferColor = IM_COL32(200, 200, 205, 190);

// A fully cached entry plays straight off disk; anything else streams. The sidecar's
// absence is what means complete — testing existence alone hands the backend the
// sparse file MediaSource allocates up front, which parses as undecodable.
static VideoPlayer* make_media_player(int tab_id, const std::string& url, bool audio_only) {
    std::string cache_path = get_cache_filepath(url);
    if (std::filesystem::exists(cache_path) &&
        !std::filesystem::exists(cache_path + ".ranges")) {
        return new VideoPlayer(cache_path, audio_only);
    }
    return new VideoPlayer(tab_id, url, audio_only);
}

// Vertical volume popup shown above the speaker button; handles drag and click-away close.

static void DrawVolumePopup(ImDrawList* draw_list, VideoPlayer* player, ImVec2 btn_min, ImVec2 btn_max,
                            ImGuiID open_id, const std::string& id_suffix) {
    float popup_w = 24.0f, popup_h = 80.0f, gap = 4.0f;
    ImVec2 popup_pos(btn_min.x + (btn_max.x - btn_min.x) * 0.5f - popup_w * 0.5f, btn_min.y - popup_h - gap);
    ImVec2 popup_max(popup_pos.x + popup_w, popup_pos.y + popup_h);

    draw_list->AddRectFilled(popup_pos, popup_max, IM_COL32(30, 30, 32, 255), 12.0f);
    draw_list->AddRect(popup_pos, popup_max, IM_COL32(255, 255, 255, 20), 12.0f, 0, 1.0f);

    ImGui::SetCursorScreenPos(popup_pos);
    ImGui::InvisibleButton(("##vol_slider_" + id_suffix).c_str(), ImVec2(popup_w, popup_h));
    if (ImGui::IsItemActive()) {
        float pct = ((popup_pos.y + popup_h - 10.0f) - ImGui::GetIO().MousePos.y) / (popup_h - 20.0f);
        pct = std::clamp(pct, 0.0f, 1.0f);
        player->set_volume(pct);
        if (pct > 0.0f && player->is_muted()) player->set_muted(false);
    }

    float vol = player->is_muted() ? 0.0f : player->get_volume();
    float track_x = popup_pos.x + popup_w * 0.5f;
    float track_top = popup_pos.y + 10.0f;
    float track_bottom = popup_pos.y + popup_h - 10.0f;
    float split_y = track_bottom - vol * (track_bottom - track_top);
    draw_list->AddRectFilled(ImVec2(track_x - 1.5f, track_top), ImVec2(track_x + 1.5f, track_bottom), kMediaTrackColor, 2.0f);
    if (vol > 0.0f) draw_list->AddRectFilled(ImVec2(track_x - 1.5f, split_y), ImVec2(track_x + 1.5f, track_bottom), kMediaFillColor, 2.0f);
    draw_list->AddCircleFilled(ImVec2(track_x, split_y), 5.0f, kMediaFillColor);

    if (ImGui::IsMouseClicked(0)) {
        ImVec2 m = ImGui::GetIO().MousePos;
        bool in_popup = m.x >= popup_pos.x && m.x <= popup_max.x && m.y >= popup_pos.y && m.y <= popup_max.y;
        bool in_btn   = m.x >= btn_min.x && m.x <= btn_max.x && m.y >= btn_min.y && m.y <= btn_max.y;
        if (!in_popup && !in_btn) ImGui::GetStateStorage()->SetBool(open_id, false);
    }
}

// Play is anchored to the left and volume to the right with symmetric padding; the timeline
// fills the space between the play button and the time label.
static void DrawMediaControlBar(ImDrawList* draw_list, VideoPlayer* player,
                                ImVec2 bar_min, ImVec2 bar_max, const std::string& id_suffix,
                                float icon_size) {
    const float pad = 12.0f, gap = 10.0f, play_sz = 26.0f, vol_sz = 24.0f;
    float cy = (bar_min.y + bar_max.y) * 0.5f;

    bool playing = player->is_playing();
    double duration = player->get_duration();
    double current_time = player->get_current_time();

    // Time label geometry (computed up front so the timeline can fill the remaining width).
    int dur_m = (int)duration / 60, dur_s = (int)duration % 60;
    char time_buf[32];
    snprintf(time_buf, sizeof(time_buf), "%d:%02d / %d:%02d", (int)current_time / 60, (int)current_time % 60, dur_m, dur_s);
    ImVec2 ts = ImGui::CalcTextSize(time_buf);

    float vol_x  = bar_max.x - pad - vol_sz;
    float time_x = vol_x - gap - ts.x;
    float tl_x   = bar_min.x + pad + play_sz + gap;
    float tl_w   = time_x - gap - tl_x;

    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(255, 255, 255, 25));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(255, 255, 255, 45));

    ImGui::SetCursorScreenPos(ImVec2(bar_min.x + pad, cy - play_sz * 0.5f));
    if (ImGui::Button(("##play_" + id_suffix).c_str(), ImVec2(play_sz, play_sz))) {
        if (playing) player->pause(); else player->play();
    }
    ImVec2 pmin = ImGui::GetItemRectMin(), pmax = ImGui::GetItemRectMax();
    ImVec2 play_c((pmin.x + pmax.x) * 0.5f, (pmin.y + pmax.y) * 0.5f);
    if (playing) DrawPauseIcon(draw_list, play_c, kMediaIconColor, icon_size);
    else         DrawPlayIcon(draw_list, play_c, kMediaIconColor, icon_size);

    ImGuiID vol_open_id = ImGui::GetID((id_suffix + "_vol_open").c_str());
    bool vol_open = ImGui::GetStateStorage()->GetBool(vol_open_id, false);
    ImGui::SetCursorScreenPos(ImVec2(vol_x, cy - vol_sz * 0.5f));
    if (ImGui::Button(("##vol_" + id_suffix).c_str(), ImVec2(vol_sz, vol_sz))) {
        vol_open = !vol_open;
        ImGui::GetStateStorage()->SetBool(vol_open_id, vol_open);
    }
    ImVec2 vmin = ImGui::GetItemRectMin(), vmax = ImGui::GetItemRectMax();
    DrawSpeakerIcon(draw_list, ImVec2((vmin.x + vmax.x) * 0.5f, (vmin.y + vmax.y) * 0.5f),
                    kMediaIconColor, icon_size, player->is_muted());

    ImGui::PopStyleColor(3);

    if (tl_w > 30.0f) {
        ImGui::SetCursorScreenPos(ImVec2(tl_x, cy - 12.0f));
        ImGui::InvisibleButton(("##slider_" + id_suffix).c_str(), ImVec2(tl_w, 24.0f));
        bool active = ImGui::IsItemActive();
        if (active && duration > 0.0) {
            float pct = std::clamp((ImGui::GetIO().MousePos.x - tl_x) / tl_w, 0.0f, 1.0f);
            current_time = pct * duration;
            player->seek(current_time);
        }
        float pct = duration > 0.0f ? (float)(current_time / duration) : 0.0f;
        float split_x = tl_x + pct * tl_w;
        draw_list->AddRectFilled(ImVec2(tl_x, cy - 1.5f), ImVec2(tl_x + tl_w, cy + 1.5f), kMediaTrackColor, 2.0f);

        // What is held locally, under the played fill. With a bounded read-ahead the
        // buffer is a window around the playhead rather than a run from the start, so
        // each span is drawn where it actually sits.
        for (auto [s, e] : player->buffered_spans(1.0 / (double)tl_w)) {
            float bx0 = tl_x + (float)s * tl_w;
            float bx1 = tl_x + (float)e * tl_w;
            if (bx1 - bx0 < 1.0f) bx1 = bx0 + 1.0f;
            draw_list->AddRectFilled(ImVec2(bx0, cy - 1.5f), ImVec2(bx1, cy + 1.5f),
                                     kMediaBufferColor, 2.0f);
        }

        if (pct > 0.0f) draw_list->AddRectFilled(ImVec2(tl_x, cy - 1.5f), ImVec2(split_x, cy + 1.5f), kMediaFillColor, 2.0f);
        draw_list->AddCircleFilled(ImVec2(split_x, cy), active ? 6.0f : 5.0f, kMediaFillColor);
    }

    // Time label (vertically centred, drawn last so it reflects any seek this frame).
    snprintf(time_buf, sizeof(time_buf), "%d:%02d / %d:%02d", (int)current_time / 60, (int)current_time % 60, dur_m, dur_s);
    draw_list->AddText(ImVec2(time_x, cy - ts.y * 0.5f), kMediaIconColor, time_buf);

    if (vol_open) DrawVolumePopup(draw_list, player, vmin, vmax, vol_open_id, id_suffix);
}

static bool is_text_inline_node(const DomNode& node) {
    return node.tag == "#text" || is_inline_text_tag(node.tag);
}

// Collapse internal runs of whitespace to a single space, but preserve a single
// leading/trailing space when present so inline spacing comes from the source text
// itself (e.g. the space in "renders <b>" vs. the lack of one in "H<sub>2</sub>O").
static std::string collapse_inline(const std::string& s, bool trim_leading) {
    std::string r;
    r.reserve(s.size());
    bool last_space = false;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!last_space) { r += ' '; last_space = true; }
        } else {
            r += c;
            last_space = false;
        }
    }
    if (trim_leading && !r.empty() && r.front() == ' ') r.erase(r.begin());
    return r;
}

static float inline_text_width(const DomNode& node, float scale) {
    std::string text = collapse_inline(node.text_content, false);
    if (text.empty()) return 0.0f;
    bool use_mono = (node.tag == "code" || node.tag == "kbd" ||
                     node.tag == "samp" || node.tag == "var") && mono_font != nullptr;
    bool shrink = (node.tag == "small" || node.tag == "sub" || node.tag == "sup");
    if (use_mono) ImGui::PushFont(mono_font);
    if (shrink) ImGui::SetWindowFontScale(scale * 0.8f);
    float w = ImGui::CalcTextSize(text.c_str()).x;
    if (shrink) ImGui::SetWindowFontScale(scale);
    if (use_mono) ImGui::PopFont();
    return w;
}

// Draws one inline text node with its tag decoration, leaving it as the last item so
// the caller can chain the next sibling with SameLine.
static void draw_inline_item(const DomNode& node, const CssStyle& merged, float scale, bool trim_leading) {
    std::string text = collapse_inline(node.text_content, trim_leading);
    if (text.empty()) { ImGui::Dummy(ImVec2(0.0f, 0.0f)); return; }

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const std::string& tag = node.tag;
    bool use_mono = (tag == "code" || tag == "kbd" || tag == "samp" || tag == "var") && mono_font != nullptr;
    bool sub = (tag == "sub"), sup = (tag == "sup");
    bool shrink = (tag == "small" || sub || sup);
    bool italic = (tag == "i" || tag == "em" || tag == "cite" || tag == "var");

    ImVec4 text_col = merged.color;
    if (tag == "mark" && !merged.has_color) text_col = ImVec4(0.1f, 0.1f, 0.1f, 1.0f);
    ImU32 col_u32 = ImGui::ColorConvertFloat4ToU32(text_col);

    if (use_mono) ImGui::PushFont(mono_font);
    if (shrink) ImGui::SetWindowFontScale(scale * 0.8f);

    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 sz = ImGui::CalcTextSize(text.c_str());

    // <sub>/<sup>: shift the (smaller) glyphs down/up off the baseline, then reserve the
    // advance with a Dummy so the next inline sibling chains onto it.
    if (sub || sup) {
        float full_h = sz.y / 0.8f;
        float dy = sup ? -0.12f * full_h : 0.32f * full_h;
        draw_list->AddText(ImVec2(pos.x, pos.y + dy), col_u32, text.c_str());
        if (shrink) ImGui::SetWindowFontScale(scale);
        if (use_mono) ImGui::PopFont();
        ImGui::Dummy(ImVec2(sz.x, full_h));
        return;
    }

    if (tag == "mark" || tag == "code" || tag == "kbd") {
        ImU32 chip = tag == "mark" ? IM_COL32(255, 235, 100, 235) : IM_COL32(255, 255, 255, 22);
        float px = 2.0f, py = 1.0f;
        draw_list->AddRectFilled(ImVec2(pos.x - px, pos.y - py),
                                 ImVec2(pos.x + sz.x + px, pos.y + sz.y + py), chip, 3.0f);
        if (tag == "kbd") {
            draw_list->AddRect(ImVec2(pos.x - px, pos.y - py),
                               ImVec2(pos.x + sz.x + px, pos.y + sz.y + py),
                               IM_COL32(255, 255, 255, 60), 3.0f, 0, 1.0f);
        }
    }

    int vtx_start = draw_list->VtxBuffer.Size;
    ImGui::TextColored(text_col, "%s", text.c_str());
    ImVec2 r_min = ImGui::GetItemRectMin();
    ImVec2 r_max = ImGui::GetItemRectMax();

    // Faux-italic: skew the glyph vertices we just emitted (top shifts right of bottom).
    if (italic) {
        for (int v = vtx_start; v < draw_list->VtxBuffer.Size; ++v) {
            ImDrawVert& vert = draw_list->VtxBuffer[v];
            vert.pos.x += (r_max.y - vert.pos.y) * 0.22f;
        }
    }

    // Faux-bold: overdraw at a couple of sub-pixel offsets to thicken the strokes.
    if (tag == "b" || tag == "strong") {
        draw_list->AddText(ImVec2(r_min.x + 0.8f, r_min.y), col_u32, text.c_str());
        draw_list->AddText(ImVec2(r_min.x + 0.4f, r_min.y + 0.4f), col_u32, text.c_str());
    }
    if (tag == "u" || tag == "ins") {
        draw_list->AddLine(ImVec2(r_min.x, r_max.y - 1.0f), ImVec2(r_max.x, r_max.y - 1.0f), col_u32, 1.0f);
    }
    if (tag == "del" || tag == "s" || tag == "strike") {
        float mid = (r_min.y + r_max.y) * 0.5f;
        draw_list->AddLine(ImVec2(r_min.x, mid), ImVec2(r_max.x, mid), col_u32, 1.0f);
    }

    if (shrink) ImGui::SetWindowFontScale(scale);
    if (use_mono) ImGui::PopFont();
}

// Inline flow: consecutive inline nodes share a line and wrap; block children break the
// line and recurse. Keeps mixed content like <p>text <b>bold</b></p> in document order.
void render_flow_children(DomNode& parent, const CssStyle& merged, Tab& tab,
                          float right_offset, float scale) {
    float wrap_right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    if (right_offset > 0.0f) wrap_right -= right_offset;

    bool prev_inline = false;
    for (size_t idx = 0; idx < parent.children.size(); ++idx) {
        DomNode& child = parent.children[idx];
        if (child.tag == "script" || child.tag == "style" || child.tag == "head" ||
            child.tag == "title" || child.tag == "meta" || child.tag == "option") {
            continue;
        }

        bool text_inline = is_text_inline_node(child);
        // Inline by tag, unless the element asks to be a block. render_node's own
        // is_inline_element already honours `display`, so deciding it from the tag
        // alone here would lay a block-level widget out on the inline path and
        // charge it the inter-item spacing below.
        bool complex_inline = (child.tag == "a" || child.tag == "span" || child.tag == "button" ||
                               child.tag == "input" || child.tag == "select") &&
                              merge_node_style(child, merged, tab).display != "block";
        bool inl = text_inline || complex_inline;

        if (inl) {
            if (prev_inline) {
                float last_x2 = ImGui::GetItemRectMax().x;
                float w = text_inline ? inline_text_width(child, scale)
                                      : ImGui::CalcTextSize(collapse_whitespace(child.text_content).c_str()).x + 20.0f;
                // Inter-item spacing comes from the source text's own spaces (preserved by
                // collapse_inline), so items butt together; only complex inline widgets get
                // a small gap. Stay on this line only if it still fits, else wrap.
                float spacing = text_inline ? 0.0f : 4.0f;
                if (w <= 0.0f || last_x2 + spacing + w <= wrap_right) {
                    ImGui::SameLine(0, spacing);
                }
            }
            if (text_inline) {
                draw_inline_item(child, merged, scale, !prev_inline);
            } else {
                bool child_flow = false;
                render_node(child, merged, child_flow, tab, -1, right_offset);
            }
            prev_inline = true;
        } else {
            prev_inline = false;
            bool child_flow = false;
            render_node(child, merged, child_flow, tab, -1, right_offset);
        }
    }
}

CssStyle merge_node_style(const DomNode& node, const CssStyle& parent_style, Tab& tab) {
    CssStyle merged;
    if (parent_style.has_color) {
        merged.color = parent_style.color;
        merged.has_color = true;
    }
    merged.font_size = parent_style.font_size;
    merged.text_align = parent_style.text_align;
    auto tag_it = tab.css_classes.find(node.tag);
    if (tag_it != tab.css_classes.end()) {
        apply_style(merged, tag_it->second);
    }
    if (!node.class_name.empty()) {
        auto class_it = tab.css_classes.find("." + node.class_name);
        if (class_it != tab.css_classes.end()) {
            apply_style(merged, class_it->second);
        }
    }
    if (node.has_inline_style) {
        apply_style(merged, node.parsed_inline_style);
    }
    // Fold vw/vh down to pixels here, so layout and painting only ever see the
    // pixel fields. A zero viewport (before the first frame) leaves them unset.
    if (merged.width_vw > -1.0f && page_viewport_w > 0.0f) {
        merged.width = merged.width_vw * 0.01f * page_viewport_w;
    }
    if (merged.height_vh > -1.0f && page_viewport_h > 0.0f) {
        merged.height = merged.height_vh * 0.01f * page_viewport_h;
        tab.vp_fit_used = true;  // opt into the slack convergence, as canvas does
    }
    return merged;
}

// Positions a flex container's children at the rects Yoga computed, then reserves the
// container's box so the document flow resumes below it. Its own padding/margin/bg is
// handled by render_node's shared code around this call.
void render_flex_container(DomNode& node, const CssStyle& merged, Tab& tab, float right_offset) {
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::SetWindowFontScale(1.0f); // measure children in absolute pixels

    float content_w = merged.width > 0.0f ? merged.width
                                          : ImGui::GetContentRegionAvail().x - right_offset;
    if (content_w < 1.0f) content_w = 1.0f;

    std::vector<DomNode*> kids;
    std::vector<FlexRect> rects;
    float total_w = 0.0f, total_h = 0.0f;
    compute_flex_layout(node, merged, content_w, tab, kids, rects, total_w, total_h);

    for (size_t i = 0; i < kids.size(); ++i) {
        const FlexRect& r = rects[i];
        CssStyle cm = merge_node_style(*kids[i], merged, tab);
        // Yoga's rect sits past the child's margin, which render_node re-applies, so
        // anchor at the margin-box origin to avoid doubling it.
        ImGui::SetCursorScreenPos(ImVec2(origin.x + r.x - cm.margin_left,
                                         origin.y + r.y - cm.margin_top));
        float child_right = std::max(0.0f, ImGui::GetContentRegionAvail().x - r.w);
        bool child_flow = false;
        render_node(*kids[i], merged, child_flow, tab, -1, child_right);
    }

    ImGui::SetWindowFontScale(merged.font_size);
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(merged.width > 0.0f ? merged.width : total_w, total_h));
}

void render_node(DomNode& node, const CssStyle& parent_style, bool& is_inline_flow, Tab& tab, int li_index, float parent_accumulated_right) {
    if (node.tag == "script" || node.tag == "style" || node.tag == "head" || node.tag == "title" || node.tag == "meta" || node.tag == "option") {
        return;
    }
    if (node.tag == "#text") {
        std::string cleaned = collapse_whitespace(node.text_content);
        if (!cleaned.empty()) {
            ImGui::TextColored(parent_style.color, "%s", cleaned.c_str());
        }
        return;
    }

    CssStyle merged = merge_node_style(node, parent_style, tab);

    bool is_inline = is_inline_element(node, merged);
    if (is_inline) {
        if (is_inline_flow) {
            ImGui::SameLine(0, 8.0f + merged.margin_left);
        }
        is_inline_flow = true;
    } else {
        is_inline_flow = false;
    }

    bool draw_bg = (merged.has_bg || merged.has_gradient || (merged.border_width > 0.0f)) &&
                   (node.tag != "input" && node.tag != "textarea" && node.tag != "select" && node.tag != "button" && node.tag != "a");
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImDrawListSplitter splitter;
    ImVec2 start_pos = ImGui::GetCursorScreenPos();
    ImVec2 content_start = start_pos;
    float block_avail_w = -1.0f;

    float base_font_scale = merged.font_size;
    if (node.tag == "h1") base_font_scale *= 1.8f;
    else if (node.tag == "h2") base_font_scale *= 1.4f;
    else if (node.tag == "h3") base_font_scale *= 1.2f;
    else if (node.tag == "h4") base_font_scale *= 1.1f;
    else if (node.tag == "h5") base_font_scale *= 1.0f;
    else if (node.tag == "h6") base_font_scale *= 0.9f;

    if (base_font_scale != 1.0f) {
        ImGui::SetWindowFontScale(base_font_scale);
    }

    if (draw_bg) {
        if (!is_inline_flow && merged.margin_top > 0.0f) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + merged.margin_top);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + merged.margin_left);
        
        content_start = ImGui::GetCursorScreenPos();

        // A block-level box spans its container, as CSS has it. The group's
        // extent measured after the children only covers the widest one, which
        // would shrink a card down to its longest line of text — and leave two
        // cards holding different content sitting at different widths.
        if (!is_inline) {
            block_avail_w = ImGui::GetContentRegionAvail().x
                          - (parent_accumulated_right + merged.margin_right);
        }

        if (!is_inline_flow && merged.padding_top > 0.0f) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + merged.padding_top);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + merged.padding_left);
        
        splitter.Split(draw_list, 2);
        splitter.SetCurrentChannel(draw_list, 1);
    } else {
        bool is_widget = (node.tag == "input" || node.tag == "textarea" || node.tag == "select" || node.tag == "button");
        if (!is_inline_flow && merged.margin_top > 0.0f) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + merged.margin_top);
        if (merged.margin_left > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + merged.margin_left);
        if (!is_widget && !is_inline_flow && merged.padding_top > 0.0f) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + merged.padding_top);
        if (!is_widget && merged.padding_left > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + merged.padding_left);
    }

    ImGui::BeginGroup();

    float child_accumulated_right = parent_accumulated_right + merged.margin_right + merged.padding_right;
    if (merged.display == "flex" && !node.children.empty()) {
        render_flex_container(node, merged, tab, child_accumulated_right);
    } else if (node.tag == "div") {
        render_flow_children(node, merged, tab, child_accumulated_right, base_font_scale);
    } else if (node.tag == "ol") {
        int index = 1;
        bool child_inline_flow = false;
        for (auto& child : node.children) {
            if (child.tag == "li") {
                render_node(child, merged, child_inline_flow, tab, index++, child_accumulated_right);
            } else {
                render_node(child, merged, child_inline_flow, tab, -1, child_accumulated_right);
            }
        }
    } else if (node.tag == "ul") {
        bool child_inline_flow = false;
        for (auto& child : node.children) {
            render_node(child, merged, child_inline_flow, tab, -1, child_accumulated_right);
        }
    } else if (node.tag == "li") {
        std::string cleaned_text = collapse_whitespace(node.text_content);
        if (li_index >= 0) {
            ImGui::TextColored(merged.color, "%d. %s", li_index, cleaned_text.c_str());
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, merged.color);
            ImGui::BulletText("%s", cleaned_text.c_str());
            ImGui::PopStyleColor();
        }
    } else if (node.tag == "h1" || node.tag == "h2" || node.tag == "h3" || node.tag == "h4" || node.tag == "h5" || node.tag == "h6" || node.tag == "p" || node.tag == "span") {
        // pure text uses the fast aligned path; mixed content goes through inline flow
        bool has_elem_child = false;
        for (auto& child : node.children) {
            if (child.tag != "#text") { has_elem_child = true; break; }
        }

        if (!has_elem_child) {
            std::string cleaned_text = collapse_whitespace(node.text_content);
            if (!cleaned_text.empty()) {
                float right_offset = parent_accumulated_right + merged.margin_right + merged.padding_right;
                if (merged.text_align == "center") {
                    float text_width = ImGui::CalcTextSize(cleaned_text.c_str()).x;
                    float avail_width = merged.width > 0.0f ? merged.width : (ImGui::GetContentRegionAvail().x - right_offset);
                    if (avail_width < 0.0f) avail_width = 0.0f;
                    float offset = (avail_width - text_width) * 0.5f;
                    if (offset > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
                } else if (merged.text_align == "right") {
                    float text_width = ImGui::CalcTextSize(cleaned_text.c_str()).x;
                    float avail_width = merged.width > 0.0f ? merged.width : (ImGui::GetContentRegionAvail().x - right_offset);
                    if (avail_width < 0.0f) avail_width = 0.0f;
                    float offset = avail_width - text_width;
                    if (offset > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
                }

                float wrap_width = merged.width > 0.0f ? merged.width : (ImGui::GetContentRegionAvail().x - right_offset);
                if (wrap_width < 0.0f) wrap_width = 0.0f;
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_width);

                if (node.tag == "span") {
                    ImGui::TextColored(merged.color, "%s", cleaned_text.c_str());
                } else {
                    ImGui::TextColored(merged.color, "%s", cleaned_text.c_str());
                    ImGui::Spacing();
                }

                ImGui::PopTextWrapPos();
            }
        } else {
            render_flow_children(node, merged, tab, child_accumulated_right, base_font_scale);
            if (node.tag != "span") ImGui::Spacing();
        }
    } else if (is_inline_text_tag(node.tag)) {
        // inline tag rendered on its own (e.g. inside a table cell)
        draw_inline_item(node, merged, base_font_scale, true);
        bool child_inline_flow = true;
        for (auto& child : node.children) {
            if (child.tag == "#text") continue;
            render_node(child, merged, child_inline_flow, tab, -1, child_accumulated_right);
        }
    } else if (node.tag == "blockquote") {
        ImVec2 bq_start = ImGui::GetCursorScreenPos();
        float indent = 14.0f;
        ImGui::Indent(indent);

        CssStyle quote_style = merged;
        if (!quote_style.has_color) quote_style.color = ImVec4(0.75f, 0.75f, 0.80f, 1.0f);
        render_flow_children(node, quote_style, tab, child_accumulated_right + indent, base_font_scale);

        ImGui::Unindent(indent);
        float bar_bottom = ImGui::GetItemRectMax().y;
        draw_list->AddRectFilled(ImVec2(bq_start.x, bq_start.y),
                                 ImVec2(bq_start.x + 3.0f, bar_bottom),
                                 IM_COL32(130, 130, 145, 200), 1.5f);
        ImGui::Spacing();
    } else if (node.tag == "table") {
        // Collect rows from direct <tr> children as well as those nested in
        // <thead>/<tbody>/<tfoot> section wrappers.
        std::vector<DomNode*> rows;
        for (auto& child : node.children) {
            if (child.tag == "tr") {
                rows.push_back(&child);
            } else if (child.tag == "thead" || child.tag == "tbody" || child.tag == "tfoot") {
                for (auto& sub : child.children) {
                    if (sub.tag == "tr") rows.push_back(&sub);
                }
            }
        }

        int col_count = 0;
        for (DomNode* row : rows) {
            int cells = 0;
            for (auto& cell : row->children) {
                if (cell.tag == "td" || cell.tag == "th") cells++;
            }
            col_count = std::max(col_count, cells);
        }

        if (col_count > 0 && !rows.empty()) {
            std::string table_id = "##table_" + std::to_string((uintptr_t)&node);
            ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                    ImGuiTableFlags_SizingStretchProp;
            if (ImGui::BeginTable(table_id.c_str(), col_count, flags)) {
                for (DomNode* row : rows) {
                    ImGui::TableNextRow();
                    int col = 0;
                    for (auto& cell : row->children) {
                        if (cell.tag != "td" && cell.tag != "th") continue;
                        if (col >= col_count) break;
                        ImGui::TableSetColumnIndex(col++);
                        bool cell_inline = false;
                        std::string cell_text = collapse_whitespace(cell.text_content);
                        if (!cell_text.empty()) {
                            ImVec4 cell_col = merged.has_color ? merged.color : ImVec4(0.92f, 0.92f, 0.95f, 1.0f);
                            if (cell.has_inline_style && cell.parsed_inline_style.has_color) {
                                cell_col = cell.parsed_inline_style.color;
                            }
                            ImVec2 cpos = ImGui::GetCursorScreenPos();
                            ImGui::TextColored(cell_col, "%s", cell_text.c_str());
                            if (cell.tag == "th") {
                                // Faux-bold overdraw to make headers read bolder than body cells.
                                draw_list->AddText(ImVec2(cpos.x + 0.6f, cpos.y),
                                                   ImGui::ColorConvertFloat4ToU32(cell_col), cell_text.c_str());
                            }
                        }
                        for (auto& sub : cell.children) {
                            if (sub.tag == "#text") continue;
                            render_node(sub, merged, cell_inline, tab, -1, 0.0f);
                        }
                    }
                }
                ImGui::EndTable();
            }
        }
        ImGui::Spacing();
    } else if (node.tag == "pre") {
        if (!node.text_content.empty()) {
            float right_offset = parent_accumulated_right + merged.margin_right + merged.padding_right;
            float wrap_width = merged.width > 0.0f ? merged.width : (ImGui::GetContentRegionAvail().x - right_offset);
            if (wrap_width < 0.0f) wrap_width = 0.0f;
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_width);
            
            bool pushed_font = false;
            if (mono_font != nullptr) {
                ImGui::PushFont(mono_font);
                pushed_font = true;
            }
            
            ImGui::TextColored(merged.color, "%s", node.text_content.c_str());
            ImGui::Spacing();
            
            if (pushed_font) {
                ImGui::PopFont();
            }
            ImGui::PopTextWrapPos();
        }
        
        bool child_inline_flow = !node.text_content.empty();
        for (auto& child : node.children) {
            if (child.tag == "#text") continue;
            render_node(child, merged, child_inline_flow, tab, -1, child_accumulated_right);
        }
    } else if (node.tag == "button") {
        std::string cleaned_text = collapse_whitespace(node.text_content);
        float btn_width = merged.width > 0.0f ? merged.width : (ImGui::CalcTextSize(cleaned_text.c_str()).x + 36.0f);
        float btn_height = merged.height > 0.0f ? merged.height : 0.0f;
        
        ImVec4 btn_bg = merged.has_bg ? merged.bg_color : ImVec4(0.53f, 0.34f, 0.84f, 0.70f);
        ImVec4 btn_text = merged.has_color ? merged.color : ImVec4(0.95f, 0.95f, 0.95f, 1.0f);
        
        ImGui::PushStyleColor(ImGuiCol_Button, btn_bg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(btn_bg.x * 0.95f, btn_bg.y * 0.95f, btn_bg.z * 0.95f, btn_bg.w));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(btn_bg.x * 0.9f, btn_bg.y * 0.9f, btn_bg.z * 0.9f, btn_bg.w));
        ImGui::PushStyleColor(ImGuiCol_Text, btn_text);
        
        float rounding = merged.border_radius >= 0.0f ? merged.border_radius : 0.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, rounding);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, merged.border_width >= 0.0f ? merged.border_width : 0.0f);
        
        ImGui::PushStyleColor(ImGuiCol_Border, merged.has_border_color ? merged.border_color : ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
        
        std::string btn_id = cleaned_text + "##" + (node.id.empty() ? std::to_string((uintptr_t)&node) : node.id);
        if (ImGui::Button(btn_id.c_str(), ImVec2(btn_width, btn_height))) {
            script_dispatch_click(tab.id, node.node_id);
        }
        
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(4);
    } else if (node.tag == "img") {
        std::string absolute_src = node.src;
        if (absolute_src.find("://") == std::string::npos) {
            absolute_src = resolve_url(tab.current_url, node.src);
        }
        
        auto tex_it = tab.page_textures.find(absolute_src);
        if (tex_it != tab.page_textures.end() && tex_it->second.id != 0) {
            const auto& tex = tex_it->second;
            
            float w = merged.width > 0.0f ? merged.width : (float)tex.width;
            float h = merged.height > 0.0f ? merged.height : (float)tex.height;
            
            float avail_width = ImGui::GetContentRegionAvail().x - (parent_accumulated_right + merged.margin_right + merged.padding_right);
            if (avail_width < 0.0f) avail_width = 0.0f;
            if (w > avail_width && avail_width > 0.0f) {
                float ratio = h / w;
                w = avail_width;
                h = w * ratio;
            }
            
            ImGui::Image((void*)(intptr_t)tex.id, ImVec2(w, h));
        } else {
            ImGui::Button("[Image Missing]", ImVec2(100.0f, 100.0f));
        }
    } else if (node.tag == "a") {
        std::string cleaned_text = collapse_whitespace(node.text_content);
        
        ImVec4 link_color = ImVec4(0.0f, 0.0f, 238.0f / 255.0f, 1.0f);
        auto tag_rule = tab.css_classes.find("a");
        if (tag_rule != tab.css_classes.end() && tag_rule->second.has_color) {
            link_color = tag_rule->second.color;
        }
        if (!node.class_name.empty()) {
            auto class_rule = tab.css_classes.find("." + node.class_name);
            if (class_rule != tab.css_classes.end() && class_rule->second.has_color) {
                link_color = class_rule->second.color;
            }
        }
        if (node.has_inline_style && node.parsed_inline_style.has_color) {
            link_color = node.parsed_inline_style.color;
        }
        
        ImGui::PushStyleColor(ImGuiCol_Text, link_color);
        ImGui::Text("%s", cleaned_text.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImVec2 min_pos = ImGui::GetItemRectMin();
            ImVec2 max_pos = ImGui::GetItemRectMax();
            min_pos.y = max_pos.y;
            ImGui::GetWindowDrawList()->AddLine(min_pos, max_pos, ImGui::ColorConvertFloat4ToU32(link_color));
            
            if (ImGui::IsItemClicked()) {
                std::string new_url = resolve_url(tab.current_url, node.href);
                start_async_fetch(tab.id, new_url);
            }
        }
        ImGui::PopStyleColor();
    } else if (node.tag == "canvas") {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float w = merged.width  > 0.0f ? merged.width  : avail.x;
        float h = merged.height > 0.0f ? merged.height : avail.y;
        if (w < 1.0f) w = 300.0f;
        if (h < 1.0f) h = 150.0f;

        float reserve_h = h;
        if (merged.height <= 0.0f) {
            reserve_h = avail.y - tab.vp_slack;
            if (reserve_h < 1.0f) reserve_h = 1.0f;
            tab.vp_fit_used = true;
        }

        script_set_canvas_size(tab.id, node.node_id, w, h);
        ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(w, reserve_h));

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 br(o.x + w, o.y + h);
        dl->PushClipRect(o, br, true);

        const std::vector<CanvasOp>* ops = script_canvas_ops(tab.id, node.node_id);
        if (ops) {
            for (const CanvasOp& op : *ops) {
                ImU32 col = ImGui::GetColorU32(op.color);
                switch (op.kind) {
                    case CanvasOp::FillRect:
                        dl->AddRectFilled(ImVec2(o.x + op.a, o.y + op.b),
                                          ImVec2(o.x + op.a + op.c, o.y + op.b + op.d), col);
                        break;
                    case CanvasOp::StrokeRect:
                        dl->AddRect(ImVec2(o.x + op.a, o.y + op.b),
                                    ImVec2(o.x + op.a + op.c, o.y + op.b + op.d),
                                    col, 0.0f, 0, op.line_width);
                        break;
                    case CanvasOp::Line:
                        dl->AddLine(ImVec2(o.x + op.a, o.y + op.b),
                                    ImVec2(o.x + op.c, o.y + op.d), col, op.line_width);
                        break;
                    case CanvasOp::Circle:
                        dl->AddCircle(ImVec2(o.x + op.a, o.y + op.b), op.c, col, 0, op.line_width);
                        break;
                    case CanvasOp::Text:
                        dl->AddText(ImVec2(o.x + op.a, o.y + op.b), col, op.text.c_str());
                        break;
                    case CanvasOp::PolyFill: {
                        std::vector<ImVec2> pts;
                        pts.reserve(op.pts.size());
                        for (const ImVec2& p : op.pts) pts.push_back(ImVec2(o.x + p.x, o.y + p.y));
                        dl->AddConvexPolyFilled(pts.data(), (int)pts.size(), col);
                        break;
                    }
                }
            }
        }
        dl->PopClipRect();
    } else if (node.tag == "hr") {
        ImGui::Separator();
        ImGui::Spacing();
    } else if (node.tag == "input") {
        std::string type = lower_str(node.type);
        std::string uid = node.id.empty() ? std::to_string((uintptr_t)&node) : node.id;
        std::string input_label = "##" + uid;

        // Remember the parsed value so a <input type="reset"> can restore it.
        if (!node.defaults_captured) {
            node.default_value = node.value;
            node.default_checked = node.checked;
            node.defaults_captured = true;
        }

        if (type == "hidden") {
            // Not rendered, matching Chrome.
        } else if (type == "checkbox") {
            if (Chrome::Checkbox(input_label, &node.checked)) {
                node.value = node.checked ? "on" : "";
            }
        } else if (type == "radio") {
            uintptr_t self = (uintptr_t)&node;
            if (!node.name.empty()) {
                if (tab.radio_selection.find(node.name) == tab.radio_selection.end() && node.checked) {
                    tab.radio_selection[node.name] = self;
                }
                auto it = tab.radio_selection.find(node.name);
                bool active = (it != tab.radio_selection.end() && it->second == self);
                if (Chrome::Radio(input_label, active)) {
                    tab.radio_selection[node.name] = self;
                }
            } else {
                if (Chrome::Radio(input_label, node.checked)) {
                    node.checked = !node.checked;
                }
            }
        } else if (type == "submit" || type == "reset" || type == "button") {
            std::string label = node.value;
            if (label.empty()) {
                label = (type == "submit") ? "Submit" : (type == "reset") ? "Reset" : "Button";
            }
            float w = merged.width  > 0.0f ? merged.width  : 0.0f;
            float h = merged.height > 0.0f ? merged.height : 0.0f;
            if (Chrome::Button(label, input_label, ImVec2(w, h))) {
                if (type == "reset") {
                    reset_form_controls(tab.page_dom);
                    tab.radio_selection.clear();
                } else if (type == "submit") {
                    std::string data;
                    collect_form_data(tab.page_dom, tab, data);
                    tab.alert_text = data.empty() ? "Form submitted (no named fields)."
                                                  : "Form submitted:\n" + data;
                    tab.show_alert = true;
                } else {
                    script_dispatch_click(tab.id, node.node_id);
                }
            }
        } else if (type == "file") {
            bool open = Chrome::Button("Choose File", input_label + "_btn", ImVec2(0, 0));
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78f, 0.78f, 0.78f, 1.0f));
            ImGui::TextUnformatted(node.value.empty()
                ? "No file chosen"
                : std::filesystem::path(node.value).filename().string().c_str());
            ImGui::PopStyleColor();
            if (open) {
#ifdef __APPLE__
                std::string picked = PlatformOpenFileDialog();
                if (!picked.empty()) node.value = picked;
#else
                // Fallback for platforms without a native dialog binding.
                static std::unordered_map<std::string, std::string> file_dirs;
                std::string pop = "filepop##" + uid;
                if (file_dirs[uid].empty()) {
                    std::error_code ec;
                    file_dirs[uid] = std::filesystem::current_path(ec).string();
                }
                ImGui::OpenPopup(pop.c_str());
#endif
            }
#ifndef __APPLE__
            static std::unordered_map<std::string, std::string> file_dirs;
            std::string pop = "filepop##" + uid;
            if (ImGui::BeginPopup(pop.c_str())) {
                std::string& dir = file_dirs[uid];
                ImGui::TextDisabled("%s", dir.c_str());
                ImGui::Separator();
                ImGui::BeginChild("flist", ImVec2(380, 260));
                if (ImGui::Selectable(".. (up)")) {
                    std::filesystem::path p(dir);
                    if (p.has_parent_path()) dir = p.parent_path().string();
                }
                std::error_code ec;
                std::vector<std::filesystem::directory_entry> entries;
                for (auto& e : std::filesystem::directory_iterator(dir, ec)) entries.push_back(e);
                std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
                    std::error_code e1, e2;
                    bool da = a.is_directory(e1), db = b.is_directory(e2);
                    if (da != db) return da;
                    return a.path().filename().string() < b.path().filename().string();
                });
                for (auto& e : entries) {
                    bool isdir = e.is_directory(ec);
                    std::string name = e.path().filename().string();
                    if (isdir) name += "/";
                    if (ImGui::Selectable(name.c_str())) {
                        if (isdir) dir = e.path().string();
                        else { node.value = e.path().string(); ImGui::CloseCurrentPopup(); }
                    }
                }
                ImGui::EndChild();
                ImGui::EndPopup();
            }
#endif
        } else if (type == "range") {
            float lo   = node.min_val.empty()  ? 0.0f   : std::strtof(node.min_val.c_str(), nullptr);
            float hi   = node.max_val.empty()  ? 100.0f : std::strtof(node.max_val.c_str(), nullptr);
            float step = node.step_val.empty() ? 0.0f   : std::strtof(node.step_val.c_str(), nullptr);
            if (hi <= lo) hi = lo + 1.0f;
            float val  = node.value.empty()    ? (lo + hi) * 0.5f : std::strtof(node.value.c_str(), nullptr);
            if (val < lo) val = lo;
            if (val > hi) val = hi;

            float width = merged.width > 0.0f ? merged.width : 200.0f;
            float rowH = ImGui::GetFontSize() + 8.0f;
            ImGui::PushID(input_label.c_str());
            ImVec2 pos = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("rng", ImVec2(width, rowH));
            bool active = ImGui::IsItemActive();
            bool hovered = ImGui::IsItemHovered();
            float pad = 8.0f, trackH = 4.0f;
            float x0 = pos.x + pad, x1 = pos.x + width - pad;
            float cy = pos.y + rowH * 0.5f;
            if (active && x1 > x0) {
                float t = (ImGui::GetIO().MousePos.x - x0) / (x1 - x0);
                t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
                val = lo + t * (hi - lo);
                if (step > 0.0f) val = lo + std::round((val - lo) / step) * step;
                if (val < lo) val = lo;
                if (val > hi) val = hi;
                char out[32];
                std::snprintf(out, sizeof(out), "%g", val);
                node.value = out;
            }
            float frac = (hi > lo) ? (val - lo) / (hi - lo) : 0.0f;
            float thumbX = x0 + frac * (x1 - x0);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(ImVec2(x0, cy - trackH*0.5f), ImVec2(x1, cy + trackH*0.5f), IM_COL32(200,200,200,255), 2.0f);
            dl->AddRectFilled(ImVec2(x0, cy - trackH*0.5f), ImVec2(thumbX, cy + trackH*0.5f),
                              ImGui::ColorConvertFloat4ToU32(Theme::form_accent), 2.0f);
            dl->AddCircleFilled(ImVec2(thumbX, cy), (active || hovered) ? 8.0f : 7.0f,
                                ImGui::ColorConvertFloat4ToU32(active ? Theme::form_accent_hover : Theme::form_accent), 20);
            if (active || hovered) ImGui::SetTooltip("%g", val);
            ImGui::PopID();
        } else if (type == "color") {
            float col[3] = {0.0f, 0.0f, 0.0f};
            unsigned int r = 0, g = 0, b = 0;
            const char* hex = node.value.c_str();
            if (node.value.size() >= 7 && node.value[0] == '#' &&
                std::sscanf(hex + 1, "%02x%02x%02x", &r, &g, &b) == 3) {
                col[0] = r / 255.0f; col[1] = g / 255.0f; col[2] = b / 255.0f;
            }
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.46f, 0.46f, 0.46f, 1.0f));
            if (ImGui::ColorEdit3(input_label.c_str(), col,
                    ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
                char out[8];
                std::snprintf(out, sizeof(out), "#%02x%02x%02x",
                    (int)(col[0] * 255.0f + 0.5f),
                    (int)(col[1] * 255.0f + 0.5f),
                    (int)(col[2] * 255.0f + 0.5f));
                node.value = out;
            }
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
        } else if (type == "number") {
            double lo   = node.min_val.empty()  ? -DBL_MAX : std::strtod(node.min_val.c_str(), nullptr);
            double hi   = node.max_val.empty()  ?  DBL_MAX : std::strtod(node.max_val.c_str(), nullptr);
            double step = node.step_val.empty() ?  1.0     : std::strtod(node.step_val.c_str(), nullptr);
            if (step <= 0.0) step = 1.0;

            char buf[64] = {0};
            std::strncpy(buf, node.value.c_str(), sizeof(buf) - 1);

            float total = merged.width > 0.0f ? merged.width : 160.0f;
            ImGui::PushItemWidth(total - 19.0f);
            {
                ChromeFieldGuard style_guard(merged);
                if (ImGui::InputTextWithHint(input_label.c_str(), node.placeholder.c_str(), buf, sizeof(buf),
                        ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_CharsScientific)) {
                    node.value = buf;
                }
            }
            ImGui::PopItemWidth();
            float fieldH = ImGui::GetItemRectSize().y;
            ImGui::SameLine(0, 2);
            int spin = Chrome::Spinner(input_label + "_sp", fieldH);
            if (spin) {
                double cur = node.value.empty() ? 0.0 : std::strtod(node.value.c_str(), nullptr);
                cur += spin * step;
                if (cur < lo) cur = lo;
                if (cur > hi) cur = hi;
                char out[32];
                std::snprintf(out, sizeof(out), "%g", cur);
                node.value = out;
            }
        } else if (type == "date" || type == "datetime-local") {
            bool withTime = (type == "datetime-local");
            int sy=0, sm=0, sd=0, sh=9, smin=0;
            bool has = withTime
                ? (std::sscanf(node.value.c_str(), "%d-%d-%dT%d:%d", &sy,&sm,&sd,&sh,&smin) >= 3)
                : (std::sscanf(node.value.c_str(), "%d-%d-%d", &sy,&sm,&sd) == 3);
            char shown[32];
            if (has) {
                if (withTime) {
                    int h12 = sh % 12; if (h12 == 0) h12 = 12;
                    std::snprintf(shown, sizeof shown, "%02d/%02d/%04d %02d:%02d %s",
                                  sm, sd, sy, h12, smin, sh >= 12 ? "PM" : "AM");
                } else {
                    std::snprintf(shown, sizeof shown, "%02d/%02d/%04d", sm, sd, sy);
                }
            }
            float width = merged.width > 0.0f ? merged.width : (withTime ? 220.0f : 150.0f);
            float rowH = ImGui::GetFontSize() + 8.0f;
            std::string pop = "datepop##" + uid;
            const char* ph = withTime ? "mm/dd/yyyy --:-- --" : "mm/dd/yyyy";
            bool clickedField = Chrome::ValueField(input_label, has ? shown : ph, !has, width - 19.0f);
            ImVec2 fmin = ImGui::GetItemRectMin(), fmax = ImGui::GetItemRectMax();
            ImGui::SameLine(0, 2);
            int spin = Chrome::Spinner(input_label + "_sp", rowH);
            if (spin) {
                int yy = has ? sy : 2026, mm = has ? sm : 1, dd = has ? sd : 1;
                Chrome::addDays(yy, mm, dd, spin);
                char v[32];
                if (withTime) std::snprintf(v, sizeof v, "%04d-%02d-%02dT%02d:%02d", yy, mm, dd, has ? sh : 9, has ? smin : 0);
                else          std::snprintf(v, sizeof v, "%04d-%02d-%02d", yy, mm, dd);
                node.value = v;
            }
            if (clickedField) {
                Chrome::viewYear()[uid] = has ? sy : 2026;
                Chrome::viewMon()[uid]  = has ? sm : 1;
                ImGui::OpenPopup(pop.c_str());
            }
            ImGui::SetNextWindowPos(ImVec2(fmin.x, fmax.y + 2));
            ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(1,1,1,1));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.12f,0.12f,0.12f,1));
            if (ImGui::BeginPopup(pop.c_str())) {
                int oy=sy, om=sm, od=sd;
                bool dchanged = Chrome::CalendarGrid(uid, sy, sm, sd, oy, om, od);
                int hh = has ? sh : sh, mn = has ? smin : smin;
                bool tchanged = false;
                if (withTime) {
                    ImGui::Separator();
                    tchanged = Chrome::ClockPicker(hh, mn);
                }
                if (dchanged || tchanged) {
                    int yy = dchanged ? oy : (has ? sy : 2026);
                    int mm2 = dchanged ? om : (has ? sm : 1);
                    int dd = dchanged ? od : (has ? sd : 1);
                    char v[32];
                    if (withTime) std::snprintf(v, sizeof v, "%04d-%02d-%02dT%02d:%02d", yy, mm2, dd, hh, mn);
                    else          std::snprintf(v, sizeof v, "%04d-%02d-%02d", yy, mm2, dd);
                    node.value = v;
                    if (!withTime) ImGui::CloseCurrentPopup();
                }
                if (withTime) { if (ImGui::Button("Done")) ImGui::CloseCurrentPopup(); }
                ImGui::EndPopup();
            }
            ImGui::PopStyleColor(2);
        } else if (type == "time") {
            int hh=9, mn=0;
            bool has = (std::sscanf(node.value.c_str(), "%d:%d", &hh, &mn) == 2);
            char shown[16];
            if (has) {
                int h12 = hh % 12; if (h12 == 0) h12 = 12;
                std::snprintf(shown, sizeof shown, "%02d:%02d %s", h12, mn, hh >= 12 ? "PM" : "AM");
            }
            float width = merged.width > 0.0f ? merged.width : 130.0f;
            float rowH = ImGui::GetFontSize() + 8.0f;
            std::string pop = "timepop##" + uid;
            bool clickedField = Chrome::ValueField(input_label, has ? shown : "--:-- --", !has, width - 19.0f);
            ImVec2 fmin = ImGui::GetItemRectMin(), fmax = ImGui::GetItemRectMax();
            ImGui::SameLine(0, 2);
            int spin = Chrome::Spinner(input_label + "_sp", rowH);
            if (spin) {
                int total = (has ? hh : 9) * 60 + (has ? mn : 0) + spin;
                total = (total % 1440 + 1440) % 1440;
                char v[8]; std::snprintf(v, sizeof v, "%02d:%02d", total / 60, total % 60);
                node.value = v;
            }
            if (clickedField) ImGui::OpenPopup(pop.c_str());
            ImGui::SetNextWindowPos(ImVec2(fmin.x, fmax.y + 2));
            ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(1,1,1,1));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.12f,0.12f,0.12f,1));
            if (ImGui::BeginPopup(pop.c_str())) {
                if (Chrome::ClockPicker(hh, mn)) {
                    char v[8]; std::snprintf(v, sizeof v, "%02d:%02d", hh, mn);
                    node.value = v;
                }
                ImGui::EndPopup();
            }
            ImGui::PopStyleColor(2);
        } else if (type == "month") {
            int sy=0, sm=0;
            bool has = (std::sscanf(node.value.c_str(), "%d-%d", &sy, &sm) == 2);
            char shown[24];
            if (has) std::snprintf(shown, sizeof shown, "%s %d", Chrome::monthName(sm), sy);
            float width = merged.width > 0.0f ? merged.width : 160.0f;
            float rowH = ImGui::GetFontSize() + 8.0f;
            std::string pop = "monthpop##" + uid;
            static std::unordered_map<std::string,int> monthViewY;
            bool clickedField = Chrome::ValueField(input_label, has ? shown : "Month yyyy", !has, width - 19.0f);
            ImVec2 fmin = ImGui::GetItemRectMin(), fmax = ImGui::GetItemRectMax();
            ImGui::SameLine(0, 2);
            int spin = Chrome::Spinner(input_label + "_sp", rowH);
            if (spin) {
                int yy = has ? sy : 2026, mm = has ? sm : 1;
                mm += spin;
                while (mm < 1)  { mm += 12; yy--; }
                while (mm > 12) { mm -= 12; yy++; }
                char v[16]; std::snprintf(v, sizeof v, "%04d-%02d", yy, mm);
                node.value = v;
            }
            if (clickedField) {
                monthViewY[uid] = has ? sy : 2026;
                ImGui::OpenPopup(pop.c_str());
            }
            ImGui::SetNextWindowPos(ImVec2(fmin.x, fmax.y + 2));
            ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(1,1,1,1));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.12f,0.12f,0.12f,1));
            if (ImGui::BeginPopup(pop.c_str())) {
                int& vy = monthViewY[uid];
                float navBox = ImGui::GetFontSize() + 6.0f;
                if (Chrome::NavArrow("my_prev", false, navBox)) vy--;
                ImGui::SameLine();
                char yl[8]; std::snprintf(yl, sizeof yl, "%d", vy);
                float tw = ImGui::CalcTextSize(yl).x;
                ImGui::SetCursorPosX((ImGui::GetWindowContentRegionMin().x + ImGui::GetWindowContentRegionMax().x - tw) * 0.5f);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(yl);
                ImGui::SameLine();
                ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - navBox);
                if (Chrome::NavArrow("my_next", true, navBox)) vy++;
                for (int m = 1; m <= 12; m++) {
                    bool sel = has && sy == vy && sm == m;
                    if (sel) { ImGui::PushStyleColor(ImGuiCol_Button, Theme::form_accent);
                               ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1,1,1,1)); }
                    else Chrome::styleAccentButtons();
                    if (ImGui::Button((std::string(Chrome::monthName(m)).substr(0,3) + "##m" + std::to_string(m)).c_str(), ImVec2(56, 26))) {
                        char v[16]; std::snprintf(v, sizeof v, "%04d-%02d", vy, m);
                        node.value = v; ImGui::CloseCurrentPopup();
                    }
                    if (sel) ImGui::PopStyleColor(2); else Chrome::unstyleAccentButtons();
                    if (m % 3 != 0) ImGui::SameLine();
                }
                ImGui::EndPopup();
            }
            ImGui::PopStyleColor(2);
        } else if (type == "week") {
            int sy=0, sw=0;
            bool has = (std::sscanf(node.value.c_str(), "%d-W%d", &sy, &sw) == 2);
            char shown[24];
            if (has) std::snprintf(shown, sizeof shown, "Week %02d, %d", sw, sy);
            float width = merged.width > 0.0f ? merged.width : 160.0f;
            float rowH = ImGui::GetFontSize() + 8.0f;
            std::string pop = "weekpop##" + uid;
            static std::unordered_map<std::string,int> weekY, weekW;
            bool clickedField = Chrome::ValueField(input_label, has ? shown : "Week --, yyyy", !has, width - 19.0f);
            ImVec2 fmin = ImGui::GetItemRectMin(), fmax = ImGui::GetItemRectMax();
            ImGui::SameLine(0, 2);
            int spin = Chrome::Spinner(input_label + "_sp", rowH);
            if (spin) {
                int yy = has ? sy : 2026, ww = has ? sw : 1;
                ww += spin;
                if (ww < 1)  { ww = 53; yy--; }
                if (ww > 53) { ww = 1;  yy++; }
                char v[16]; std::snprintf(v, sizeof v, "%04d-W%02d", yy, ww);
                node.value = v;
            }
            if (clickedField) {
                weekY[uid] = has ? sy : 2026;
                weekW[uid] = has ? sw : 1;
                ImGui::OpenPopup(pop.c_str());
            }
            ImGui::SetNextWindowPos(ImVec2(fmin.x, fmax.y + 2));
            ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(1,1,1,1));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.12f,0.12f,0.12f,1));
            if (ImGui::BeginPopup(pop.c_str())) {
                int& vy = weekY[uid]; int& vw = weekW[uid];
                float navBox = ImGui::GetFontSize() + 6.0f;
                ImGui::AlignTextToFramePadding(); ImGui::Text("Year"); ImGui::SameLine();
                if (Chrome::NavArrow("wy_p", false, navBox)) vy--; ImGui::SameLine();
                ImGui::AlignTextToFramePadding(); ImGui::Text("%d", vy); ImGui::SameLine();
                if (Chrome::NavArrow("wy_n", true, navBox)) vy++; ImGui::SameLine();
                ImGui::AlignTextToFramePadding(); ImGui::Text("Week"); ImGui::SameLine();
                if (Chrome::NavArrow("ww_p", false, navBox)) { if (--vw < 1) vw = 53; } ImGui::SameLine();
                ImGui::AlignTextToFramePadding(); ImGui::Text("%02d", vw); ImGui::SameLine();
                if (Chrome::NavArrow("ww_n", true, navBox)) { if (++vw > 53) vw = 1; }
                Chrome::styleAccentButtons();
                if (ImGui::Button("Set##week")) {
                    char v[16]; std::snprintf(v, sizeof v, "%04d-W%02d", vy, vw);
                    node.value = v; ImGui::CloseCurrentPopup();
                }
                Chrome::unstyleAccentButtons();
                ImGui::EndPopup();
            }
            ImGui::PopStyleColor(2);
        } else {
            // Text-like fields: text, password, email, search, url, tel.
            char buf[1024] = {0};
            std::strncpy(buf, node.value.c_str(), sizeof(buf) - 1);

            float width = merged.width > 0.0f ? merged.width : 200.0f;
            ImGui::PushItemWidth(width);

            ImGuiInputTextFlags flags = 0;
            if (type == "password") flags |= ImGuiInputTextFlags_Password;

            std::string hint = node.placeholder;
            if (hint.empty() && type == "email") hint = "name@example.web";
            if (hint.empty() && type == "url")   hint = "star://";

            {
                ChromeFieldGuard style_guard(merged);
                if (ImGui::InputTextWithHint(input_label.c_str(), hint.c_str(), buf, sizeof(buf), flags)) {
                    node.value = buf;
                }
            }
            ImGui::PopItemWidth();
        }
    } else if (node.tag == "textarea") {
        if (!node.defaults_captured) {
            node.default_value = node.value;
            node.defaults_captured = true;
        }
        char buf[4096] = {0};
        std::strncpy(buf, node.value.c_str(), sizeof(buf) - 1);
        
        float width = merged.width > 0.0f ? merged.width : 300.0f;
        float height = merged.height > 0.0f ? merged.height : 100.0f;
        
        std::string label = "##" + (node.id.empty() ? std::to_string((uintptr_t)&node) : node.id);
        
        {
            InputStyleGuard style_guard(merged);
            if (ImGui::InputTextMultiline(label.c_str(), buf, sizeof(buf), ImVec2(width, height))) {
                node.value = buf;
            }
            
            if (node.value.empty() && !node.placeholder.empty()) {
                ImVec2 min_pos = ImGui::GetItemRectMin();
                ImVec2 max_pos = ImGui::GetItemRectMax();
                float border_size = ImGui::GetStyle().FrameBorderSize;
                ImVec2 clip_min = ImVec2(min_pos.x + border_size, min_pos.y + border_size);
                ImVec2 clip_max = ImVec2(max_pos.x - border_size, max_pos.y - border_size);
                ImVec2 text_pos = ImVec2(min_pos.x + ImGui::GetStyle().FramePadding.x, min_pos.y + ImGui::GetStyle().FramePadding.y);
                
                ImGui::PushClipRect(clip_min, clip_max, true);
                ImGui::GetWindowDrawList()->AddText(
                    ImGui::GetFont(),
                    ImGui::GetFontSize(),
                    text_pos,
                    ImGui::GetColorU32(ImGuiCol_TextDisabled),
                    node.placeholder.c_str(),
                    nullptr,
                    width - ImGui::GetStyle().FramePadding.x * 2.0f
                );
                ImGui::PopClipRect();
            }
        }
    } else if (node.tag == "select") {
        std::vector<std::string> options;
        std::vector<std::string> option_vals;
        int current_item = -1;
        
        for (size_t idx = 0; idx < node.children.size(); idx++) {
            if (node.children[idx].tag == "option") {
                std::string opt_text = trim_spaces(node.children[idx].text_content);
                std::string opt_val = node.children[idx].value.empty() ? opt_text : node.children[idx].value;
                options.push_back(opt_text);
                option_vals.push_back(opt_val);
                
                if (node.value == opt_val) {
                    current_item = (int)idx;
                }
            }
        }
        
        if (current_item == -1 && !option_vals.empty()) {
            current_item = 0;
            node.value = option_vals[0];
        }

        // Capture the initial selection so a form reset can restore it.
        if (!node.defaults_captured) {
            node.default_value = node.value;
            node.defaults_captured = true;
        }

        std::string combo_label = "##" + (node.id.empty() ? std::to_string((uintptr_t)&node) : node.id);
        
        std::vector<const char*> items;
        for (const auto& opt : options) {
            items.push_back(opt.c_str());
        }
        
        float width = merged.width > 0.0f ? merged.width : 150.0f;
        ImGui::PushItemWidth(width);
        
        {
            InputStyleGuard style_guard(merged);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.53f, 0.34f, 0.84f, 0.65f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.53f, 0.34f, 0.84f, 0.85f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.43f, 0.24f, 0.74f, 1.00f));
            
            if (!items.empty()) {
                if (ImGui::Combo(combo_label.c_str(), &current_item, items.data(), items.size())) {
                    if (current_item >= 0 && current_item < (int)option_vals.size()) {
                        node.value = option_vals[current_item];
                    }
                }
            }
            ImGui::PopStyleColor(3);
        }
        ImGui::PopItemWidth();
    } else if (node.tag == "video") {
        std::string absolute_src = get_media_source(node);
        if (absolute_src.find("://") == std::string::npos) {
            absolute_src = resolve_url(tab.current_url, absolute_src);
        }
        
        VideoPlayer* player = nullptr;
        auto player_it = tab.active_players.find(absolute_src);
        if (player_it == tab.active_players.end()) {
            player = make_media_player(tab.id, absolute_src, false);
            if (player) {
                if (node.loop) player->set_loop(true);
                if (node.muted) player->set_muted(true);
                if (node.autoplay) {
                    player->play();
                }
                tab.active_players[absolute_src] = player;
            }
        } else {
            player = player_it->second;
        }

        // Sized like <img> is from its texture: the video's own dimensions are the
        // default, and a single specified axis derives the other from them, so the
        // picture keeps its real aspect instead of sitting in a fixed box. The
        // dimensions only exist once a frame has decoded, hence the 16:9 stand-in.
        float nat_w = player ? (float)player->get_width() : 0.0f;
        float nat_h = player ? (float)player->get_height() : 0.0f;
        bool has_natural = nat_w > 0.0f && nat_h > 0.0f;

        float w, h;
        if (merged.width > 0.0f && merged.height > 0.0f) {
            w = merged.width;
            h = merged.height;
        } else if (merged.width > 0.0f) {
            w = merged.width;
            h = has_natural ? w * (nat_h / nat_w) : w * 9.0f / 16.0f;
        } else if (merged.height > 0.0f) {
            h = merged.height;
            w = has_natural ? h * (nat_w / nat_h) : h * 16.0f / 9.0f;
        } else if (has_natural) {
            w = nat_w;
            h = nat_h;
        } else {
            w = 640.0f;
            h = 360.0f;
        }

        float avail_width = ImGui::GetContentRegionAvail().x - (parent_accumulated_right + merged.margin_right + merged.padding_right);
        if (avail_width < 0.0f) avail_width = 0.0f;
        if (w > avail_width && avail_width > 0.0f) {
            float ratio = h / w;
            w = avail_width;
            h = w * ratio;
        }

        std::string id_suffix = "##video_" + std::to_string((uintptr_t)&node);

        if (!player) {
            ImGui::BeginGroup();
            ImGui::Dummy(ImVec2(w, h));
            ImVec2 center = ImVec2((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
                                   (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f);
            DrawSpinner(center, 25.0f, 3.5f, Theme::spinner);
            ImGui::EndGroup();
        } else {
            player->update();
            
            ImGui::BeginGroup();
            ImVec2 video_pos = ImGui::GetCursorScreenPos();
            
            unsigned int tex_id = player->get_texture_id();
            if (tex_id != 0 && player->get_width() > 0 && player->get_height() > 0) {
                ImGui::Image((void*)(intptr_t)tex_id, ImVec2(w, h));
            } else {
                ImDrawList* draw_list = ImGui::GetWindowDrawList();
                ImVec2 video_max = ImVec2(video_pos.x + w, video_pos.y + h);
                draw_list->AddRectFilled(video_pos, video_max, IM_COL32(10, 10, 12, 255));
                ImVec2 center = ImVec2(video_pos.x + w * 0.5f, video_pos.y + h * 0.5f);
                if (player->has_error()) {
                    const char* msg = "This video cannot be decoded";
                    ImVec2 ts = ImGui::CalcTextSize(msg);
                    draw_list->AddText(ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f),
                                       IM_COL32(200, 200, 205, 255), msg);
                } else {
                    DrawSpinner(center, 20.0f, 3.0f, Theme::spinner);
                }
                ImGui::Dummy(ImVec2(w, h));
            }
            
            if (node.controls) {
                ImDrawList* draw_list = ImGui::GetWindowDrawList();
                float control_bar_height = 42.0f;
                ImVec2 bar_min = ImVec2(video_pos.x, video_pos.y + h - control_bar_height);
                ImVec2 bar_max = ImVec2(video_pos.x + w, video_pos.y + h);

                draw_list->AddRectFilled(bar_min, bar_max, IM_COL32(15, 15, 18, 220));
                // Horizontal-only separator so it never spills past the video's sides.
                draw_list->AddLine(bar_min, ImVec2(bar_max.x, bar_min.y), IM_COL32(255, 255, 255, 15), 1.0f);

                DrawMediaControlBar(draw_list, player, bar_min, bar_max, id_suffix, 11.0f);
            }
            ImGui::EndGroup();

            ImGui::SetCursorScreenPos(ImVec2(video_pos.x, video_pos.y + h + 8.0f));
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
        }
    } else if (node.tag == "audio") {
        std::string absolute_src = get_media_source(node);
        if (absolute_src.find("://") == std::string::npos) {
            absolute_src = resolve_url(tab.current_url, absolute_src);
        }
        
        VideoPlayer* player = nullptr;
        auto player_it = tab.active_players.find(absolute_src);
        if (player_it == tab.active_players.end()) {
            player = make_media_player(tab.id, absolute_src, true);
            if (player) {
                if (node.loop) player->set_loop(true);
                if (node.muted) player->set_muted(true);
                if (node.autoplay) {
                    player->play();
                }
                tab.active_players[absolute_src] = player;
            }
        } else {
            player = player_it->second;
        }

        float w = merged.width > 0.0f ? merged.width : 450.0f;
        float h = 42.0f;

        std::string id_suffix = "##audio_" + std::to_string((uintptr_t)&node);

        if (!player) {
            ImGui::BeginGroup();
            ImGui::Button("[Audio Loading...]", ImVec2(w, h));
            ImGui::EndGroup();
        } else {
            player->update();
            
            ImGui::BeginGroup();
            ImVec2 audio_pos = ImGui::GetCursorScreenPos();
            
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            ImVec2 card_max = ImVec2(audio_pos.x + w, audio_pos.y + h);
            
            draw_list->AddRectFilled(audio_pos, card_max, IM_COL32(40, 40, 42, 255), h * 0.5f);
            draw_list->AddRect(audio_pos, card_max, IM_COL32(255, 255, 255, 15), h * 0.5f, 0, 1.0f);
            DrawMediaControlBar(draw_list, player, audio_pos, card_max, id_suffix, 12.0f);
            
            ImGui::EndGroup();

            ImGui::SetCursorScreenPos(ImVec2(audio_pos.x, audio_pos.y + h + 8.0f));
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
        }
    } else {
        render_flow_children(node, merged, tab, child_accumulated_right, base_font_scale);
    }

    ImGui::EndGroup();

    if (draw_bg) {
        ImVec2 min_p = content_start;
        ImVec2 max_p = ImGui::GetItemRectMax();
        
        max_p.x += merged.padding_right;
        max_p.y += merged.padding_bottom;
        
        if (merged.width > 0.0f) max_p.x = min_p.x + merged.width;
        else if (block_avail_w > 0.0f) max_p.x = std::max(max_p.x, min_p.x + block_avail_w);
        if (merged.height > 0.0f) max_p.y = min_p.y + merged.height;
        
        splitter.SetCurrentChannel(draw_list, 0);
        
        float rounding = merged.border_radius;
        if (merged.has_gradient) {
            ImU32 col_start = ImGui::ColorConvertFloat4ToU32(merged.gradient_start);
            ImU32 col_end = ImGui::ColorConvertFloat4ToU32(merged.gradient_end);
            draw_list->AddRectFilledMultiColor(min_p, max_p, col_start, col_start, col_end, col_end);
        } else if (merged.has_bg) {
            draw_list->AddRectFilled(min_p, max_p, ImGui::ColorConvertFloat4ToU32(merged.bg_color), rounding);
        }
        
        if (merged.border_width > 0.0f && merged.has_border_color) {
            draw_list->AddRect(min_p, max_p, ImGui::ColorConvertFloat4ToU32(merged.border_color), rounding, 0, merged.border_width);
        }
        
        splitter.Merge(draw_list);
        
        ImGui::SetCursorScreenPos(ImVec2(start_pos.x, max_p.y + merged.margin_bottom));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
    } else {
        bool is_widget = (node.tag == "input" || node.tag == "textarea" || node.tag == "select" || node.tag == "button");
        if (!is_inline && !is_widget && merged.padding_bottom > 0.0f) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + merged.padding_bottom);
        if (!is_inline && merged.margin_bottom > 0.0f) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + merged.margin_bottom);
        // For inline elements, leave the group as the last item so the next inline
        // sibling can chain onto it with SameLine; a trailing Dummy would break that.
        if (!is_inline) ImGui::Dummy(ImVec2(0.0f, 0.0f));
    }

    if (base_font_scale != 1.0f) {
        ImGui::SetWindowFontScale(1.0f);
    }
}

void DrawSpinner(ImVec2 center, float radius, float thickness, const ImVec4& color) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    int num_segments = 30;
    float start_angle = (float)ImGui::GetTime() * 8.0f;
    float end_angle = start_angle + (3.14159265f * 1.5f);
    draw_list->PathArcTo(center, radius, start_angle, end_angle, num_segments);
    draw_list->PathStroke(ImGui::ColorConvertFloat4ToU32(color), 0, thickness);
}

void DrawBackArrowIcon(ImVec2 center, ImU32 color, float thickness) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddLine(ImVec2(center.x + 7.0f, center.y), ImVec2(center.x - 7.0f, center.y), color, thickness);
    draw_list->PathClear();
    draw_list->PathLineTo(ImVec2(center.x, center.y + 7.0f));
    draw_list->PathLineTo(ImVec2(center.x - 7.0f, center.y));
    draw_list->PathLineTo(ImVec2(center.x, center.y - 7.0f));
    draw_list->PathStroke(color, 0, thickness);
}

void DrawForwardArrowIcon(ImVec2 center, ImU32 color, float thickness) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddLine(ImVec2(center.x - 7.0f, center.y), ImVec2(center.x + 7.0f, center.y), color, thickness);
    draw_list->PathClear();
    draw_list->PathLineTo(ImVec2(center.x, center.y - 7.0f));
    draw_list->PathLineTo(ImVec2(center.x + 7.0f, center.y));
    draw_list->PathLineTo(ImVec2(center.x, center.y + 7.0f));
    draw_list->PathStroke(color, 0, thickness);
}

// Lucide "lock" / "lock-open", in their 24x24 viewBox coordinates.
void DrawLockIcon(ImVec2 center, ImU32 color, bool closed, float size) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const float PI = 3.14159265f;
    const float s = size / 24.0f;
    const float thickness = 2.0f * s;

    // viewBox point -> screen, box centred on `center`.
    auto P = [&](float x, float y) {
        return ImVec2(center.x + (x - 12.0f) * s, center.y + (y - 12.0f) * s);
    };

    draw_list->AddRect(P(3, 11), P(21, 22), color, 2.0f * s, 0, thickness);

    draw_list->PathClear();
    draw_list->PathLineTo(P(7, 11));
    draw_list->PathArcTo(P(12, 7), 5.0f * s, PI, closed ? 2.0f * PI : 1.94f * PI, 20);
    if (closed) draw_list->PathLineTo(P(17, 11));
    draw_list->PathStroke(color, 0, thickness);
}

void DrawReloadIcon(ImVec2 center, float radius, ImU32 color, float thickness) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const float PI = 3.14159265f;
    float s = radius / 9.0f;
    
    draw_list->PathArcTo(center, radius, 0.0f, 1.85f * PI, 32);
    draw_list->PathStroke(color, 0, thickness);
    
    draw_list->PathClear();
    draw_list->PathLineTo(ImVec2(center.x + radius, center.y - radius));
    draw_list->PathLineTo(ImVec2(center.x + radius, center.y - 4.0f * s));
    draw_list->PathLineTo(ImVec2(center.x + 4.0f * s, center.y - 4.0f * s));
    draw_list->PathStroke(color, 0, thickness);
}
