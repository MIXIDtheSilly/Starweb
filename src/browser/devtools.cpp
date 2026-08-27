#include "devtools.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "fetcher.hpp"
#include "globals.hpp"
#include "media_player.hpp"
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
};

using Clock = std::chrono::steady_clock;

// One request. Opened and closed on a worker thread, read only on the render
// thread; the two halves travel through the pending queue below.
struct NetRecord {
    std::uint64_t id = 0;
    std::string url;
    std::string method = "GET";
    const char* initiator = "other";  // always a literal, see fetcher.hpp
    Clock::time_point start;
    Clock::time_point finish;

    bool done = false;
    bool success = false;
    // Never went out at all (mixed content), as opposed to a failure.
    bool blocked = false;
    int status = 0;
    std::string status_text;
    std::string error;
    std::string content_type;
    std::size_t size = 0;
    double ms = 0.0;
    double offset_ms = 0.0;  // from the navigation that owns this row
    RequestTiming timing;
    bool secure = false;
    TlsInfo tls;
    std::vector<std::pair<std::string, std::string>> req_headers;
    std::vector<std::pair<std::string, std::string>> res_headers;
    std::string preview;      // text bodies only, capped
    bool preview_truncated = false;
    bool preview_binary = false;
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

    // Network.
    std::vector<NetRecord> nets;
    Clock::time_point net_epoch = Clock::now();
    char net_filter[128] = "";
    int net_kind = 0;              // index into kKinds; 0 is All
    bool net_preserve = false;
    std::uint64_t net_selected = 0;
    float net_split = 0.5f;
    int net_detail_tab = 0;        // Headers / Response / Timing / Security
    bool net_scroll_to_end = false;
    // Set while a row is still in flight, so the idle loop keeps drawing until it
    // lands. Recomputed by every draw of the panel.
    bool net_wants_frames = false;

    // Sources. The file list is rebuilt every frame, so the selection is held by
    // key (a URL, or "page" for an inline chunk) rather than by index.
    std::string src_pin;
    char src_find[128] = "";
    bool src_wrap = false;
    float src_split = 0.34f;
    int src_hit = 0;
    int src_hl_line = 0;   // 1-based, 0 for none
    int src_goto_line = 0; // scrolled to once, then cleared
    bool src_scroll_to_pin = false;
    // Dev hook only; resolved in draw_sources, where the file list exists.
    std::string src_want;
    int src_want_line = 0;

    // Split lines and the y each one starts at, rebuilt when any part of the key
    // below changes.
    std::string src_key;
    const void* src_key_ptr = nullptr;
    std::size_t src_key_len = 0;
    bool src_key_wrap = false;
    float src_key_w = 0.0f;
    std::vector<std::string> src_lines;
    std::vector<float> src_tops;  // size lines + 1; the last entry is the total
    float src_widest = 0.0f;
    bool src_hits_dirty = true;
    std::vector<int> src_hits;  // 0-based line indexes

    // Edit buffers, refilled whenever the selection changes.
    std::uint64_t edit_for = 0;
    char edit_text[1024] = "";
    char edit_id[128] = "";
    char edit_class[256] = "";
    char edit_style[512] = "";
};

constexpr std::size_t kMaxLogs = 2000;
constexpr std::size_t kMaxNets = 200;
constexpr std::size_t kMaxMediaNets = 60;
// Enough to read a stylesheet or a JSON reply without 200 rows of them adding up.
constexpr std::size_t kMaxPreview = 32u * 1024u;

std::unordered_map<int, TabState> g_tabs;
// Closed tabs. A fetch can outlive its tab, and its record would otherwise fault
// a fresh TabState into existence that nothing draws or clears.
std::unordered_set<int> g_dead_tabs;
float g_dock_w = 380.0f;
double g_idle_wait = 0.0;

// Guards the queues only. Producers are fetch workers; the render thread drains
// them once a frame so no panel ever draws with a lock held.
std::mutex g_mux;
struct PendingLog {
    int tab_id;
    LogEntry entry;
    // A navigation's clear travels the queue too, to stay ordered with the messages.
    bool reset = false;
};
std::vector<PendingLog> g_pending_logs;

// Network events stay in one queue so their order survives the crossing: a reset
// must not wipe records that were opened after it.
struct PendingNet {
    enum class Kind { Begin, End, Reset } kind;
    int tab_id = 0;
    NetRecord rec;
};
std::vector<PendingNet> g_pending_nets;
std::atomic<std::uint64_t> g_next_net_id{1};

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
    std::vector<PendingNet> nets;
    {
        std::lock_guard<std::mutex> lk(g_mux);
        logs.swap(g_pending_logs);
        nets.swap(g_pending_nets);
    }
    for (auto& p : logs) {
        if (g_dead_tabs.count(p.tab_id)) continue;
        TabState& st = state(p.tab_id);
        if (p.reset) {
            st.logs.clear();
            continue;
        }
        st.logs.push_back(std::move(p.entry));
        if (st.logs.size() > kMaxLogs) {
            st.logs.erase(st.logs.begin(), st.logs.begin() + (st.logs.size() - kMaxLogs));
        }
        st.scroll_to_end = true;
    }
    for (auto& p : nets) {
        // An End carries no tab id: its owner is whichever tab the Begin was folded
        // into, so a record for a tab that has since closed disappears on its own.
        if (p.kind == PendingNet::Kind::End) {
            for (auto& [tid, ts] : g_tabs) {
                (void)tid;
                bool hit = false;
                // From the back: the request that just finished is a recent one.
                for (auto it = ts.nets.rbegin(); it != ts.nets.rend(); ++it) {
                    if (it->id != p.rec.id) continue;
                    NetRecord& r = *it;
                    NetRecord& in = p.rec;
                    r.done = true;
                    r.finish = in.finish;
                    r.ms = std::chrono::duration<double, std::milli>(in.finish - r.start).count();
                    r.success = in.success;
                    r.status = in.status;
                    r.status_text = std::move(in.status_text);
                    r.error = std::move(in.error);
                    r.content_type = std::move(in.content_type);
                    r.size = in.size;
                    r.timing = in.timing;
                    r.secure = in.secure;
                    r.tls = std::move(in.tls);
                    r.req_headers = std::move(in.req_headers);
                    r.res_headers = std::move(in.res_headers);
                    r.preview = std::move(in.preview);
                    r.preview_truncated = in.preview_truncated;
                    r.preview_binary = in.preview_binary;
                    hit = true;
                    break;
                }
                if (hit) break;
            }
            continue;
        }

        if (g_dead_tabs.count(p.tab_id)) continue;
        TabState& st = state(p.tab_id);
        switch (p.kind) {
            case PendingNet::Kind::Reset:
                st.net_epoch = p.rec.start;
                if (!st.net_preserve) {
                    st.nets.clear();
                    st.net_selected = 0;
                }
                break;
            case PendingNet::Kind::End:
                break;  // handled above
            case PendingNet::Kind::Begin: {
                NetRecord rec = std::move(p.rec);
                rec.offset_ms =
                    std::chrono::duration<double, std::milli>(rec.start - st.net_epoch).count();
                const bool is_media = std::strcmp(rec.initiator, "media") == 0;
                st.nets.push_back(std::move(rec));
                // Media gets its own, tighter cap: 64 KiB chunks add up to hundreds
                // of requests that would evict everything else.
                if (is_media) {
                    std::size_t media = 0;
                    for (const NetRecord& r : st.nets)
                        if (std::strcmp(r.initiator, "media") == 0) media++;
                    if (media > kMaxMediaNets) {
                        for (auto it = st.nets.begin(); it != st.nets.end(); ++it) {
                            if (std::strcmp(it->initiator, "media") == 0) {
                                st.nets.erase(it);
                                break;
                            }
                        }
                    }
                }
                if (st.nets.size() > kMaxNets) {
                    st.nets.erase(st.nets.begin(),
                                  st.nets.begin() + (st.nets.size() - kMaxNets));
                }
                st.net_scroll_to_end = true;
                break;
            }
        }
    }
}

void draw_console(Tab& tab, TabState& st);
void draw_elements(Tab& tab, TabState& st);
void draw_network(Tab& tab, TabState& st);
void draw_sources(Tab& tab, TabState& st);
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

constexpr float kStripH = 34.0f;
constexpr float kTabH = 24.0f;
// Also how far a control row is held off the dock's edges; panes run edge to edge.
constexpr float kTabInset = 8.0f;
constexpr float kBandPad = 6.0f;
constexpr float kDtRounding = 4.5f;

// Nothing at rest, hover fill under the cursor, recessed fill in an outline
// once selected.
void control_box(ImDrawList* dl, ImVec2 mn, ImVec2 mx, bool active, bool hovered,
                 bool held = false) {
    if (active) {
        dl->AddRectFilled(mn, mx, Theme::dt_recess, kDtRounding);
        dl->AddRect(mn, mx, Theme::outline_mid, kDtRounding, 0, 1.0f);
    } else if (held) {
        dl->AddRectFilled(mn, mx, Theme::dt_press_bg, kDtRounding);
    } else if (hovered) {
        dl->AddRectFilled(mn, mx, Theme::dt_hover_bg, kDtRounding);
    }
}

void outline_field(bool focused, bool hovered) {
    ImGui::GetWindowDrawList()->AddRect(
        ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
        focused ? Theme::outline_bright
                : (hovered ? Theme::outline_mid : Theme::outline_dim),
        kDtRounding, 0, focused ? 2.0f : 1.0f);
}

