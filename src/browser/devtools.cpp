#include "devtools.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "globals.hpp"
#include "parser.hpp"
#include "renderer.hpp"
#include "script.hpp"
#include "theme.hpp"

namespace devtools {
namespace {

enum class Panel { Elements, Console, Network, Sources, Metrics };

struct LogEntry {
    Level level = Level::Log;
    std::string text;
    std::string source;
    int line = 0;
};

struct NodeBox {
    std::uint64_t node_id;
    ImVec2 min, max;
    // Left, top, right, bottom, as the layout used them. See note_box.
    ImVec4 pad;
};

// Per tab, owned by the render thread. Anything a worker thread produces arrives
// through the pending queues below and is folded in here at the top of a frame.
struct TabState {
    bool open = false;
    Panel panel = Panel::Elements;
    // The tab bar owns the selection, so a panel switch from outside it has to be
    // pushed in with SetSelected on the next frame.
    bool force_panel = false;

    std::vector<LogEntry> logs;
    char filter[128] = "";
    bool show_log = true, show_warn = true, show_error = true;
    bool scroll_to_end = false;

    char input[1024] = "";
    std::vector<std::string> history;
    int history_pos = -1;  // -1 = editing a fresh line
    bool focus_input = false;

    // Elements. Nodes are held by id and re-resolved by walking the tree, never
    // by pointer: the DOM is rebuilt on every load and Tab itself moves.
    std::uint64_t selected = 0;
    std::uint64_t hovered = 0;   // from the tree, this frame
    bool picking = false;
    std::vector<NodeBox> boxes;       // finished last frame, what the picker hits
    std::vector<NodeBox> boxes_next;  // filling this frame
    float tree_split = 0.55f;         // share of the panel the tree gets

    int detail_tab = 0;  // Styles / Computed / Node

    // Edit buffers, refilled whenever the selection changes.
    std::uint64_t edit_for = 0;
    char edit_text[1024] = "";
    char edit_id[128] = "";
    char edit_class[256] = "";
    char edit_style[512] = "";
};

constexpr std::size_t kMaxLogs = 2000;

std::unordered_map<int, TabState> g_tabs;
float g_dock_w = 380.0f;
double g_idle_wait = 0.0;

// Guards the queues only. Producers are fetch workers; the render thread drains
// them once a frame so no panel ever draws with a lock held.
std::mutex g_mux;
struct PendingLog {
    int tab_id;
    LogEntry entry;
};
std::vector<PendingLog> g_pending_logs;

TabState& state(int tab_id) { return g_tabs[tab_id]; }

TabState* find(int tab_id) {
    auto it = g_tabs.find(tab_id);
    return it == g_tabs.end() ? nullptr : &it->second;
}

ImVec4 level_color(Level l) {
    switch (l) {
        case Level::Warn:   return ImVec4(0.95f, 0.78f, 0.35f, 1.0f);
        case Level::Error:  return ImVec4(0.95f, 0.45f, 0.45f, 1.0f);
        case Level::Input:  return ImVec4(0.60f, 0.68f, 0.85f, 1.0f);
        case Level::Result: return ImVec4(0.70f, 0.85f, 0.70f, 1.0f);
        default:            return ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
    }
}

const char* level_prefix(Level l) {
    switch (l) {
        case Level::Warn:   return "! ";
        case Level::Error:  return "x ";
        case Level::Input:  return "> ";
        case Level::Result: return "< ";
        default:            return "  ";
    }
}

void drain() {
    std::vector<PendingLog> logs;
    {
        std::lock_guard<std::mutex> lk(g_mux);
        logs.swap(g_pending_logs);
    }
    for (auto& p : logs) {
        TabState& st = state(p.tab_id);
        st.logs.push_back(std::move(p.entry));
        if (st.logs.size() > kMaxLogs) {
            st.logs.erase(st.logs.begin(), st.logs.begin() + (st.logs.size() - kMaxLogs));
        }
        st.scroll_to_end = true;
    }
}

void draw_console(Tab& tab, TabState& st);
void draw_elements(Tab& tab, TabState& st);
void draw_placeholder(const char* name);

// Depth-first search for a node by id, plus the chain of ancestors above it.
DomNode* find_node(DomNode& root, std::uint64_t id, std::vector<DomNode*>* chain) {
    if (root.node_id == id) {
        if (chain) chain->push_back(&root);
        return &root;
    }
    for (DomNode& child : root.children) {
        if (DomNode* hit = find_node(child, id, chain)) {
            if (chain) chain->push_back(&root);
            return hit;
        }
    }
    return nullptr;
}

// ---- Chrome shared by the panels -------------------------------------------
// Drawn to match the window's own toolbar rather than left as stock ImGui: flat
// tabs with a top accent stripe, transparent buttons that tint purple on hover.

constexpr float kStripH = 30.0f;
// Where the first panel tab starts. The panes below run edge to edge.
constexpr float kTabInset = 8.0f;
constexpr float kBandPad = 6.0f;
// Shared by every control in the dock. Squarer than the window chrome's 6, which
// suits an address bar but not a pane of inspector controls.
constexpr float kDtRounding = 3.0f;

// Rounded-top tab, the same shape and tones as a page tab.
bool strip_tab(const char* label, bool active, float w) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(label, ImVec2(w, kStripH));
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked();
    ImVec2 mn = p, mx = ImVec2(p.x + w, p.y + kStripH);

