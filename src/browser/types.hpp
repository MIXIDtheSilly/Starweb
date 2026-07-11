#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include "imgui.h"

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
    
    float border_width = 0.0f;
    ImVec4 border_color = ImVec4(0, 0, 0, 0);
    bool has_border_color = false;
    
    float font_size = 1.0f;
    std::string display = "";
};

struct DomNode {
    std::string tag;
    std::string class_name;
    std::string id;
    std::string onclick;
    std::string href;
    std::string text_content;
    std::string type;
    std::string value;
    std::string placeholder;
    std::string inline_style;
    bool has_inline_style = false;
    CssStyle parsed_inline_style;
    std::vector<DomNode> children;
};

struct FetchResult {
    bool success = false;
    int status_code = 0;
    std::string status_text;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    std::string error_message;
    DomNode dom;
    std::unordered_map<std::string, CssStyle> css_classes;
};

struct Tab {
    int id = 0;
    std::string current_url = "moon://localhost/index.html";
    char url_input[512] = "moon://localhost/index.html";
    std::string status_text = "Idle";
    bool is_fetching = false;
    
    std::vector<std::string> navigation_history;
    int history_index = -1;
    
    FetchResult active_page;
    bool new_page_ready = false;
    int active_socket_fd = -1;
    
    DomNode page_dom;
    std::unordered_map<std::string, CssStyle> css_classes;
    std::string alert_text = "";
    bool show_alert = false;
    bool reset_scroll_next_frame = false;
    
    std::string title = "New Tab";
};