bool strip_tab(const char* label, bool active, float w) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(label, ImVec2(w, kStripH));
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked();

    const float pad = (kStripH - kTabH) * 0.5f;
    ImVec2 mn(p.x + 0.5f, p.y + pad), mx(p.x + w - 0.5f, p.y + pad + kTabH);
    control_box(dl, mn, mx, active, hovered);

    ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(std::round(p.x + (w - ts.x) * 0.5f),
                       std::round(p.y + (kStripH - ts.y) * 0.5f)),
                active ? Theme::tab_text_on : Theme::tab_text_off, label);
    return clicked;
}

// Transparent until hovered, like the back/forward/reload buttons.
bool tool_button(const char* label, bool active = false) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const ImVec2 pad = ImGui::GetStyle().FramePadding;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 size(ts.x + pad.x * 2.0f, ImGui::GetFrameHeight());
    // Fires on release, as ImGui's own button does. The selection pills below
    // take the press instead.
    const bool hit = ImGui::InvisibleButton(label, size);

    ImVec2 mn = p, mx = ImVec2(p.x + size.x, p.y + size.y);
    control_box(dl, mn, mx, active, ImGui::IsItemHovered(), ImGui::IsItemActive());
    dl->AddText(ImVec2(std::round(p.x + pad.x), std::round(p.y + (size.y - ts.y) * 0.5f)),
                active ? Theme::dt_text_on : Theme::tab_text_on, label);
    return hit;
}

// Icon-only variant.
bool tool_icon_button(const char* id, void (*icon)(ImVec2, ImU32, float), bool active) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float side = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool hit = ImGui::InvisibleButton(id, ImVec2(side, side));

    control_box(dl, p, ImVec2(p.x + side, p.y + side), active, ImGui::IsItemHovered(),
                ImGui::IsItemActive());
    icon(ImVec2(std::round(p.x + side * 0.5f), std::round(p.y + side * 0.5f)),
         active ? Theme::plus_color_hover : Theme::icon_normal, side - 8.0f);
    return hit;
}

// Sub-tab inside a panel.
bool sub_tab(const char* label, bool active) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = ImGui::GetFrameHeight();
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const float w = ts.x + 18.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(label, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();

    control_box(dl, p, ImVec2(p.x + w, p.y + h), active, hovered);
    dl->AddText(ImVec2(std::round(p.x + (w - ts.x) * 0.5f),
                       std::round(p.y + (h - ts.y) * 0.5f)),
                (active || hovered) ? Theme::dt_text_on : Theme::dt_text_off, label);
    return ImGui::IsItemClicked();
}

// Level filter chip. The glyph carries the level's colour.
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

    const ImVec2 mn = p, mx = ImVec2(p.x + w, p.y + h);
    control_box(dl, mn, mx, *value, ImGui::IsItemHovered());
    icon(ImVec2(std::round(mn.x + 7.0f + kChipIcon * 0.5f), std::round(mn.y + h * 0.5f)),
         *value ? tint : (tint & 0x00FFFFFF) | 0x70000000, kChipIcon);
    dl->AddText(ImVec2(std::round(mn.x + 7.0f + kChipIcon + 5.0f),
                       std::round(mn.y + (h - ts.y) * 0.5f)),
                *value ? Theme::dt_text_on : Theme::dt_text_off, label);
}

struct ControlRow {
    void begin() {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + kBandPad));
        ImGui::Indent(kTabInset);
    }

    void end() {
        ImGui::Unindent(kTabInset);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        // The cursor already carries one ItemSpacing past the last control; take
        // that back before adding the pad or the row sits high in its space.
        ImGui::SetCursorScreenPos(
            ImVec2(p.x, p.y - ImGui::GetStyle().ItemSpacing.y + kBandPad));
        // ImGui only grows a window to items, not to a moved cursor.
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
    }
};

// `right_inset` is the row inset on a toolbar row, where the field has to stop
// where the row started. Inside a pane the pane's own padding does that, so 1.
bool field_input(const char* id, const char* hint, char* buf, std::size_t cap,
                 float right_inset = kTabInset) {
    ImGui::PushStyleColor(ImGuiCol_FrameBg, Theme::dt_field_bg);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Theme::dt_field_bg);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Theme::dt_field_bg);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(8.0f, ImGui::GetStyle().FramePadding.y));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, kDtRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushItemWidth(-right_inset);
    bool changed = ImGui::InputTextWithHint(id, hint, buf, cap);
    const bool focused = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopItemWidth();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(3);
    // After the field, so its fill does not paint over the outline.
    outline_field(focused, hovered);
    return changed;
}

// A recessed pane, held off the dock's edges by the control rows' inset.
void begin_surface(const char* id, ImVec2 size, ImGuiWindowFlags flags = 0) {
    ImGui::Indent(kTabInset);
    if (size.x <= 0.0f) size.x += ImGui::GetContentRegionAvail().x - kTabInset;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(Theme::dt_recess));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, kDtRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
    ImGui::BeginChild(id, size, true, flags);
}

void end_surface() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::Unindent(kTabInset);
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

std::uint64_t net_begin(int tab_id, const std::string& url, const std::string& method,
                        const char* initiator) {
    PendingNet p;
    p.kind = PendingNet::Kind::Begin;
    p.tab_id = tab_id;
    p.rec.id = g_next_net_id.fetch_add(1);
    p.rec.url = url;
    p.rec.method = method.empty() ? "GET" : method;
    p.rec.initiator = initiator ? initiator : "other";
    p.rec.start = Clock::now();
    const std::uint64_t id = p.rec.id;
    std::lock_guard<std::mutex> lk(g_mux);
    g_pending_nets.push_back(std::move(p));
    return id;
}

void net_blocked(int tab_id, const std::string& url, const char* initiator,
                 const std::string& reason) {
    PendingNet p;
    p.kind = PendingNet::Kind::Begin;
    p.tab_id = tab_id;
    p.rec.id = g_next_net_id.fetch_add(1);
    p.rec.url = url;
    p.rec.initiator = initiator ? initiator : "other";
    p.rec.start = p.rec.finish = Clock::now();
    // Arrives already closed: there is no second half to wait for.
    p.rec.done = true;
    p.rec.success = false;
    p.rec.blocked = true;
    p.rec.error = reason;
    std::lock_guard<std::mutex> lk(g_mux);
    g_pending_nets.push_back(std::move(p));
}

void net_end(std::uint64_t rec, const FetchResult& res, std::size_t body_bytes) {
    if (rec == 0) return;
    PendingNet p;
    p.kind = PendingNet::Kind::End;
    // No tab id: End is matched by record id in whichever tab the Begin created.
    p.rec.id = rec;
    p.rec.finish = Clock::now();
    p.rec.done = true;
    p.rec.success = res.success;
    p.rec.status = res.status_code;
    p.rec.status_text = res.status_text;
    p.rec.error = res.error_message;
    p.rec.timing = res.timing;
    p.rec.secure = res.is_secure;
    p.rec.tls = res.tls;
    p.rec.req_headers = res.request_headers;
    p.rec.res_headers.assign(res.headers.begin(), res.headers.end());
    std::sort(p.rec.res_headers.begin(), p.rec.res_headers.end());

    auto ct = res.headers.find("content-type");
    if (ct != res.headers.end()) p.rec.content_type = ct->second;

    // A streamed body never lands in res.body, so fall back to what the transport
    // counted and then to what the server declared.
    p.rec.size = body_bytes;
    if (p.rec.size == 0) p.rec.size = res.streamed_bytes;
    if (p.rec.size == 0) {
        auto cl = res.headers.find("content-length");
        if (cl != res.headers.end()) {
            try { p.rec.size = (std::size_t)std::stoull(cl->second); } catch (...) {}
        }
    }

    // Only text is worth a copy; an image preview is megabytes the pane cannot render.
    if (!res.body.empty()) {
        std::string lower = p.rec.content_type;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        const bool textual =
            lower.rfind("text/", 0) == 0 || lower.find("json") != std::string::npos ||
            lower.find("javascript") != std::string::npos ||
            lower.find("xml") != std::string::npos || lower.empty();
        if (textual) {
            const std::size_t take = std::min(res.body.size(), kMaxPreview);
            // An empty content-type is not a promise of text; check before trusting it.
            const bool has_nul = res.body.find('\0', 0) < take;
            if (has_nul) {
                p.rec.preview_binary = true;
            } else {
                p.rec.preview.assign(res.body, 0, take);
                p.rec.preview_truncated = take < res.body.size();
            }
        } else {
            p.rec.preview_binary = true;
        }
    }

    std::lock_guard<std::mutex> lk(g_mux);
    g_pending_nets.push_back(std::move(p));
}

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

bool select_request(int tab_id, const std::string& query) {
    if (query.empty()) return true;
    TabState& st = state(tab_id);
    std::string url = query, want_tab;
    if (const std::size_t comma = query.find(','); comma != std::string::npos) {
        url = query.substr(0, comma);
        want_tab = query.substr(comma + 1);
    }
    const std::pair<const char*, int> names[] = {
        {"headers", 0}, {"response", 1}, {"timing", 2}, {"security", 3},
    };
    for (const auto& [name, idx] : names)
        if (want_tab == name) st.net_detail_tab = idx;

    for (const NetRecord& r : st.nets) {
        if (r.url.find(url) != std::string::npos) {
            st.net_selected = r.id;
            return true;
        }
    }
    return false;
}