    if (active) {
        dl->AddRectFilled(mn, mx, Theme::dt_tab_active_bg, kDtRounding, ImDrawFlags_RoundCornersTop);
        dl->AddRectFilled(mn, ImVec2(mx.x, mn.y + 2.0f), Theme::tab_accent_stripe);
    } else if (hovered) {
        dl->AddRectFilled(mn, mx, Theme::dt_tab_hover_bg, kDtRounding, ImDrawFlags_RoundCornersTop);
    }

    ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(std::round(mn.x + (w - ts.x) * 0.5f),
                       std::round(mn.y + (kStripH - ts.y) * 0.5f)),
                active ? Theme::dt_text_on : Theme::dt_text_off, label);
    return clicked;
}

// Transparent until hovered, like the back/forward/reload buttons.
bool tool_button(const char* label, bool active = false) {
    ImGui::PushStyleColor(ImGuiCol_Button,
                          active ? Theme::btn_active_highlight : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::btn_hover_highlight);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::btn_active_highlight);
    ImGui::PushStyleColor(ImGuiCol_Text, active ? Theme::dt_accent : Theme::dt_text);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, kDtRounding);
    bool hit = ImGui::Button(label);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    return hit;
}

// Icon-only variant. Square rather than round: a round button reads as browser
// chrome, and this row is an inspector's.
bool tool_icon_button(const char* id, void (*icon)(ImVec2, ImU32, float), bool active) {
    const float side = ImGui::GetFrameHeight();
    ImGui::PushStyleColor(ImGuiCol_Button,
                          active ? Theme::btn_active_highlight : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::btn_hover_highlight);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::btn_active_highlight);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, kDtRounding);
    bool hit = ImGui::Button(id, ImVec2(side, side));
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    ImVec2 c((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
             (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f);
    icon(c, active ? ImGui::ColorConvertFloat4ToU32(Theme::dt_accent) : Theme::dt_text_on,
         side - 6.0f);
    return hit;
}

// Sub-tab inside a panel. The dock's tabs wear their accent stripe on top, so
// this row wears it underneath and the two never read as the same control.
bool sub_tab(const char* label, bool active) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = ImGui::GetFrameHeight();
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const float w = ts.x + 18.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(label, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();

    dl->AddText(ImVec2(std::round(p.x + (w - ts.x) * 0.5f),
                       std::round(p.y + (h - ts.y) * 0.5f)),
                (active || hovered) ? Theme::dt_text_on : Theme::dt_text_off, label);
    if (active) {
        dl->AddRectFilled(ImVec2(p.x, p.y + h - 2.0f), ImVec2(p.x + w, p.y + h),
                          ImGui::ColorConvertFloat4ToU32(Theme::dt_accent));
    }
    return ImGui::IsItemClicked();
}

// Level filter chip. Neutral fill when on, with the level's glyph and colour
// doing the signalling, so it never shouts louder than the log it filters.
constexpr float kChipIcon = 14.0f;

void chip_toggle(const char* label, bool* value, ImU32 tint,
                 void (*icon)(ImVec2, ImU32, float)) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const float h = ImGui::GetFrameHeight();
    const float w = ts.x + kChipIcon + 19.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(label, ImVec2(w, h));
    if (ImGui::IsItemClicked()) *value = !*value;
    const bool hovered = ImGui::IsItemHovered();

    const ImVec2 mn = p, mx = ImVec2(p.x + w, p.y + h);
    if (*value) {
        dl->AddRectFilled(mn, mx, IM_COL32(255, 255, 255, 22), kDtRounding);
    } else if (hovered) {
        dl->AddRectFilled(mn, mx, IM_COL32(255, 255, 255, 12), kDtRounding);
    }
    icon(ImVec2(std::round(mn.x + 7.0f + kChipIcon * 0.5f), std::round(mn.y + h * 0.5f)),
         *value ? tint : (tint & 0x00FFFFFF) | 0x70000000, kChipIcon);
    dl->AddText(ImVec2(std::round(mn.x + 7.0f + kChipIcon + 5.0f),
                       std::round(mn.y + (h - ts.y) * 0.5f)),
                *value ? Theme::dt_text_on : Theme::dt_text_off, label);
}

// A control row sits on a lighter band, the way the omnibox sits on the toolbar,
// or a recessed field ends up darker than its own background. The height isn't
// known until the controls are laid out, so the band goes on a second channel.
struct ToolbarBand {
    ImDrawListSplitter splitter;
    float top = 0.0f;

    void begin() {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        top = p.y;
        splitter.Split(dl, 2);
        splitter.SetCurrentChannel(dl, 1);
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + kBandPad));
    }

    // `footer` puts the hairline on top instead, for the band under the panel.
    void end(bool footer = false) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        // The cursor already carries one ItemSpacing past the last control; take
        // that back before adding the pad or the row sits high in its band.
        const float bottom = p.y - ImGui::GetStyle().ItemSpacing.y + kBandPad;
        const float x0 = ImGui::GetWindowPos().x;
        const float x1 = x0 + ImGui::GetWindowWidth();
        const float edge = footer ? top + 0.5f : bottom - 0.5f;
        splitter.SetCurrentChannel(dl, 0);
        // A footer sits on the dock's bottom edge, so it takes the corner with
        // it; a square fill would paint past the rounding onto the border.
        dl->AddRectFilled(ImVec2(x0, top), ImVec2(x1, bottom), Theme::toolbar_bg,
                          footer ? ImGui::GetStyle().ChildRounding : 0.0f,
                          ImDrawFlags_RoundCornersBottomRight);
        dl->AddLine(ImVec2(x0, edge), ImVec2(x1, edge), Theme::dt_hairline, 1.0f);
        splitter.Merge(dl);
        ImGui::SetCursorScreenPos(ImVec2(p.x, bottom));
        // ImGui only grows a window to items, not to a moved cursor.
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
    }
};

