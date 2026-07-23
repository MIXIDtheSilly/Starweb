#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include "imgui.h"
#include "../common/net.hpp"
#include "../common/tls_info.hpp"

struct CssStyle {
    ImVec4 color = ImVec4(1, 1, 1, 1);
    ImVec4 bg_color = ImVec4(0, 0, 0, 0);
    bool has_bg = false;
    bool has_color = false;
    float border_radius = -1.0f;
    ImVec4 gradient_start = ImVec4(0, 0, 0, 0);
    ImVec4 gradient_end = ImVec4(0, 0, 0, 0);
    bool has_gradient = false;
    std::string text_align = "left";
    
    float padding_left = 0.0f;
    float padding_right = 0.0f;
    float padding_top = 0.0f;
    float padding_bottom = 0.0f;
    
    float margin_left = 0.0f;
    float margin_right = 0.0f;
    float margin_top = 0.0f;
    float margin_bottom = 0.0f;
    
    float width = -1.0f;
    float height = -1.0f;
    // Viewport-relative width/height, as a percentage. Kept apart from the pixel
    // fields because the viewport is only known at render time; merge_node_style
    // folds them into width/height so nothing downstream has to care.
    float width_vw = -1.0f;
    float height_vh = -1.0f;

    float border_width = 0.0f;
    ImVec4 border_color = ImVec4(0, 0, 0, 0);
    bool has_border_color = false;
    
    float font_size = 1.0f;
    std::string display = "";

    // flexbox; empty string / -1 means unset
    std::string flex_direction = "";
    std::string justify_content = "";
    std::string align_items = "";
    std::string align_self = "";
    std::string flex_wrap = "";
    float row_gap = -1.0f;
    float column_gap = -1.0f;
    float flex_grow = -1.0f;
    float flex_shrink = -1.0f;
    float flex_basis = -1.0f;
};

struct CanvasOp {
    enum Kind { FillRect, StrokeRect, Line, Circle, Text, PolyFill };
    Kind kind;
    float a = 0, b = 0, c = 0, d = 0;
    ImVec4 color = ImVec4(1, 1, 1, 1);
    float line_width = 1.0f;
    bool fill = false;
    std::string text;
    std::vector<ImVec2> pts;
};

struct DomNode {
    uint64_t node_id = 0;
    std::string tag;
    std::string class_name;
    std::string id;
    std::string onclick;
    std::string href;
    std::string src;
    std::string text_content;
    std::string type;
    std::string value;
    std::string placeholder;
    std::string name;
    std::string min_val;
    std::string max_val;
    std::string step_val;
    bool checked = false;
    // Original values captured on first render, for form reset.
    std::string default_value;
    bool default_checked = false;
    bool defaults_captured = false;
    std::string inline_style;
    bool has_inline_style = false;
    CssStyle parsed_inline_style;
    std::vector<DomNode> children;
    
    // Media attributes
    bool autoplay = false;
    bool loop = false;
    bool controls = false;
    bool muted = false;
};

// A <script> from the page, kept in document order. Inline scripts arrive from the
// parser with `source` already filled and `src` empty; external ones arrive with only
// `src` set, and the fetcher resolves it and fills in `source` from the response.
// An external script whose fetch failed keeps an empty `source` and is not run.
struct PageScript {
    std::string src;
    std::string source;
};

struct FetchResult {
    bool success = false;
    int status_code = 0;
    std::string status_text;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    std::string error_message;
    // star:// only. tls_error gets an interstitial, not the generic error page.
    bool is_secure = false;
    bool tls_error = false;
    TlsInfo tls;
    DomNode dom;
    std::unordered_map<std::string, CssStyle> css_classes;
    std::unordered_map<std::string, std::string> fetched_images;
    std::vector<PageScript> scripts;
};

struct TextureInfo {
    unsigned int id = 0;
    int width = 0;
    int height = 0;
};

struct Tab {
    int id = 0;
    std::string current_url = "star://localhost/index.html";
    char url_input[512] = "star://localhost/index.html";
    std::string status_text = "Idle";
    bool is_fetching = false;
    
    std::vector<std::string> navigation_history;
    int history_index = -1;
    
    FetchResult active_page;
    bool new_page_ready = false;
    net::socket_t active_socket_fd = net::kInvalidSocket;
    
    DomNode page_dom;
    std::unordered_map<std::string, CssStyle> css_classes;
    std::string alert_text = "";
    bool show_alert = false;
    bool reset_scroll_next_frame = false;
    
    std::string title = "New Tab";
    // Radio-button group state: form control name -> selected node address.
    std::unordered_map<std::string, uintptr_t> radio_selection;
    std::unordered_map<std::string, TextureInfo> page_textures;
    std::unordered_map<std::string, class VideoPlayer*> active_players;

    // Viewport-fitting slack. An auto-height <canvas> or a `vh` length wants to
    // fill the viewport exactly, but the element sits inside a chain of wrapper
    // elements that each add item spacing after it, so filling it precisely
    // still overflows by a handful of pixels. The overflow is measured after the
    // frame and subtracted next time, converging in a frame or two. Reset when
    // the viewport is resized.
    float vp_slack = 0.0f;
    bool  vp_fit_used = false;
    float vp_last_h = 0.0f;
};