void select_source(int tab_id, const std::string& query) {
    if (query.empty()) return;
    TabState& st = state(tab_id);
    st.src_want = query;
    st.src_want_line = 0;
    if (const std::size_t comma = query.find(','); comma != std::string::npos) {
        st.src_want = query.substr(0, comma);
        st.src_want_line = std::atoi(query.c_str() + comma + 1);
    }
}

void on_navigation_start(int tab_id) {
    PendingNet p;
    p.kind = PendingNet::Kind::Reset;
    p.tab_id = tab_id;
    p.rec.start = Clock::now();  // the epoch every row's offset is measured from
    std::lock_guard<std::mutex> lk(g_mux);
    // Queued rather than applied here: warnings for the previous page may still be
    // in flight, and the reset has to land in order with them.
    g_pending_nets.push_back(std::move(p));
    g_pending_logs.push_back({tab_id, LogEntry{}, true});
}

void on_navigate(int tab_id) {
    TabState* st = find(tab_id);
    if (!st) return;
    // Node ids are reassigned by the parse, so a selection from the old page would
    // point at an unrelated element. on_navigation_start clears the logs instead.
    st->selected = st->hovered = st->edit_for = 0;
    st->src_pin.clear();
    st->src_hl_line = st->src_goto_line = 0;
    st->picking = false;
    st->boxes.clear();
    st->boxes_next.clear();
}

void on_tab_closed(int tab_id) {
    g_tabs.erase(tab_id);
    g_dead_tabs.insert(tab_id);
}

bool pick_active(int tab_id) {
    TabState* st = find(tab_id);
    return st && st->open && st->picking;
}

bool capturing(int tab_id) {
    TabState* st = find(tab_id);
    return st && st->open && st->panel == Panel::Elements;
}

void note_box(int tab_id, std::uint64_t node_id, ImVec2 min, ImVec2 max) {
    TabState* st = find(tab_id);
    if (st) st->boxes_next.push_back({node_id, min, max});
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
    if (!st || !st->open) return false;
    return st->panel == Panel::Metrics ||
           (st->panel == Panel::Network && st->net_wants_frames);
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
    dl->AddRectFilled(box->min, box->max, IM_COL32(140, 120, 245, 55));
    dl->AddRect(box->min, box->max, IM_COL32(140, 120, 245, 225));

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
        ImVec2 cmx(p.x + sz.x + 10.0f, p.y + sz.y + 6.0f);
        dl->AddRectFilled(p, cmx, IM_COL32(19, 19, 23, 240), kDtRounding);
        dl->AddRect(p, cmx, Theme::outline_mid, kDtRounding, 0, 1.0f);
        dl->AddText(ImVec2(p.x + 5.0f, p.y + 3.0f), Theme::dt_text_on, chip);
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

    ImGui::PushStyleColor(ImGuiCol_Text, Theme::dt_text);
    ImGui::PushStyleColor(ImGuiCol_Header, ImGui::ColorConvertU32ToFloat4(Theme::dt_row_selected));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImGui::ColorConvertU32ToFloat4(Theme::dt_row_hover));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImGui::ColorConvertU32ToFloat4(Theme::dt_row_selected));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(Theme::outline_dim));
    ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, ImGui::ColorConvertU32ToFloat4(Theme::dt_bg));
    ImGui::PushStyleColor(ImGuiCol_TableBorderLight, ImGui::ColorConvertU32ToFloat4(Theme::outline_dim));
    ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, ImGui::ColorConvertU32ToFloat4(Theme::outline_dim));
    ImGui::PushStyleColor(ImGuiCol_TableRowBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, ImVec4(1.0f, 1.0f, 1.0f, 0.03f));
    switch (st.panel) {
        case Panel::Console:  draw_console(tab, st); break;
        case Panel::Elements: draw_elements(tab, st); break;
        case Panel::Network:  draw_network(tab, st); break;
        case Panel::Sources:  draw_sources(tab, st); break;
        case Panel::Metrics:  draw_placeholder("Metrics"); break;
    }
    ImGui::PopStyleColor(10);
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

void draw_box_model(const CssStyle& s, const NodeBox* box) {
    const float row = ImGui::GetTextLineHeight();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float w = std::min(ImGui::GetContentRegionAvail().x, 300.0f);
    const float h = row * 7.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    struct Ring { const char* label; ImU32 col; float l, r, t, b; };
    const Ring rings[] = {
        {"margin",  IM_COL32(120, 92, 48, 255),  s.margin_left,  s.margin_right,
                                                 s.margin_top,   s.margin_bottom},
        {"border",  IM_COL32(92, 92, 62, 255),   s.border_width, s.border_width,
                                                 s.border_width, s.border_width},
        {"padding", IM_COL32(60, 96, 72, 255),   s.padding_left, s.padding_right,
                                                 s.padding_top,  s.padding_bottom},
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
        float cw = (box->max.x - box->min.x) - s.padding_left - s.padding_right
                 - 2.0f * s.border_width;
        float ch = (box->max.y - box->min.y) - s.padding_top - s.padding_bottom
                 - 2.0f * s.border_width;
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
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        computed = merge_node_style(**it, computed, tab);
    }

    const NodeBox* box = nullptr;
    for (const NodeBox& b : st.boxes)
        if (b.node_id == node.node_id) { box = &b; break; }

    // Header: what is selected, in the same colours the tree uses.
    if (mono_font) ImGui::PushFont(mono_font);
    ImGui::PushStyleColor(ImGuiCol_Text, Theme::dt_accent);
    ImGui::TextWrapped("%s", node_label(node).c_str());
    ImGui::PopStyleColor();
    if (mono_font) ImGui::PopFont();

    const char* detail_names[] = {"Styles", "Computed", "Node"};
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
                if (field_input("##dt_e_id", "none", st.edit_id, IM_ARRAYSIZE(st.edit_id), 1.0f)) {
                    node.id = st.edit_id;
                }
                ImGui::TextColored(Theme::dt_dim, "class");
                if (field_input("##dt_e_cls", "none", st.edit_class, IM_ARRAYSIZE(st.edit_class), 1.0f)) {
                    node.class_name = st.edit_class;
                }
            }
            ImGui::TextColored(Theme::dt_dim, "text");
            if (field_input("##dt_e_txt", "empty", st.edit_text, IM_ARRAYSIZE(st.edit_text),
                            1.0f)) {
                node.text_content = st.edit_text;
                if (node.tag != "#text") node.children.clear();  // as textContent does
            }
            ImGui::TextColored(Theme::dt_dim, "style");
            if (field_input("##dt_e_sty", "color: red; padding: 8",
                            st.edit_style, IM_ARRAYSIZE(st.edit_style), 1.0f)) {
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
                style_row("font-size", "%.2fx", computed.font_size);
                if (!computed.font_family.empty())
                    style_row("font-family", "%s", computed.font_family.c_str());
                style_row("text-align", "%s", computed.text_align.c_str());
                style_row("width", "%.1f", computed.width);
                style_row("height", "%.1f", computed.height);
                style_row("padding", "%.0f %.0f %.0f %.0f", computed.padding_top,
                          computed.padding_right, computed.padding_bottom, computed.padding_left);
                style_row("margin", "%.0f %.0f %.0f %.0f", computed.margin_top,
                          computed.margin_right, computed.margin_bottom, computed.margin_left);
                style_row("border", "%.1f", computed.border_width);
                style_row("border-radius", "%.1f", computed.border_radius);
                if (!computed.display.empty()) style_row("display", "%s", computed.display.c_str());
                if (!computed.position.empty()) {
                    style_row("position", "%s", computed.position.c_str());
                    style_row("left/top", "%.0f / %.0f",
                              computed.pos_left.resolve(page_viewport_w_full, page_viewport_h_full),
                              computed.pos_top.resolve(page_viewport_w_full, page_viewport_h_full));
                }
                if (computed.display == "flex" || !computed.flex_direction.empty()) {
                    style_row("flex-direction", "%s", computed.flex_direction.c_str());
                    style_row("justify-content", "%s", computed.justify_content.c_str());
                    style_row("align-items", "%s", computed.align_items.c_str());
                    style_row("gap", "%.0f / %.0f", computed.row_gap, computed.column_gap);
                }
                if (computed.flex_grow >= 0.0f || computed.flex_basis >= 0.0f) {
                    style_row("flex", "%.1f %.1f %.1f", computed.flex_grow, computed.flex_shrink,
                              computed.flex_basis);
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
    ControlRow band;
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
    ImGui::SameLine(0.0f, std::max(8.0f, right - ImGui::CalcTextSize(count).x - kTabInset));
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
                                            Theme::dt_grip, 1.0f);
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

// Network.

// The chip row. `All` excludes media, whose 64 KiB range requests would bury the
// document; media-probe stays in, since there is only one per file.
struct NetKind { const char* label; const char* tip; };
const NetKind kKinds[] = {
    {"All",   "Everything except media range requests"},
    {"Doc",   "The page itself"},
    {"CSS",   "Stylesheets"},
    {"Lua",   "External scripts"},
    {"Img",   "Images and the favicon"},
    {"Media", "Video and audio: the probe and every range request"},
    {"Fetch", "Requests the page's Lua made with fetch()"},
};
constexpr int kNumKinds = (int)(sizeof(kKinds) / sizeof(kKinds[0]));

// Which chip a row belongs to, or -1 for one only `All` shows.
int kind_of(const NetRecord& r) {
    const char* i = r.initiator;
    if (std::strcmp(i, "document") == 0)   return 1;
    if (std::strcmp(i, "stylesheet") == 0) return 2;
    if (std::strcmp(i, "script") == 0)     return 3;
    if (std::strcmp(i, "image") == 0 || std::strcmp(i, "favicon") == 0) return 4;
    if (std::strcmp(i, "media") == 0 || std::strcmp(i, "media-probe") == 0) return 5;
    if (std::strcmp(i, "fetch") == 0)      return 6;
    return -1;
}

bool hidden_from_all(const NetRecord& r) {
    return std::strcmp(r.initiator, "media") == 0;
}

bool net_passes(const TabState& st, const NetRecord& r) {
    if (st.net_kind == 0) {
        if (hidden_from_all(r)) return false;
    } else if (kind_of(r) != st.net_kind) {
        return false;
    }
    if (st.net_filter[0] == '\0') return true;
    return r.url.find(st.net_filter) != std::string::npos;
}

// The last path segment, which is what identifies a row at this width. Falls back
// to the host for a request at the site root.
std::string short_name(const std::string& url) {
    std::size_t start = url.find("://");
    start = (start == std::string::npos) ? 0 : start + 3;
    std::size_t slash = url.find('/', start);
    if (slash == std::string::npos) return url.substr(start);
    std::string path = url.substr(slash);
    const std::size_t q = path.find_first_of("?#");
    if (q != std::string::npos) path.resize(q);
    const std::size_t last = path.find_last_of('/');
    std::string name = (last == std::string::npos) ? path : path.substr(last + 1);
    if (name.empty()) return url.substr(start, slash - start);
    return name;
}

// Trimmed from the front: the tail carries the extension.
std::string elide_front(std::string name, float w) {
    if (ImGui::CalcTextSize(name.c_str()).x <= w) return name;
    while (name.size() > 2 && ImGui::CalcTextSize((".." + name).c_str()).x > w) {
        name.erase(0, 1);
    }
    return ".." + name;
}

// Three significant figures at most: the Size column is fixed, and "336.3 KB"
// is one character wider than it can hold.
std::string fmt_bytes(std::size_t n) {
    char b[32];
    const double kb = (double)n / 1024.0;
    const double mb = kb / 1024.0;
    if (n < 1024)          std::snprintf(b, sizeof b, "%zu B", n);
    else if (kb < 100.0)   std::snprintf(b, sizeof b, "%.1f KB", kb);
    else if (kb < 1024.0)  std::snprintf(b, sizeof b, "%.0f KB", kb);
    else if (mb < 100.0)   std::snprintf(b, sizeof b, "%.1f MB", mb);
    else                   std::snprintf(b, sizeof b, "%.0f MB", mb);
    return b;
}

std::string fmt_ms(double ms) {
    char b[32];
    if (ms < 0.0) std::snprintf(b, sizeof b, "-");
    else if (ms < 1.0) std::snprintf(b, sizeof b, "%.2f ms", ms);
    else if (ms < 1000.0) std::snprintf(b, sizeof b, "%.0f ms", ms);
    else std::snprintf(b, sizeof b, "%.2f s", ms / 1000.0);
    return b;
}

ImVec4 status_color(const NetRecord& r) {
    if (!r.done)                       return Theme::dt_dim;
    if (!r.success || r.status >= 400) return ImVec4(0.95f, 0.45f, 0.45f, 1.0f);
    if (r.status >= 300)               return ImVec4(0.60f, 0.75f, 0.95f, 1.0f);
    return ImVec4(0.55f, 0.80f, 0.60f, 1.0f);
}

// Text-only sibling of chip_toggle: one choice out of seven, so each chip carries
// a word rather than a glyph. Same fills, so the two rows read alike.
bool filter_chip(const char* label, bool active) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const float h = ImGui::GetFrameHeight();
    const float w = ts.x + 16.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(label, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();

    control_box(dl, p, ImVec2(p.x + w, p.y + h), active, hovered);
    dl->AddText(ImVec2(std::round(p.x + (w - ts.x) * 0.5f), std::round(p.y + (h - ts.y) * 0.5f)),
                active ? Theme::dt_text_on : Theme::dt_text_off, label);
    return ImGui::IsItemClicked();
}

void kv_row(const char* key, const std::string& value) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextColored(Theme::dt_dim, "%s", key);
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s", value.c_str());
}