// Squared off, not a pill like the omnibox: a column of pills in a properties
// pane reads as noise.
bool field_input(const char* id, const char* hint, char* buf, std::size_t cap) {
    ImGui::PushStyleColor(ImGuiCol_FrameBg, Theme::dt_field_bg);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(8.0f, ImGui::GetStyle().FramePadding.y));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, kDtRounding);
    ImGui::PushItemWidth(-1.0f);
    bool changed = ImGui::InputTextWithHint(id, hint, buf, cap);
    ImGui::PopItemWidth();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    return changed;
}

// A recessed pane, matching the viewport's rounding and the chrome's tones.
void begin_surface(const char* id, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(Theme::dt_surface_bg));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, kDtRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
    ImGui::BeginChild(id, size, true);
}

void end_surface() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void section_label(const char* text) {
    ImGui::Spacing();
    ImGui::TextColored(Theme::dt_dim, "%s", text);
}

// `tag#id.class` for the tree row and the highlight chip.
std::string node_label(const DomNode& n) {
    if (n.tag == "#text") {
        std::string t = collapse_whitespace(n.text_content);
        if (t.size() > 48) { t.resize(45); t += "..."; }
        return "\"" + t + "\"";
    }
    std::string s = "<" + n.tag;
    if (!n.id.empty()) s += " #" + n.id;
    if (!n.class_name.empty()) s += " ." + n.class_name;
    s += ">";
    return s;
}

}  // namespace

void log(int tab_id, Level lvl, std::string text, std::string source, int line) {
    std::lock_guard<std::mutex> lk(g_mux);
    g_pending_logs.push_back({tab_id, LogEntry{lvl, std::move(text), std::move(source), line}});
}

std::uint64_t net_begin(int, const std::string&, const std::string&, const char*) { return 0; }
void net_end(std::uint64_t, const FetchResult&, std::size_t) {}

void toggle(int tab_id) {
    TabState& st = state(tab_id);
    st.open = !st.open;
}

void set_open(int tab_id, bool open) { state(tab_id).open = open; }

void set_panel(int tab_id, const std::string& name) {
    const std::pair<const char*, Panel> byname[] = {
        {"elements", Panel::Elements}, {"console", Panel::Console},
        {"network", Panel::Network},   {"sources", Panel::Sources},
        {"metrics", Panel::Metrics},
    };
    for (const auto& [key, id] : byname) {
        if (name == key) {
            TabState& st = state(tab_id);
            st.panel = id;
            st.force_panel = true;
            return;
        }
    }
}

bool is_open(int tab_id) {
    TabState* st = find(tab_id);
    return st && st->open;
}

float dock_width(int tab_id, float shell_avail_w) {
    if (!is_open(tab_id)) return 0.0f;
    // The page keeps at least kMinPage, but on a window too narrow to give both
    // their minimum the dock takes half rather than disappearing.
    constexpr float kMinPage = 360.0f;
    constexpr float kMinDock = 280.0f;
    float max_w = shell_avail_w - kMinPage;
    if (max_w < kMinDock) max_w = shell_avail_w * 0.5f;
    return std::clamp(g_dock_w, std::min(kMinDock, max_w), max_w);
}

void drag_dock(float delta_x) {
    g_dock_w += delta_x;
    g_dock_w = std::clamp(g_dock_w, 200.0f, 1600.0f);
}

void select_node(Tab& tab, const std::string& query) {
    if (query.empty()) return;
    std::function<DomNode*(DomNode&)> walk = [&](DomNode& n) -> DomNode* {
        bool hit = query[0] == '#'   ? n.id == query.substr(1)
                 : query[0] == '.'   ? n.class_name == query.substr(1)
                                     : n.tag == query;
        if (hit) return &n;
        for (DomNode& c : n.children)
            if (DomNode* r = walk(c)) return r;
        return nullptr;
    };
    if (DomNode* n = walk(tab.page_dom)) {
        TabState& st = state(tab.id);
        st.selected = n->node_id;
        st.edit_for = 0;
    }
}

void on_navigate(int tab_id) {
    TabState* st = find(tab_id);
    if (!st) return;
    st->logs.clear();
    // Node ids are reassigned by the parse, so a selection from the old page would
    // point at an unrelated element.
    st->selected = st->hovered = st->edit_for = 0;
    st->picking = false;
    st->boxes.clear();
    st->boxes_next.clear();
}

void on_tab_closed(int tab_id) { g_tabs.erase(tab_id); }

bool pick_active(int tab_id) {
    TabState* st = find(tab_id);
    return st && st->open && st->picking;
}

bool capturing(int tab_id) {
    TabState* st = find(tab_id);
    return st && st->open && st->panel == Panel::Elements;
}

void note_box(int tab_id, std::uint64_t node_id, ImVec2 min, ImVec2 max, ImVec4 used_padding) {
    TabState* st = find(tab_id);
    if (st) st->boxes_next.push_back({node_id, min, max, used_padding});
}

void begin_frame(int tab_id) {
    drain();
    if (TabState* st = find(tab_id)) {
        // Last frame's boxes are what the picker hits against this frame.
        st->boxes.swap(st->boxes_next);
        st->boxes_next.clear();
        st->hovered = 0;
    }
}

bool wants_frames(int tab_id) {
    TabState* st = find(tab_id);
    return st && st->open && st->panel == Panel::Metrics;
}

void note_idle_wait(double seconds) { g_idle_wait = seconds; }