void header_table(const char* id, const std::vector<std::pair<std::string, std::string>>& hs) {
    if (hs.empty()) {
        ImGui::TextDisabled("  none");
        return;
    }
    if (ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthStretch, 0.42f);
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch, 0.58f);
        for (const auto& [k, v] : hs) kv_row(k.c_str(), v);
        ImGui::EndTable();
    }
}

// The phase breakdown. Each stage is the gap between two of perform_request's
// stamps, so a missing stamp stops the chart rather than inventing an empty bar.
void draw_timing(const NetRecord& r) {
    struct Stage { const char* label; double from, to; };
    const RequestTiming& t = r.timing;
    const double after_connect = t.secured >= 0.0 ? t.secured : t.connected;
    const Stage stages[] = {
        {"Resolve",  0.0,             t.resolved},
        {"Connect",  t.resolved,      t.connected},
        {"TLS",      t.connected,     t.secured},
        {"Send",     after_connect,   t.sent},
        {"Wait",     t.sent,          t.first_byte},
        {"Download", t.first_byte,    t.complete},
    };

    double span = t.complete;
    for (const Stage& s : stages) span = std::max(span, s.to);
    if (span <= 0.0) {
        ImGui::TextDisabled("No timing: the request failed before it reached the network.");
        return;
    }

    // Measured, not guessed: "Download" overruns any round number that fits the
    // rest, and the dock's font is not fixed at build time.
    float label_w = 0.0f;
    for (const Stage& s : stages) label_w = std::max(label_w, ImGui::CalcTextSize(s.label).x);
    label_w += 8.0f;
    const float value_w = ImGui::CalcTextSize("000.00 ms").x + 6.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float bar_h = std::max(6.0f, ImGui::GetTextLineHeight() - 5.0f);

    for (const Stage& s : stages) {
        if (s.from < 0.0 || s.to < 0.0 || s.to < s.from) continue;
        ImGui::TextColored(Theme::dt_dim, "%s", s.label);
        ImGui::SameLine(label_w);

        const float track_w = std::max(40.0f, ImGui::GetContentRegionAvail().x - value_w);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float y = std::round(p.y + (ImGui::GetTextLineHeight() - bar_h) * 0.5f);
        dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + track_w, y + bar_h),
                          ImGui::ColorConvertFloat4ToU32(Theme::dt_field_bg), 2.0f);
        const float x0 = p.x + track_w * (float)(s.from / span);
        // A sub-millisecond stage is still a stage; keep it visible.
        const float x1 = std::max(x0 + 2.0f, p.x + track_w * (float)(s.to / span));
        dl->AddRectFilled(ImVec2(x0, y), ImVec2(std::min(x1, p.x + track_w), y + bar_h),
                          ImGui::ColorConvertFloat4ToU32(Theme::dt_accent), 2.0f);

        ImGui::Dummy(ImVec2(track_w, ImGui::GetTextLineHeight()));
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::TextColored(Theme::dt_dim, "%s", fmt_ms(s.to - s.from).c_str());
    }

    ImGui::Spacing();
    ImGui::Text("Total. %s", fmt_ms(r.ms).c_str());
    if (t.complete >= 0.0) {
        // The gap is the queueing either side of perform_request, worth showing.
        ImGui::TextColored(Theme::dt_dim, "On the wire. %s", fmt_ms(t.complete).c_str());
    }
    ImGui::TextColored(Theme::dt_dim, "Started. +%s", fmt_ms(r.offset_ms).c_str());
}

void draw_net_selected(const NetRecord& r, TabState& st) {
    if (mono_font) ImGui::PushFont(mono_font);
    ImGui::PushStyleColor(ImGuiCol_Text, Theme::dt_accent);
    ImGui::TextWrapped("%s %s", r.method.c_str(), short_name(r.url).c_str());
    ImGui::PopStyleColor();
    if (mono_font) ImGui::PopFont();

    const char* names[] = {"Headers", "Response", "Timing", "Security"};
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, 0.0f));
    for (int i = 0; i < 4; i++) {
        if (i) ImGui::SameLine();
        if (sub_tab(names[i], st.net_detail_tab == i)) st.net_detail_tab = i;
    }
    ImGui::PopStyleVar();
    ImGui::Spacing();

    switch (st.net_detail_tab) {
        case 0: {
            section_label("General");
            if (ImGui::BeginTable("##dt_net_general", 2,
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthStretch, 0.42f);
                ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch, 0.58f);
                kv_row("URL", r.url);
                kv_row("Method", r.method);
                kv_row("Initiator", r.initiator);
                if (!r.done) {
                    kv_row("Status", "in flight");
                } else if (r.blocked) {
                    kv_row("Status", r.error);
                } else if (!r.success) {
                    kv_row("Status", r.error.empty() ? "failed" : r.error);
                } else {
                    kv_row("Status", std::to_string(r.status) + " " + r.status_text);
                }
                if (!r.content_type.empty()) kv_row("Type", r.content_type);
                if (r.done && !r.blocked) kv_row("Size", fmt_bytes(r.size));
                kv_row("Transport", r.secure ? "star:// (TLS)" : "moon:// (plaintext)");
                ImGui::EndTable();
            }
            section_label("Request headers");
            header_table("##dt_net_req", r.req_headers);
            section_label("Response headers");
            header_table("##dt_net_res", r.res_headers);
            break;
        }
        case 1: {
            if (!r.done) {
                ImGui::TextDisabled("Still in flight.");
            } else if (r.blocked) {
                ImGui::TextDisabled("%s", r.error.c_str());
            } else if (r.preview_binary) {
                ImGui::TextDisabled("Binary body, %s. Not shown.", fmt_bytes(r.size).c_str());
            } else if (r.preview.empty()) {
                ImGui::TextDisabled("Empty body.");
            } else {
                if (r.preview_truncated) {
                    ImGui::TextColored(Theme::dt_dim, "First %s of %s",
                                       fmt_bytes(r.preview.size()).c_str(),
                                       fmt_bytes(r.size).c_str());
                }
                if (mono_font) ImGui::PushFont(mono_font);
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(r.preview.c_str(),
                                       r.preview.c_str() + r.preview.size());
                ImGui::PopTextWrapPos();
                if (mono_font) ImGui::PopFont();
            }
            break;
        }
        case 2:
            if (!r.done)        ImGui::TextDisabled("Still in flight.");
            else if (r.blocked) ImGui::TextDisabled("%s", r.error.c_str());
            else                draw_timing(r);
            break;
        case 3: {
            if (!r.secure) {
                ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.35f, 1.0f), "Plaintext");
                ImGui::TextWrapped("moon:// carries no TLS, so this request was readable "
                                   "and alterable in transit.");
                break;
            }
            if (ImGui::BeginTable("##dt_net_tls", 2,
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthStretch, 0.42f);
                ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch, 0.58f);
                kv_row("Protocol", r.tls.version);
                kv_row("Cipher", r.tls.cipher);
                kv_row("ALPN", r.tls.alpn);
                kv_row("Certificate", r.tls.peer_subject);
                kv_row("Issuer", r.tls.peer_issuer);
                kv_row("Valid from", r.tls.not_before);
                kv_row("Valid to", r.tls.not_after);
                kv_row("Verified", r.tls.verified ? "yes" : "no");
                kv_row("Session", r.tls.resumed ? "resumed" : "new handshake");
                ImGui::EndTable();
            }
            break;
        }
    }
}

void draw_network(Tab&, TabState& st) {
    ControlRow band;
    band.begin();
    if (tool_icon_button("##dt_net_clear", DrawBanIcon, false)) {
        st.nets.clear();
        st.net_selected = 0;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clear the network log");
    ImGui::SameLine(0.0f, 10.0f);
    if (filter_chip("Preserve", st.net_preserve)) st.net_preserve = !st.net_preserve;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keep the log across navigations");
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::PushItemWidth(-1.0f);
    field_input("##dt_net_filter", "Filter by URL", st.net_filter, IM_ARRAYSIZE(st.net_filter));
    ImGui::PopItemWidth();

    // The chips wrap rather than overflow: the dock is resizable down to 280.
    const float row_right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    for (int i = 0; i < kNumKinds; i++) {
        if (i) {
            const float w = ImGui::CalcTextSize(kKinds[i].label).x + 16.0f;
            if (ImGui::GetItemRectMax().x + 4.0f + w < row_right) ImGui::SameLine(0.0f, 4.0f);
        }
        if (filter_chip(kKinds[i].label, st.net_kind == i)) st.net_kind = i;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kKinds[i].tip);
    }
    band.end();

    // Reserved for the summary footer, measured the same way the console's prompt is.
    const float footer_h = ImGui::GetTextLineHeight() + 2.0f * kBandPad +
                           ImGui::GetStyle().ItemSpacing.y;
    const float avail_h = std::max(1.0f, ImGui::GetContentRegionAvail().y - footer_h);
    const float table_h = std::max(70.0f, avail_h * st.net_split - 4.0f);

    std::size_t shown = 0, total_bytes = 0, hidden_media = 0;
    double slowest_end = 0.0;
    bool any_pending = false;

    begin_surface("##dt_net_table", ImVec2(0, table_h));
    if (mono_font) ImGui::PushFont(mono_font);
    // No column rules: they would run the full height of the table, drawing a
    // grid through the empty space under the last row.
    const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("##dt_nets", 4, tf, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("St", ImGuiTableColumnFlags_WidthFixed, 34.0f);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 64.0f);
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 54.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (const NetRecord& r : st.nets) {
            if (!r.done) any_pending = true;
            if (st.net_kind == 0 && hidden_from_all(r)) hidden_media++;
            if (!net_passes(st, r)) continue;
            shown++;
            total_bytes += r.size;
            slowest_end = std::max(slowest_end, r.offset_ms + r.ms);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            // GET is the default; naming it on every row only costs name column width.
            std::string name = r.method == "GET" ? std::string() : r.method + " ";
            name += short_name(r.url);
            // The selectable spans every column, so its label is not clipped to the
            // Name column.
            name = elide_front(std::move(name), ImGui::GetContentRegionAvail().x);
            char label[512];
            std::snprintf(label, sizeof label, "%s##net%llu", name.c_str(),
                          (unsigned long long)r.id);
            if (ImGui::Selectable(label, st.net_selected == r.id,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                st.net_selected = r.id;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", r.url.c_str(), r.initiator);

            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, status_color(r));
            if (!r.done)            ImGui::TextUnformatted("...");
            else if (r.blocked)     ImGui::TextUnformatted("blk");
            else if (!r.success)    ImGui::TextUnformatted("err");
            else                    ImGui::Text("%d", r.status);
            ImGui::PopStyleColor();

            // A blocked row has no size and no duration; 0 would read as a measurement.
            ImGui::TableNextColumn();
            if (r.done && !r.blocked) ImGui::TextUnformatted(fmt_bytes(r.size).c_str());
            else                      ImGui::TextDisabled("-");

            ImGui::TableNextColumn();
            if (r.done && !r.blocked) ImGui::TextUnformatted(fmt_ms(r.ms).c_str());
            else                      ImGui::TextDisabled("-");
        }
        if (st.net_scroll_to_end) {
            ImGui::SetScrollHereY(1.0f);
            st.net_scroll_to_end = false;
        }
        ImGui::EndTable();
    }
    if (mono_font) ImGui::PopFont();
    if (shown == 0) {
        ImGui::TextDisabled(st.nets.empty() ? "No requests recorded. Reload the page."
                                            : "Nothing matches this filter.");
    }
    end_surface();

    ImGui::InvisibleButton("##dt_net_split", ImVec2(-1.0f, 7.0f));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
    if (ImGui::IsItemActive() && avail_h > 1.0f) {
        st.net_split = std::clamp(st.net_split + ImGui::GetIO().MouseDelta.y / avail_h,
                                  0.15f, 0.85f);
    }
    {
        ImVec2 gm = ImGui::GetItemRectMin(), gx = ImGui::GetItemRectMax();
        float y = std::round((gm.y + gx.y) * 0.5f);
        float cx = (gm.x + gx.x) * 0.5f;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(cx - 14.0f, y), ImVec2(cx + 14.0f, y),
                                            Theme::dt_grip, 1.0f);
    }

    begin_surface("##dt_net_detail", ImVec2(0, -footer_h));
    const NetRecord* sel = nullptr;
    for (const NetRecord& r : st.nets)
        if (r.id == st.net_selected) { sel = &r; break; }
    if (!sel) {
        ImGui::TextDisabled("Select a request.");
    } else {
        draw_net_selected(*sel, st);
    }
    end_surface();

    ControlRow footer;
    footer.begin();
    char summary[192];
    if (hidden_media > 0) {
        std::snprintf(summary, sizeof summary, "%zu requests. %s. %s.  (%zu media hidden)",
                      shown, fmt_bytes(total_bytes).c_str(), fmt_ms(slowest_end).c_str(),
                      hidden_media);
    } else {
        std::snprintf(summary, sizeof summary, "%zu requests. %s. %s", shown,
                      fmt_bytes(total_bytes).c_str(), fmt_ms(slowest_end).c_str());
    }
    ImGui::TextColored(Theme::dt_dim, "%s", summary);
    footer.end();

    // The idle loop would otherwise sleep through a request completing, leaving a
    // row reading "..." until something else happened to wake the window.
    st.net_wants_frames = any_pending;
}

// Sources.

// Only Text goes through the line viewer; the rest are decoded assets.
enum class SrcKind { Text, Image, Media, Font };

// A row in the file list. Everything borrowed here lives for one draw.
struct SourceFile {
    const char* group = nullptr;
    // The resolved URL, or "page" for an inline chunk; what src_pin and a console
    // error's `source` hold.
    std::string key;
    std::string label;
    const std::string* text = nullptr;  // Text: null for a script whose fetch failed
    SrcKind kind = SrcKind::Text;
    const std::string* data = nullptr;  // Image: bytes as they arrived
    TextureInfo tex;
    bool favicon = false;
    // Media: the page's own player, null until the element starts one. Never one
    // of our own: a URL is a single range cache, so a second reader would clash.
    VideoPlayer* player = nullptr;
    ImFont* font = nullptr;  // Font
};