void draw_overlay(Tab& tab, ImVec2 vp_min, ImVec2 vp_max) {
    TabState* stp = find(tab.id);
    if (!stp || !stp->open || stp->panel != Panel::Elements) return;
    TabState& st = *stp;

    // Picking reads the boxes collected last frame, smallest hit first: the
    // deepest element under the cursor is the one that was meant.
    if (st.picking) {
        ImVec2 m = ImGui::GetIO().MousePos;
        const NodeBox* best = nullptr;
        if (m.x >= vp_min.x && m.x < vp_max.x && m.y >= vp_min.y && m.y < vp_max.y) {
            float best_area = 0.0f;
            for (const NodeBox& b : st.boxes) {
                if (m.x < b.min.x || m.x >= b.max.x || m.y < b.min.y || m.y >= b.max.y) continue;
                float area = (b.max.x - b.min.x) * (b.max.y - b.min.y);
                if (!best || area < best_area) { best = &b; best_area = area; }
            }
        }
        if (best) {
            st.hovered = best->node_id;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                st.selected = best->node_id;
                st.picking = false;
            }
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) st.picking = false;
    }

    const std::uint64_t target = st.hovered ? st.hovered : st.selected;
    if (!target) return;
    const NodeBox* box = nullptr;
    for (const NodeBox& b : st.boxes)
        if (b.node_id == target) { box = &b; break; }
    if (!box) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->PushClipRect(vp_min, vp_max, true);
    dl->AddRectFilled(box->min, box->max, IM_COL32(111, 168, 220, 60));
    dl->AddRect(box->min, box->max, IM_COL32(111, 168, 220, 220));

    std::vector<DomNode*> chain;
    DomNode* node = find_node(tab.page_dom, target, &chain);
    if (node) {
        char chip[256];
        std::snprintf(chip, sizeof chip, "%s  %.0f x %.0f", node_label(*node).c_str(),
                      box->max.x - box->min.x, box->max.y - box->min.y);
        ImVec2 sz = ImGui::CalcTextSize(chip);
        // Above the box unless that would leave the viewport, then just inside it.
        float y = box->min.y - sz.y - 6.0f;
        if (y < vp_min.y) y = box->min.y + 2.0f;
        ImVec2 p(box->min.x, y);
        dl->AddRectFilled(p, ImVec2(p.x + sz.x + 10.0f, p.y + sz.y + 6.0f),
                          IM_COL32(28, 28, 32, 240), 3.0f);
        dl->AddText(ImVec2(p.x + 5.0f, p.y + 3.0f), IM_COL32(235, 235, 240, 255), chip);
    }
    dl->PopClipRect();
}

void draw(Tab& tab) {
    TabState& st = state(tab.id);
    st.force_panel = false;

    const std::pair<const char*, Panel> panels[] = {
        {"Elements", Panel::Elements}, {"Console", Panel::Console},
        {"Network", Panel::Network},   {"Sources", Panel::Sources},
        {"Metrics", Panel::Metrics},
    };

    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Same as the page's list (see browser.cpp): unscaled, a hairline carries two
    // device pixels of gradient per side and a 14px icon reads as a smudge.
    const float fb_scale = ImGui::GetIO().DisplayFramebufferScale.y;
    if (fb_scale > 1.0f) dl->_FringeScale = 1.0f / fb_scale;

    ImVec2 strip = ImGui::GetCursorScreenPos();
    const float strip_w = ImGui::GetContentRegionAvail().x;
    dl->AddRectFilled(strip, ImVec2(strip.x + strip_w, strip.y + kStripH), Theme::dt_strip_bg);

    const float tab_w = std::max(52.0f, std::min(88.0f, (strip_w - 2.0f * kTabInset) / 5.0f));
    ImGui::SetCursorScreenPos(ImVec2(strip.x + kTabInset, strip.y));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    for (int i = 0; i < 5; i++) {
        if (i) ImGui::SameLine();
        if (strip_tab(panels[i].first, st.panel == panels[i].second, tab_w)) {
            st.panel = panels[i].second;
        }
    }
    ImGui::PopStyleVar();
    dl->AddLine(ImVec2(strip.x, strip.y + kStripH - 0.5f),
                ImVec2(strip.x + strip_w, strip.y + kStripH - 0.5f), Theme::dt_hairline, 1.0f);
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Text, Theme::dt_text);
    switch (st.panel) {
        case Panel::Console:  draw_console(tab, st); break;
        case Panel::Elements: draw_elements(tab, st); break;
        case Panel::Network:  draw_placeholder("Network"); break;
        case Panel::Sources:  draw_placeholder("Sources"); break;
        case Panel::Metrics:  draw_placeholder("Metrics"); break;
    }
    ImGui::PopStyleColor();
}

namespace {

void draw_placeholder(const char* name) {
    begin_surface("##dt_todo", ImVec2(0, 0));
    ImGui::Spacing();
    ImGui::TextColored(Theme::dt_dim, "%s", name);
    ImGui::TextDisabled("Not built yet.");
    end_surface();
}

// Tags render_node walks past. Shown, but greyed, so the tree matches the parse
// rather than the paint.
bool is_unpainted(const std::string& tag) {
    return tag == "script" || tag == "style" || tag == "head" || tag == "title" ||
           tag == "meta" || tag == "option";
}

void draw_tree_node(DomNode& node, TabState& st) {
    if (node.tag == "#text" && collapse_whitespace(node.text_content).empty()) return;

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_DefaultOpen;
    bool leaf = node.children.empty();
    if (leaf) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (node.node_id == st.selected) flags |= ImGuiTreeNodeFlags_Selected;

    const bool dim = is_unpainted(node.tag) || node.tag == "#text";
    if (dim) ImGui::PushStyleColor(ImGuiCol_Text, Theme::dt_dim);
    bool opened = ImGui::TreeNodeEx((void*)(uintptr_t)node.node_id, flags, "%s",
                                    node_label(node).c_str());
    if (dim) ImGui::PopStyleColor();

    if (ImGui::IsItemHovered()) st.hovered = node.node_id;
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) st.selected = node.node_id;