void collect_sources(const Tab& tab, std::vector<SourceFile>& out) {
    const FetchResult& page = tab.active_page;
    if (!page.body.empty()) {
        SourceFile f;
        f.group = "Document";
        f.key = tab.current_url;
        f.label = short_name(tab.current_url);
        f.text = &page.body;
        out.push_back(std::move(f));
    }
    for (const PageScript& sc : page.scripts) {
        // "page" is the chunk name run_page_scripts passes, so an error's source
        // lines up with a row here.
        const bool inline_chunk = sc.src.empty();
        SourceFile f;
        f.group = "Scripts";
        f.key = inline_chunk ? std::string("page") : sc.src;
        f.label = inline_chunk ? std::string("page") : short_name(sc.src);
        f.text = sc.source.empty() ? nullptr : &sc.source;
        out.push_back(std::move(f));
    }
    for (const auto& sheet : page.stylesheets) {
        const bool inline_sheet = sheet.first == "(inline)";
        SourceFile f;
        f.group = "Stylesheets";
        f.key = sheet.first;
        f.label = inline_sheet ? sheet.first : short_name(sheet.first);
        f.text = &sheet.second;
        out.push_back(std::move(f));
    }

    // fetched_images is a hash map, and a list that reshuffles between frames is
    // unusable.
    std::vector<const std::string*> img_urls;
    for (const auto& kv : page.fetched_images) img_urls.push_back(&kv.first);
    std::sort(img_urls.begin(), img_urls.end(),
              [](const std::string* a, const std::string* b) { return *a < *b; });
    for (const std::string* url : img_urls) {
        SourceFile f;
        f.group = "Images";
        f.key = *url;
        f.label = short_name(*url);
        f.kind = SrcKind::Image;
        f.data = &page.fetched_images.at(*url);
        auto tex = tab.page_textures.find(*url);
        if (tex != tab.page_textures.end()) f.tex = tex->second;
        out.push_back(std::move(f));
    }
    // Kept apart from page_textures: it outlives the page it came from.
    if (!page.favicon_bytes.empty()) {
        SourceFile f;
        f.group = "Images";
        f.key = page.favicon_url.empty() ? std::string("favicon") : page.favicon_url;
        f.label = page.favicon_url.empty() ? std::string("favicon")
                                           : short_name(page.favicon_url);
        f.kind = SrcKind::Image;
        f.data = &page.favicon_bytes;
        f.tex = tab.favicon;
        f.favicon = true;
        out.push_back(std::move(f));
    }

    // From the live DOM rather than the fetch: media never lands in a body.
    std::vector<std::string> media;
    find_media_in_dom(tab.page_dom, media);
    std::unordered_set<std::string> seen;
    for (const std::string& src : media) {
        std::string url = resolve_url(tab.current_url, src);
        if (!seen.insert(url).second) continue;
        SourceFile f;
        f.group = "Media";
        f.label = short_name(url);
        f.kind = SrcKind::Media;
        auto player = tab.active_players.find(url);
        if (player != tab.active_players.end()) f.player = player->second;
        f.key = std::move(url);
        out.push_back(std::move(f));
    }

    // Only once something is loaded, so an empty tab still reads as empty. These
    // are the bundled faces a page can select; there is no @font-face to fetch.
    if (!out.empty()) {
        for (const auto& face : page_font_faces()) {
            SourceFile f;
            f.group = "Fonts";
            f.key = "font:" + face.first;
            f.label = face.first;
            f.kind = SrcKind::Font;
            f.font = face.second;
            out.push_back(std::move(f));
        }
    }
}

// Magic numbers, not the content type: the server's guess and the decoder's
// answer disagree too often.
const char* image_format(const std::string& b) {
    auto at = [&](const char* sig, std::size_t n, std::size_t off) {
        return b.size() >= off + n && std::memcmp(b.data() + off, sig, n) == 0;
    };
    if (at("\x89PNG", 4, 0)) return "PNG";
    if (at("\xFF\xD8\xFF", 3, 0)) return "JPEG";
    if (at("GIF8", 4, 0)) return "GIF";
    if (at("RIFF", 4, 0) && at("WEBP", 4, 8)) return "WebP";
    if (at("BM", 2, 0)) return "BMP";
    if (at("\x00\x00\x01\x00", 4, 0)) return "ICO";
    // SVG is text, so the loader sniffs for the tag rather than a magic number.
    const std::size_t svg = b.find("<svg");
    if (svg != std::string::npos && svg < 512) return "SVG";
    return "unknown";
}

// Behind a preview, so a transparent PNG reads as transparent.
void checkerboard(ImVec2 mn, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 mx(mn.x + size.x, mn.y + size.y);
    const float c = 8.0f;
    dl->PushClipRect(mn, mx, true);
    dl->AddRectFilled(mn, mx, IM_COL32(56, 56, 62, 255));
    for (int row = 0; mn.y + row * c < mx.y; row++) {
        for (int col = (row & 1); mn.x + col * c < mx.x; col += 2) {
            dl->AddRectFilled(ImVec2(mn.x + col * c, mn.y + row * c),
                              ImVec2(mn.x + (col + 1) * c, mn.y + (row + 1) * c),
                              IM_COL32(40, 40, 45, 255));
        }
    }
    dl->PopClipRect();
}

std::string fmt_clock(double secs) {
    if (secs < 0.0 || secs != secs) return "-";
    const int t = (int)secs;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d:%02d", t / 60, t % 60);
    return buf;
}

void asset_table(const char* id, const std::function<void()>& rows) {
    if (ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthStretch, 0.36f);
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch, 0.64f);
        rows();
        ImGui::EndTable();
    }
}

// What is left for a preview once the table below it has its room. A reservation,
// not a measurement: the rows are not laid out yet.
float preview_height_budget() {
    return std::max(120.0f, ImGui::GetContentRegionAvail().y -
                                6.0f * ImGui::GetTextLineHeightWithSpacing());
}

void draw_image_view(const SourceFile& f) {
    const float avail = ImGui::GetContentRegionAvail().x;
    if (f.tex.id == 0 || f.tex.width <= 0) {
        ImGui::TextColored(Theme::dt_dim, "No preview: the bytes did not decode.");
    } else {
        // Never scaled up: a 16x16 icon stretched across the pane misreports it.
        const float scale = std::min({1.0f, avail / (float)f.tex.width,
                                      preview_height_budget() / (float)f.tex.height});
        const ImVec2 size(std::floor(f.tex.width * scale), std::floor(f.tex.height * scale));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 at(std::round(p.x + (avail - size.x) * 0.5f), p.y);
        ImGui::SetCursorScreenPos(at);
        checkerboard(at, size);
        ImGui::Image((void*)(intptr_t)f.tex.id, size);
        if (scale < 1.0f) {
            ImGui::TextColored(Theme::dt_dim, "Shown at %d%%", (int)std::round(scale * 100.0f));
        }
    }

    ImGui::Spacing();
    asset_table("##dt_src_img", [&] {
        kv_row("URL", f.key);
        if (f.data) kv_row("Format", image_format(*f.data));
        if (f.tex.id != 0) {
            kv_row("Size", std::to_string(f.tex.width) + " x " + std::to_string(f.tex.height));
        }
        if (f.data) kv_row("Bytes", fmt_bytes(f.data->size()));
        if (f.favicon) kv_row("Role", "favicon");
    });
}

void draw_media_view(const SourceFile& f) {
    VideoPlayer* p = f.player;
    if (p && !p->is_audio_only() && p->get_texture_id() != 0 && p->get_width() > 0) {
        const float avail = ImGui::GetContentRegionAvail().x;
        const float scale = std::min({1.0f, avail / (float)p->get_width(),
                                      preview_height_budget() / (float)p->get_height()});
        const ImVec2 size(std::floor(p->get_width() * scale), std::floor(p->get_height() * scale));
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(std::round(at.x + (avail - size.x) * 0.5f), at.y));
        ImGui::Image((void*)(intptr_t)p->get_texture_id(), size);
        ImGui::Spacing();
    }

    asset_table("##dt_src_media", [&] {
        kv_row("URL", f.key);
        if (!p) {
            kv_row("State", "not started");
        } else if (p->has_error()) {
            kv_row("State", "failed to decode");
        } else {
            kv_row("Kind", p->is_audio_only() ? "audio" : "video");
            if (p->get_width() > 0) {
                kv_row("Size", std::to_string(p->get_width()) + " x " +
                                   std::to_string(p->get_height()));
            }
            kv_row("Position", fmt_clock(p->get_current_time()) + " / " +
                                   fmt_clock(p->get_duration()));
            kv_row("State", p->is_playing() ? "playing" : "paused");
        }
    });
}