    if (opened && !leaf) {
        for (DomNode& child : node.children) draw_tree_node(child, st);
        ImGui::TreePop();
    }
}

void style_row(const char* name, const char* fmt, ...) IM_FMTARGS(2);
void style_row(const char* name, const char* fmt, ...) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextColored(Theme::dt_dim, "%s", name);
    ImGui::TableNextColumn();
    va_list args;
    va_start(args, fmt);
    ImGui::TextV(fmt, args);
    va_end(args);
}

// CssStyle spells "unset" as a negative, and printing that raw reads as a real
// size. Report what the layout would fall back to instead.
void style_row_size(const char* name, float v) {
    if (v < 0.0f) style_row(name, "auto");
    else          style_row(name, "%.1f", v);
}

void draw_box_model(const CssStyle& s, const NodeBox* box) {
    const float row = ImGui::GetTextLineHeight();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float w = std::min(ImGui::GetContentRegionAvail().x, 300.0f);
    const float h = row * 7.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // What the layout inset the content by, which is not always what the CSS asked
    // for. Only the renderer knows, so a node it never painted falls back to the
    // declared values.
    const float pl = box ? box->pad.x : s.padding_left;
    const float pt = box ? box->pad.y : s.padding_top;
    const float pr = box ? box->pad.z : s.padding_right;
    const float pb = box ? box->pad.w : s.padding_bottom;

    // The border ring is drawn for the shape a box model is expected to have, but
    // it is a zero-width one here: the renderer strokes the border on the padding
    // box's own edge, so it never displaces the content.
    struct Ring { const char* label; ImU32 col; float l, r, t, b; };
    const Ring rings[] = {
        {"margin",  IM_COL32(120, 92, 48, 255),  s.margin_left,  s.margin_right,
                                                 s.margin_top,   s.margin_bottom},
        {"border",  IM_COL32(92, 92, 62, 255),   s.border_width, s.border_width,
                                                 s.border_width, s.border_width},
        {"padding", IM_COL32(60, 96, 72, 255),   pl, pr, pt, pb},
    };

    ImVec2 mn = origin, mx = ImVec2(origin.x + w, origin.y + h);
    const float inset_x = w * 0.13f, inset_y = h * 0.13f;
    for (const Ring& r : rings) {
        dl->AddRectFilled(mn, mx, r.col);
        dl->AddRect(mn, mx, IM_COL32(0, 0, 0, 90));
        char buf[24];
        std::snprintf(buf, sizeof buf, "%.0f", r.t);
        dl->AddText(ImVec2((mn.x + mx.x) * 0.5f - 6.0f, mn.y + 1.0f), IM_COL32_WHITE, buf);
        std::snprintf(buf, sizeof buf, "%.0f", r.b);
        dl->AddText(ImVec2((mn.x + mx.x) * 0.5f - 6.0f, mx.y - row - 1.0f), IM_COL32_WHITE, buf);
        std::snprintf(buf, sizeof buf, "%.0f", r.l);
        dl->AddText(ImVec2(mn.x + 3.0f, (mn.y + mx.y) * 0.5f - row * 0.5f), IM_COL32_WHITE, buf);
        std::snprintf(buf, sizeof buf, "%.0f", r.r);
        dl->AddText(ImVec2(mx.x - 20.0f, (mn.y + mx.y) * 0.5f - row * 0.5f), IM_COL32_WHITE, buf);
        dl->AddText(ImVec2(mn.x + 3.0f, mn.y + row + 1.0f), IM_COL32(210, 210, 210, 200), r.label);
        mn.x += inset_x; mn.y += inset_y; mx.x -= inset_x; mx.y -= inset_y;
    }
    dl->AddRectFilled(mn, mx, IM_COL32(52, 74, 100, 255));
    dl->AddRect(mn, mx, IM_COL32(0, 0, 0, 90));
    char content[48];
    if (box) {
        float cw = (box->max.x - box->min.x) - pl - pr;
        float ch = (box->max.y - box->min.y) - pt - pb;
        std::snprintf(content, sizeof content, "%.0f x %.0f", std::max(cw, 0.0f),
                      std::max(ch, 0.0f));
    } else {
        std::snprintf(content, sizeof content, "not painted");
    }
    ImVec2 csz = ImGui::CalcTextSize(content);
    dl->AddText(ImVec2((mn.x + mx.x) * 0.5f - csz.x * 0.5f, (mn.y + mx.y) * 0.5f - row * 0.5f),
                IM_COL32_WHITE, content);
    ImGui::Dummy(ImVec2(w, h));
}

void load_edit_buffers(TabState& st, const DomNode& n) {
    std::snprintf(st.edit_text, sizeof st.edit_text, "%s", n.text_content.c_str());
    std::snprintf(st.edit_id, sizeof st.edit_id, "%s", n.id.c_str());
    std::snprintf(st.edit_class, sizeof st.edit_class, "%s", n.class_name.c_str());
    std::snprintf(st.edit_style, sizeof st.edit_style, "%s", n.inline_style.c_str());
    st.edit_for = n.node_id;
}

void draw_styles_pane(Tab& tab, TabState& st, DomNode& node, std::vector<DomNode*>& chain) {
    // The renderer's own resolver, fed the same starting style and the same
    // ancestor chain it would have seen, so these are the numbers that were used.
    CssStyle computed;
    computed.color = ImVec4(0.95f, 0.95f, 0.95f, 1.0f);
    computed.has_color = true;
    // Resolving a vh-sized node opts the page into the viewport-slack convergence,
    // and the dock draws after the flag is read and cleared for the frame. Merely
    // inspecting one must not arm it for the next.
    const bool fit_used = tab.vp_fit_used;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        computed = merge_node_style(**it, computed, tab);
    }
    tab.vp_fit_used = fit_used;

    const NodeBox* box = nullptr;
    for (const NodeBox& b : st.boxes)
        if (b.node_id == node.node_id) { box = &b; break; }

    if (mono_font) ImGui::PushFont(mono_font);
    ImGui::PushStyleColor(ImGuiCol_Text, Theme::dt_accent);
    ImGui::TextWrapped("%s", node_label(node).c_str());
    ImGui::PopStyleColor();
    if (mono_font) ImGui::PopFont();

    const char* detail_names[] = {"Styles", "Computed", "Node"};
    // The rule goes down first so each tab's underline paints over its own span
    // of it rather than being sliced by it.
    const ImVec2 row = ImGui::GetCursorScreenPos();
    const float rule_y = row.y + ImGui::GetFrameHeight() - 1.0f;
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(row.x, rule_y), ImVec2(row.x + ImGui::GetContentRegionAvail().x, rule_y),
        Theme::dt_hairline, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, 0.0f));
    for (int i = 0; i < 3; i++) {
        if (i) ImGui::SameLine();
        if (sub_tab(detail_names[i], st.detail_tab == i)) st.detail_tab = i;
    }
    ImGui::PopStyleVar();
    ImGui::Spacing();

    {
        if (st.detail_tab == 0) {
            if (st.edit_for != node.node_id) load_edit_buffers(st, node);

            if (node.tag != "#text") {
                ImGui::TextColored(Theme::dt_dim, "id");
                if (field_input("##dt_e_id", "none", st.edit_id, IM_ARRAYSIZE(st.edit_id))) {
                    node.id = st.edit_id;
                }
                ImGui::TextColored(Theme::dt_dim, "class");
                if (field_input("##dt_e_cls", "none", st.edit_class, IM_ARRAYSIZE(st.edit_class))) {
                    node.class_name = st.edit_class;
                }
            }
            ImGui::TextColored(Theme::dt_dim, "text");
            if (field_input("##dt_e_txt", "empty", st.edit_text, IM_ARRAYSIZE(st.edit_text))) {
                node.text_content = st.edit_text;
                if (node.tag != "#text") node.children.clear();  // as textContent does
            }
            ImGui::TextColored(Theme::dt_dim, "style");
            if (field_input("##dt_e_sty", "color: red; padding: 8",
                           st.edit_style, IM_ARRAYSIZE(st.edit_style))) {
                // Same path el.style.x = y takes, so an edit behaves like a script's.
                node.inline_style = st.edit_style;
                node.parsed_inline_style = CssStyle();
                parse_css_properties(node.inline_style, node.parsed_inline_style);
                node.has_inline_style = !node.inline_style.empty();
            }

            section_label("Matched rules");
            // Class rules are keyed with the leading dot, as merge_node_style looks
            // them up; tag rules are keyed bare.
            bool matched = false;
            if (tab.css_classes.count(node.tag)) {
                ImGui::BulletText("%s", node.tag.c_str());
                matched = true;
            }
            if (!node.class_name.empty() && tab.css_classes.count("." + node.class_name)) {
                ImGui::BulletText(".%s", node.class_name.c_str());
                matched = true;
            }
            if (node.has_inline_style) {
                ImGui::BulletText("inline");
                matched = true;
            }
            if (!matched) ImGui::TextDisabled("  none (inherited only)");

            section_label("Box model");
            draw_box_model(computed, box);
        }

        if (st.detail_tab == 1) {
            if (ImGui::BeginTable("##dt_computed", 2,
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
                style_row("color", "%.2f %.2f %.2f %.2f", computed.color.x, computed.color.y,
                          computed.color.z, computed.color.w);
                if (computed.has_bg)
                    style_row("background", "%.2f %.2f %.2f %.2f", computed.bg_color.x,
                              computed.bg_color.y, computed.bg_color.z, computed.bg_color.w);
                // The heading scale is folded in at paint time, not by the cascade,
                // so the inherited multiplier on its own would report an h1 at 1x.
                style_row("font-size", "%.2fx", computed.font_size * heading_font_scale(node.tag));
                if (!computed.font_family.empty())
                    style_row("font-family", "%s", computed.font_family.c_str());
                style_row("text-align", "%s", computed.text_align.c_str());
                style_row_size("width", computed.width);
                style_row_size("height", computed.height);
                style_row("padding", "%.0f %.0f %.0f %.0f", computed.padding_top,
                          computed.padding_right, computed.padding_bottom, computed.padding_left);
                style_row("margin", "%.0f %.0f %.0f %.0f", computed.margin_top,
                          computed.margin_right, computed.margin_bottom, computed.margin_left);
                style_row("border", "%.1f", computed.border_width);
                if (computed.border_radius < 0.0f) style_row("border-radius", "none");
                else style_row("border-radius", "%.1f", computed.border_radius);
                if (!computed.display.empty()) style_row("display", "%s", computed.display.c_str());
                if (!computed.position.empty()) {
                    style_row("position", "%s", computed.position.c_str());
                    style_row("left/top", "%.0f / %.0f",
                              computed.pos_left.resolve(page_viewport_w_full, page_viewport_h_full),
                              computed.pos_top.resolve(page_viewport_w_full, page_viewport_h_full));
                }
                if (computed.display == "flex" || !computed.flex_direction.empty()) {
                    // Unset properties are shown as the value the layout falls back
                    // to, not as the empty string the cascade left behind.
                    auto or_default = [](const std::string& v, const char* d) {
                        return v.empty() ? d : v.c_str();
                    };
                    style_row("flex-direction", "%s", or_default(computed.flex_direction, "row"));
                    style_row("justify-content", "%s",
                              or_default(computed.justify_content, "flex-start"));
                    style_row("align-items", "%s", or_default(computed.align_items, "stretch"));
                    style_row("gap", "%.0f / %.0f", std::max(0.0f, computed.row_gap),
                              std::max(0.0f, computed.column_gap));
                }
                if (computed.flex_grow >= 0.0f || computed.flex_shrink >= 0.0f ||
                    computed.flex_basis >= 0.0f) {
                    // Yoga's web defaults, which is what an unset field leaves in place.
                    char basis[24];
                    if (computed.flex_basis >= 0.0f) {
                        std::snprintf(basis, sizeof basis, "%.1f", computed.flex_basis);
                    } else {
                        std::snprintf(basis, sizeof basis, "auto");
                    }
                    style_row("flex", "%.1f %.1f %s", std::max(0.0f, computed.flex_grow),
                              computed.flex_shrink < 0.0f ? 1.0f : computed.flex_shrink, basis);
                }
                ImGui::EndTable();
            }
        }

        if (st.detail_tab == 2) {
            ImGui::Text("node_id  %llu", (unsigned long long)node.node_id);
            ImGui::Text("tag      %s", node.tag.c_str());
            if (!node.href.empty())        ImGui::TextWrapped("href     %s", node.href.c_str());
            if (!node.src.empty())         ImGui::TextWrapped("src      %s", node.src.c_str());
            if (!node.type.empty())        ImGui::Text("type     %s", node.type.c_str());
            if (!node.value.empty())       ImGui::TextWrapped("value    %s", node.value.c_str());
            if (!node.placeholder.empty()) ImGui::Text("placeholder %s", node.placeholder.c_str());
            if (!node.name.empty())        ImGui::Text("name     %s", node.name.c_str());
            if (!node.onclick.empty())     ImGui::TextWrapped("onclick  %s", node.onclick.c_str());
            ImGui::Text("children %d", (int)node.children.size());
            ImGui::Text("handlers click=%s input=%s",
                        script_has_click_handler(tab.id, node.node_id) ? "yes" : "no",
                        "-");
            if (box) {
                ImGui::Text("box      %.0f,%.0f  %.0f x %.0f", box->min.x, box->min.y,
                            box->max.x - box->min.x, box->max.y - box->min.y);
            } else {
                ImGui::TextColored(Theme::dt_dim, "box      not painted this frame");
            }
        }
    }
}