void draw_font_view(const SourceFile& f) {
    if (f.font) ImGui::PushFont(f.font);
    ImGui::SetWindowFontScale(1.9f);
    ImGui::TextUnformatted(f.label.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted("The quick brown fox jumps over the lazy dog. 0123456789");
    ImGui::PopTextWrapPos();
    if (f.font) ImGui::PopFont();

    ImGui::Spacing();
    if (mono_font) ImGui::PushFont(mono_font);
    ImGui::TextColored(Theme::dt_dim, "font-family: %s;", f.label.c_str());
    if (mono_font) ImGui::PopFont();
}

void open_source(TabState& st, const std::string& key, int line) {
    st.panel = Panel::Sources;
    st.src_pin = key;
    st.src_scroll_to_pin = true;
    st.src_hl_line = line;
    st.src_goto_line = line;
}

// Splits the file into lines and measures each one. Wrapping makes rows a
// variable height, which ImGuiListClipper cannot do; src_tops takes its place.
void rebuild_source_cache(TabState& st, const SourceFile& f, float wrap_w) {
    const std::size_t len = f.text ? f.text->size() : 0;
    if (st.src_key == f.key && st.src_key_ptr == f.text && st.src_key_len == len &&
        st.src_key_wrap == st.src_wrap &&
        (!st.src_wrap || std::fabs(st.src_key_w - wrap_w) < 0.5f)) {
        return;
    }
    st.src_key = f.key;
    st.src_key_ptr = f.text;
    st.src_key_len = len;
    st.src_key_wrap = st.src_wrap;
    st.src_key_w = wrap_w;
    st.src_hits_dirty = true;
    st.src_lines.clear();

    if (f.text) {
        for (std::size_t start = 0; start <= f.text->size();) {
            const std::size_t nl = f.text->find('\n', start);
            std::string line = f.text->substr(
                start, nl == std::string::npos ? std::string::npos : nl - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            // ImGui has no tab stops, so a literal tab is one narrow glyph.
            for (std::size_t t = line.find('\t'); t != std::string::npos;
                 t = line.find('\t', t + 4)) {
                line.replace(t, 1, "    ");
            }
            st.src_lines.push_back(std::move(line));
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
    }

    // Measured under the font the viewer draws in; the caller pushes mono first.
    const float line_h = ImGui::GetTextLineHeight();
    st.src_tops.assign(st.src_lines.size() + 1, 0.0f);
    st.src_widest = 0.0f;
    float y = 0.0f;
    for (std::size_t i = 0; i < st.src_lines.size(); i++) {
        st.src_tops[i] = y;
        const char* b = st.src_lines[i].c_str();
        const char* e = b + st.src_lines[i].size();
        if (st.src_wrap) {
            y += std::max(line_h, ImGui::CalcTextSize(b, e, false, wrap_w).y);
        } else {
            st.src_widest = std::max(st.src_widest, ImGui::CalcTextSize(b, e).x);
            y += line_h;
        }
    }
    st.src_tops.back() = y;
}

void rebuild_source_hits(TabState& st) {
    if (!st.src_hits_dirty) return;
    st.src_hits_dirty = false;
    st.src_hits.clear();
    if (st.src_find[0] == '\0') return;

    auto fold = [](std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    };
    const std::string needle = fold(st.src_find);
    for (std::size_t i = 0; i < st.src_lines.size(); i++) {
        if (fold(st.src_lines[i]).find(needle) != std::string::npos) {
            st.src_hits.push_back((int)i);
        }
    }
    if (st.src_hit >= (int)st.src_hits.size()) st.src_hit = 0;
}

void draw_source_view(TabState& st, const SourceFile* f) {
    if (!f) {
        ImGui::TextDisabled("Select a file.");
        return;
    }
    switch (f->kind) {
        case SrcKind::Image: draw_image_view(*f); return;
        case SrcKind::Media: draw_media_view(*f); return;
        case SrcKind::Font:  draw_font_view(*f);  return;
        case SrcKind::Text:  break;
    }
    if (mono_font) ImGui::PushFont(mono_font);

    // From the raw newline count, not src_lines: the split needs the wrap width,
    // which needs the gutter.
    const std::size_t nlines =
        f->text ? (std::size_t)std::count(f->text->begin(), f->text->end(), '\n') + 1 : 1;
    char widest[24];
    std::snprintf(widest, sizeof widest, "%zu", nlines);
    const float gutter = ImGui::CalcTextSize(widest).x + 16.0f;
    const float wrap_w = std::max(40.0f, ImGui::GetContentRegionAvail().x - gutter);

    rebuild_source_cache(st, *f, wrap_w);
    rebuild_source_hits(st);

    if (!f->text) {
        if (mono_font) ImGui::PopFont();
        ImGui::TextDisabled("This script failed to load.");
        return;
    }

    const int n = (int)st.src_lines.size();
    const float view_h = ImGui::GetWindowHeight();
    if (st.src_goto_line > 0 && n > 0) {
        const int idx = std::clamp(st.src_goto_line - 1, 0, n - 1);
        // A third down, not at the top: the lines above an error matter too.
        ImGui::SetScrollY(std::max(0.0f, st.src_tops[idx] - view_h * 0.33f));
        st.src_goto_line = 0;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    // Only visible rows are emitted, so the scrollbar needs the extent up front.
    if (!st.src_wrap && n > 0) ImGui::Dummy(ImVec2(gutter + st.src_widest, 0.0f));

    const float scroll = ImGui::GetScrollY();
    int first = 0, last = n;
    if (n > 0) {
        const auto b = st.src_tops.begin();
        first = std::max(0, (int)(std::upper_bound(b, b + n, scroll) - b) - 1);
        last = std::min(n, (int)(std::lower_bound(b, b + n, scroll + view_h) - b) + 1);
    }

    if (first > 0) ImGui::Dummy(ImVec2(0.0f, st.src_tops[first]));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // From the window, not the cursor, so a band stays put under a sideways scroll.
    const float x0 = ImGui::GetWindowPos().x + 1.0f;
    const float x1 = x0 + ImGui::GetWindowWidth() - 2.0f;
    for (int i = first; i < last; i++) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = st.src_tops[i + 1] - st.src_tops[i];
        const bool marked = (i + 1) == st.src_hl_line;
        const bool hit = std::binary_search(st.src_hits.begin(), st.src_hits.end(), i);
        if (marked) {
            dl->AddRectFilled(ImVec2(x0, p.y), ImVec2(x1, p.y + h), Theme::dt_row_selected);
        } else if (hit) {
            dl->AddRectFilled(ImVec2(x0, p.y), ImVec2(x1, p.y + h), Theme::dt_row_hover);
        }

        char num[24];
        std::snprintf(num, sizeof num, "%d", i + 1);
        dl->AddText(ImVec2(std::round(p.x + gutter - 8.0f - ImGui::CalcTextSize(num).x), p.y),
                    marked ? Theme::dt_text_on : Theme::dt_text_off, num);

        ImGui::SetCursorScreenPos(ImVec2(p.x + gutter, p.y));
        if (st.src_wrap) ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_w);
        ImGui::TextUnformatted(st.src_lines[i].c_str());
        if (st.src_wrap) ImGui::PopTextWrapPos();
    }
    if (last < n) ImGui::Dummy(ImVec2(0.0f, st.src_tops[n] - st.src_tops[last]));

    ImGui::PopStyleVar();
    if (mono_font) ImGui::PopFont();
}

void draw_sources(Tab& tab, TabState& st) {
    std::vector<SourceFile> files;
    collect_sources(tab, files);

    if (!st.src_want.empty()) {
        for (const SourceFile& f : files) {
            if (f.key.find(st.src_want) == std::string::npos) continue;
            open_source(st, f.key, st.src_want_line);
            st.src_want.clear();
            break;
        }
    }

    const SourceFile* sel = nullptr;
    for (const SourceFile& f : files)
        if (f.key == st.src_pin) { sel = &f; break; }
    if (!sel && !files.empty()) {
        // The pin is from a page that has since been replaced.
        sel = &files[0];
        st.src_pin = sel->key;
        st.src_hl_line = 0;
    }

    // An asset has no lines, so wrapping and find have nothing to act on.
    const bool textual = !sel || sel->kind == SrcKind::Text;

    ControlRow band;
    band.begin();
    if (textual) {
        if (tool_button("Wrap", st.src_wrap)) {
            st.src_wrap = !st.src_wrap;
            if (st.src_wrap) ImGui::SetScrollX(0.0f);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Wrap long lines to the pane");
        ImGui::SameLine(0.0f, 4.0f);
    }
    if (tool_button("Copy") && sel) {
        ImGui::SetClipboardText(textual ? (sel->text ? sel->text->c_str() : "")
                                        : sel->key.c_str());
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(textual ? "Copy this file to the clipboard"
                                  : "Copy this URL to the clipboard");
    }
    if (textual) {
        ImGui::SameLine(0.0f, 8.0f);
        if (field_input("##dt_src_find", "Find in file", st.src_find,
                        IM_ARRAYSIZE(st.src_find))) {
            st.src_hits_dirty = true;
            st.src_hit = 0;
        }
    }

    if (textual && st.src_find[0] != '\0') {
        auto step = [&](int by) {
            if (st.src_hits.empty()) return;
            const int n = (int)st.src_hits.size();
            st.src_hit = (st.src_hit + by + n) % n;
            st.src_hl_line = st.src_hits[st.src_hit] + 1;
            st.src_goto_line = st.src_hl_line;
        };
        if (tool_button("Prev")) step(-1);
        ImGui::SameLine(0.0f, 4.0f);
        if (tool_button("Next")) step(1);
        ImGui::SameLine(0.0f, 8.0f);
        char count[64];
        if (st.src_hits.empty()) std::snprintf(count, sizeof count, "no matches");
        else std::snprintf(count, sizeof count, "%d / %d lines", st.src_hit + 1,
                           (int)st.src_hits.size());
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(Theme::dt_dim, "%s", count);
    }
    band.end();

    // Reserved for the summary footer, measured the way the network panel's is.
    const float footer_h = ImGui::GetTextLineHeight() + 2.0f * kBandPad +
                           ImGui::GetStyle().ItemSpacing.y;
    const float avail_h = std::max(1.0f, ImGui::GetContentRegionAvail().y - footer_h);
    const float list_h = std::max(60.0f, avail_h * st.src_split - 4.0f);

    begin_surface("##dt_src_files", ImVec2(0, list_h));
    if (files.empty()) {
        ImGui::TextDisabled("Nothing loaded. Open a page.");
    }
    const char* group = nullptr;
    for (int i = 0; i < (int)files.size(); i++) {
        const SourceFile& f = files[i];
        if (group != f.group) {
            group = f.group;
            if (i) ImGui::Spacing();
            ImGui::TextColored(Theme::dt_dim, "%s", group);
        }
        std::string size;
        bool bad = false;
        switch (f.kind) {
            case SrcKind::Text:
                size = f.text ? fmt_bytes(f.text->size()) : std::string("failed");
                bad = !f.text;
                break;
            case SrcKind::Image:
                size = f.data ? fmt_bytes(f.data->size()) : std::string("failed");
                bad = f.tex.id == 0;
                break;
            case SrcKind::Media: size = "stream"; break;
            case SrcKind::Font:  size = "face"; break;
        }
        const float size_w = ImGui::CalcTextSize(size.c_str()).x;
        char label[512];
        std::snprintf(label, sizeof label, "%s##src%d",
                      elide_front(f.label, ImGui::GetContentRegionAvail().x - size_w - 10.0f)
                          .c_str(),
                      i);
        if (ImGui::Selectable(label, sel == &f)) {
            st.src_pin = f.key;
            st.src_hl_line = 0;
            st.src_hit = 0;
            st.src_scroll_to_pin = false;
        }
        if (sel == &f && st.src_scroll_to_pin) {
            ImGui::SetScrollHereY(0.5f);
            st.src_scroll_to_pin = false;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f.key.c_str());
        // Centred in the row: a Selectable's rect is grown by half an ItemSpacing
        // on each side.
        const ImVec2 rmn = ImGui::GetItemRectMin(), rmx = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(rmx.x - size_w,
                   std::round((rmn.y + rmx.y - ImGui::GetTextLineHeight()) * 0.5f)),
            bad ? IM_COL32(242, 115, 115, 255) : Theme::dt_text_off, size.c_str());
    }
    end_surface();

    ImGui::InvisibleButton("##dt_src_split", ImVec2(-1.0f, 7.0f));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
    if (ImGui::IsItemActive() && avail_h > 1.0f) {
        st.src_split = std::clamp(st.src_split + ImGui::GetIO().MouseDelta.y / avail_h,
                                  0.12f, 0.80f);
    }
    {
        const ImVec2 gm = ImGui::GetItemRectMin(), gx = ImGui::GetItemRectMax();
        const float y = std::round((gm.y + gx.y) * 0.5f);
        const float cx = (gm.x + gx.x) * 0.5f;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(cx - 14.0f, y), ImVec2(cx + 14.0f, y),
                                            Theme::dt_grip, 1.0f);
    }

    // Forced on: the scrollbar width comes out of the wrap width, so letting it
    // appear with the content would size the wrap from the wrap.
    ImGuiWindowFlags vf = ImGuiWindowFlags_AlwaysVerticalScrollbar;
    if (textual && !st.src_wrap) vf |= ImGuiWindowFlags_HorizontalScrollbar;
    begin_surface("##dt_src_view", ImVec2(0, -footer_h), vf);
    draw_source_view(st, sel);
    end_surface();

    ControlRow footer;
    footer.begin();
    std::string summary;
    if (sel) {
        switch (sel->kind) {
            case SrcKind::Text:
                if (sel->text) {
                    summary = sel->label + "  -  " + std::to_string(st.src_lines.size()) +
                              " lines, " + fmt_bytes(sel->text->size());
                }
                break;
            case SrcKind::Image:
                summary = sel->label + "  -  " +
                          (sel->data ? std::string(image_format(*sel->data)) + ", " : "") +
                          (sel->tex.id != 0 ? std::to_string(sel->tex.width) + " x " +
                                                  std::to_string(sel->tex.height) + ", "
                                            : "") +
                          (sel->data ? fmt_bytes(sel->data->size()) : "no bytes");
                break;
            case SrcKind::Media:
                summary = sel->label + "  -  " +
                          (sel->player ? std::string(sel->player->is_audio_only() ? "audio"
                                                                                  : "video") +
                                             ", " + fmt_clock(sel->player->get_duration())
                                       : std::string("not started"));
                break;
            case SrcKind::Font:
                summary = sel->label + "  -  bundled face";
                break;
        }
    }
    if (summary.empty()) summary = std::to_string(files.size()) + " files";
    ImGui::TextColored(Theme::dt_dim, "%s", summary.c_str());
    footer.end();
}

// Lua puts the position in the message itself ("chunk:12: attempt to ...").
int ref_line(const LogEntry& e) {
    if (e.line > 0) return e.line;
    if (e.source.empty()) return 0;
    if (e.text.compare(0, e.source.size(), e.source) != 0) return 0;
    std::size_t i = e.source.size();
    if (i >= e.text.size() || e.text[i] != ':') return 0;
    int n = 0;
    for (i++; i < e.text.size() && std::isdigit((unsigned char)e.text[i]); i++) {
        n = n * 10 + (e.text[i] - '0');
    }
    return n;
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
    // Trim; an empty prompt is just a keystroke, not a command.
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
    ControlRow band;
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

    // Measured in mono (a point smaller than the UI font, what the row draws in):
    // mismatched and the prompt either falls short of the dock's bottom edge or
    // pushes the log pane off it.
    if (mono_font) ImGui::PushFont(mono_font);
    const float prompt_h = ImGui::GetFrameHeight() + 2.0f * kBandPad +
                           ImGui::GetStyle().ItemSpacing.y;
    if (mono_font) ImGui::PopFont();
    begin_surface("##dt_log", ImVec2(0, -prompt_h));
    if (mono_font) ImGui::PushFont(mono_font);
    // Collected once, not per row: the log can run to thousands of lines.
    std::vector<SourceFile> files;
    collect_sources(tab, files);
    auto jumpable = [&files](const std::string& key) {
        for (const SourceFile& f : files)
            if (f.key == key && f.text) return true;
        return false;
    };

    bool any = false;
    int row = 0;
    for (const LogEntry& e : st.logs) {
        row++;
        if (!passes_filter(st, e)) continue;
        any = true;
        ImGui::PushStyleColor(ImGuiCol_Text, level_color(e.level));
        ImGui::TextWrapped("%s%s", level_prefix(e.level), e.text.c_str());
        ImGui::PopStyleColor();
        if (e.source.empty()) continue;

        const int line = ref_line(e);
        char ref[600];
        if (line > 0) std::snprintf(ref, sizeof ref, "%s:%d", e.source.c_str(), line);
        else          std::snprintf(ref, sizeof ref, "%s", e.source.c_str());
        // Drawn by hand: the colour depends on a hover the item reports only once
        // it exists.
        const float lead = ImGui::CalcTextSize("      ").x;
        const ImVec2 ts = ImGui::CalcTextSize(ref);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        char id[32];
        std::snprintf(id, sizeof id, "##dt_ref%d", row);
        ImGui::InvisibleButton(id, ImVec2(lead + ts.x, ts.y));
        const bool hot = jumpable(e.source) && ImGui::IsItemHovered();
        if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const ImU32 col = hot ? ImGui::ColorConvertFloat4ToU32(Theme::dt_accent)
                              : Theme::dt_text_off;
        ImDrawList* rdl = ImGui::GetWindowDrawList();
        rdl->AddText(ImVec2(p.x + lead, p.y), col, ref);
        if (hot) {
            rdl->AddLine(ImVec2(p.x + lead, p.y + ts.y - 1.0f),
                         ImVec2(p.x + lead + ts.x, p.y + ts.y - 1.0f), col, 1.0f);
            if (ImGui::IsItemClicked()) open_source(st, e.source, line);
        }
    }
    if (mono_font) ImGui::PopFont();
    if (!any) ImGui::TextDisabled("Nothing logged yet.");
    if (st.scroll_to_end) {
        ImGui::SetScrollHereY(1.0f);
        st.scroll_to_end = false;
    }
    end_surface();

    ControlRow prompt;
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
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Theme::dt_field_bg);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Theme::dt_field_bg);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(8.0f, ImGui::GetStyle().FramePadding.y));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, kDtRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushItemWidth(-kTabInset);
    const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue |
                                      ImGuiInputTextFlags_CallbackHistory;
    if (ImGui::InputTextWithHint("##dt_eval", "Run Lua in this page", st.input,
                                 IM_ARRAYSIZE(st.input), flags,
                                 console_input_callback, &st)) {
        submit_console(st, tab.id);
    }
    const bool eval_focused = ImGui::IsItemActive();
    const bool eval_hovered = ImGui::IsItemHovered();
    ImGui::PopItemWidth();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(3);
    outline_field(eval_focused, eval_hovered);
    if (mono_font) ImGui::PopFont();
    if (st.focus_input) {
        ImGui::SetKeyboardFocusHere(-1);
        st.focus_input = false;
    }
    prompt.end();
}

}  // namespace
}  // namespace devtools