void draw_elements(Tab& tab, TabState& st) {
    ToolbarBand band;
    band.begin();
    if (tool_icon_button("##dt_pick", DrawInspectIcon, st.picking)) st.picking = !st.picking;
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(st.picking
                              ? "Click an element in the page (right-click cancels)"
                              : "Select an element by clicking it in the page");
    }
    ImGui::SameLine(0.0f, 6.0f);
    if (tool_button("Deselect")) { st.selected = 0; st.edit_for = 0; }
    ImGui::SameLine();
    float right = ImGui::GetContentRegionAvail().x;
    char count[32];
    std::snprintf(count, sizeof count, "%d boxes", (int)st.boxes.size());
    ImGui::SameLine(0.0f, std::max(8.0f, right - ImGui::CalcTextSize(count).x - 4.0f));
    ImGui::TextColored(Theme::dt_dim, "%s", count);
    band.end();

    const float avail_h = ImGui::GetContentRegionAvail().y;
    const float tree_h = std::max(80.0f, avail_h * st.tree_split - 4.0f);

    begin_surface("##dt_tree", ImVec2(0, tree_h));
    if (mono_font) ImGui::PushFont(mono_font);
    draw_tree_node(tab.page_dom, st);
    if (mono_font) ImGui::PopFont();
    end_surface();

    // Tree over detail, not side by side: the dock is too narrow for two columns.
    ImGui::InvisibleButton("##dt_el_split", ImVec2(-1.0f, 7.0f));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
    if (ImGui::IsItemActive() && avail_h > 1.0f) {
        st.tree_split = std::clamp(st.tree_split + ImGui::GetIO().MouseDelta.y / avail_h,
                                   0.15f, 0.85f);
    }
    {
        ImVec2 gm = ImGui::GetItemRectMin(), gx = ImGui::GetItemRectMax();
        float y = std::round((gm.y + gx.y) * 0.5f);
        float cx = (gm.x + gx.x) * 0.5f;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(cx - 14.0f, y), ImVec2(cx + 14.0f, y),
                                            Theme::dt_hairline, 1.0f);
    }

    begin_surface("##dt_detail", ImVec2(0, 0));
    std::vector<DomNode*> chain;
    DomNode* node = st.selected ? find_node(tab.page_dom, st.selected, &chain) : nullptr;
    if (!node) {
        ImGui::TextDisabled("Select an element in the tree, or pick one in the page.");
    } else {
        draw_styles_pane(tab, st, *node, chain);
    }
    end_surface();
}

bool passes_filter(const TabState& st, const LogEntry& e) {
    if (e.level == Level::Log && !st.show_log) return false;
    if (e.level == Level::Warn && !st.show_warn) return false;
    if (e.level == Level::Error && !st.show_error) return false;
    if (st.filter[0] == '\0') return true;
    return e.text.find(st.filter) != std::string::npos ||
           e.source.find(st.filter) != std::string::npos;
}

int console_input_callback(ImGuiInputTextCallbackData* data) {
    TabState& st = *static_cast<TabState*>(data->UserData);
    if (data->EventFlag != ImGuiInputTextFlags_CallbackHistory) return 0;
    if (st.history.empty()) return 0;

    const int prev = st.history_pos;
    if (data->EventKey == ImGuiKey_UpArrow) {
        if (st.history_pos == -1) st.history_pos = (int)st.history.size() - 1;
        else if (st.history_pos > 0) st.history_pos--;
    } else if (data->EventKey == ImGuiKey_DownArrow) {
        if (st.history_pos != -1 && ++st.history_pos >= (int)st.history.size()) st.history_pos = -1;
    }
    if (prev == st.history_pos) return 0;

    const char* text = st.history_pos >= 0 ? st.history[st.history_pos].c_str() : "";
    data->DeleteChars(0, data->BufTextLen);
    data->InsertChars(0, text);
    return 0;
}

void submit_console(TabState& st, int tab_id) {
    std::string cmd = st.input;
    st.input[0] = '\0';
    st.history_pos = -1;
    st.focus_input = true;
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    auto b = std::find_if(cmd.begin(), cmd.end(), not_space);
    auto e = std::find_if(cmd.rbegin(), cmd.rend(), not_space).base();
    cmd = (b < e) ? std::string(b, e) : std::string();
    if (cmd.empty()) return;

    if (st.history.empty() || st.history.back() != cmd) st.history.push_back(cmd);
    st.logs.push_back({Level::Input, cmd, {}, 0});

    ScriptEngine* eng = script_engine_for(tab_id);
    if (!eng || !eng->ok()) {
        st.logs.push_back({Level::Error, "no script engine for this page", {}, 0});
    } else {
        // Deliberately outside g_mux: luaL_error longjmps, and a lock_guard
        // skipped that way never releases.
        std::string result, err;
        if (eng->eval(cmd, result, err)) {
            if (!result.empty()) st.logs.push_back({Level::Result, result, {}, 0});
        } else {
            st.logs.push_back({Level::Error, err, {}, 0});
        }
    }
    st.scroll_to_end = true;
}

void draw_console(Tab& tab, TabState& st) {
    ToolbarBand band;
    band.begin();
    if (tool_icon_button("##dt_clear", DrawBanIcon, false)) st.logs.clear();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clear console");
    ImGui::SameLine(0.0f, 10.0f);
    chip_toggle("Log", &st.show_log, Theme::dt_text_on, DrawScrollIcon);
    ImGui::SameLine(0.0f, 4.0f);
    chip_toggle("Warn", &st.show_warn, IM_COL32(242, 199, 89, 255), DrawTriangleAlertIcon);
    ImGui::SameLine(0.0f, 4.0f);
    chip_toggle("Error", &st.show_error, IM_COL32(242, 115, 115, 255), DrawCircleXIcon);
    ImGui::Spacing();
    field_input("##dt_filter", "Filter", st.filter, IM_ARRAYSIZE(st.filter));
    band.end();

    // The prompt band costs two pads plus the spacing above it. Measured in mono,
    // the font the row is drawn in, or the reservation is off by the difference
    // and the dock either grows a scrollbar or stops short of its bottom edge.
    if (mono_font) ImGui::PushFont(mono_font);
    const float prompt_h = ImGui::GetFrameHeight() + 2.0f * kBandPad +
                           ImGui::GetStyle().ItemSpacing.y;
    if (mono_font) ImGui::PopFont();
    begin_surface("##dt_log", ImVec2(0, -prompt_h));
    if (mono_font) ImGui::PushFont(mono_font);
    bool any = false;
    for (const LogEntry& e : st.logs) {
        if (!passes_filter(st, e)) continue;
        any = true;
        ImGui::PushStyleColor(ImGuiCol_Text, level_color(e.level));
        ImGui::TextWrapped("%s%s", level_prefix(e.level), e.text.c_str());
        ImGui::PopStyleColor();
        if (!e.source.empty()) {
            if (e.line > 0) ImGui::TextDisabled("      %s:%d", e.source.c_str(), e.line);
            else            ImGui::TextDisabled("      %s", e.source.c_str());
        }
    }
    if (mono_font) ImGui::PopFont();
    if (!any) ImGui::TextDisabled("Nothing logged yet.");
    if (st.scroll_to_end) {
        ImGui::SetScrollHereY(1.0f);
        st.scroll_to_end = false;
    }
    end_surface();

    ToolbarBand prompt;
    prompt.begin();
    // Mono goes on first so the gutter squares off the field's height. Sized off
    // the taller UI font it becomes the tallest item and tips the row off centre.
    if (mono_font) ImGui::PushFont(mono_font);
    const float gutter = ImGui::GetFrameHeight();
    ImGui::Dummy(ImVec2(gutter, gutter));
    DrawChevronRightIcon(ImVec2((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
                                (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f),
                         ImGui::ColorConvertFloat4ToU32(Theme::dt_accent), 14.0f);
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, Theme::dt_field_bg);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(8.0f, ImGui::GetStyle().FramePadding.y));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, kDtRounding);
    ImGui::PushItemWidth(-1.0f);
    const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue |
                                      ImGuiInputTextFlags_CallbackHistory;
    if (ImGui::InputTextWithHint("##dt_eval", "Run Lua in this page", st.input,
                                 IM_ARRAYSIZE(st.input), flags,
                                 console_input_callback, &st)) {
        submit_console(st, tab.id);
    }
    ImGui::PopItemWidth();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    if (mono_font) ImGui::PopFont();
    if (st.focus_input) {
        ImGui::SetKeyboardFocusHere(-1);
        st.focus_input = false;
    }
    prompt.end(true);
}

}  // namespace
}  // namespace devtools
