#include "renderer.hpp"
#include "parser.hpp"
#include "fetcher.hpp"
#include "globals.hpp"
#include "../common/url_parser.hpp"
#include <cmath>
#include <cstring>

InputStyleGuard::InputStyleGuard(const CssStyle& merged) {
    float rounding = merged.border_radius >= 0.0f ? merged.border_radius : 0.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, rounding);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, merged.border_width >= 0.0f ? merged.border_width : 1.0f);
    
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
    ImGui::PopStyleVar(2);
}

bool is_inline_element(const DomNode& node, const CssStyle& merged) {
    if (merged.display == "inline" || merged.display == "inline-block") return true;
    if (merged.display == "block") return false;
    
    if (node.tag == "span" || node.tag == "a" || node.tag == "button" || 
        node.tag == "input" || node.tag == "textarea" || node.tag == "select" || node.tag == "option") {
        return true;
    }
    return false;
}

void render_node(DomNode& node, const CssStyle& parent_style, bool& is_inline_flow, Tab& tab, int li_index, float parent_accumulated_right) {
    if (node.tag == "script" || node.tag == "style" || node.tag == "head" || node.tag == "title" || node.tag == "meta" || node.tag == "option") {
        return;
    }

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
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + merged.margin_top);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + merged.margin_left);
        
        content_start = ImGui::GetCursorScreenPos();
        
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + merged.padding_top);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + merged.padding_left);
        
        splitter.Split(draw_list, 2);
        splitter.SetCurrentChannel(draw_list, 1);
    } else {
        if (merged.margin_top > 0.0f) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + merged.margin_top);
        if (merged.margin_left > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + merged.margin_left);
        if (merged.padding_top > 0.0f) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + merged.padding_top);
        if (merged.padding_left > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + merged.padding_left);
    }

    ImGui::BeginGroup();

    float child_accumulated_right = parent_accumulated_right + merged.margin_right + merged.padding_right;
    if (node.tag == "div") {
        bool child_inline_flow = false;
        for (auto& child : node.children) {
            render_node(child, merged, child_inline_flow, tab, -1, child_accumulated_right);
        }
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
        
        bool child_inline_flow = true;
        float child_accumulated_right = parent_accumulated_right + merged.margin_right + merged.padding_right;
        for (auto& child : node.children) {
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
            if (!node.onclick.empty()) {
                tab.alert_text = extract_alert_message(node.onclick);
                tab.show_alert = true;
            } else {
                tab.alert_text = "Button clicked.";
                tab.show_alert = true;
            }
        }
        
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(4);
    } else if (node.tag == "a") {
        std::string cleaned_text = collapse_whitespace(node.text_content);
        ImVec4 link_color = merged.has_color ? merged.color : ImVec4(0.1f, 0.3f, 0.85f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, link_color);
        ImGui::Text("%s", cleaned_text.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImVec2 min_pos = ImGui::GetItemRectMin();
            ImVec2 max_pos = ImGui::GetItemRectMax();
            min_pos.y = max_pos.y;
            ImGui::GetWindowDrawList()->AddLine(min_pos, max_pos, ImGui::ColorConvertFloat4ToU32(link_color));
            
            if (ImGui::IsItemClicked()) {
                std::string new_url = node.href;
                if (new_url.find("://") == std::string::npos) {
                    auto opt_curr = parse_url(tab.current_url);
                    if (opt_curr) {
                        if (!new_url.empty() && new_url[0] != '/') {
                            new_url = "/" + new_url;
                        }
                        new_url = opt_curr->scheme + "://" + opt_curr->host + ":" + std::to_string(opt_curr->port) + new_url;
                    }
                }
                start_async_fetch(tab.id, new_url);
            }
        }
        ImGui::PopStyleColor();
    } else if (node.tag == "hr") {
        ImGui::Separator();
        ImGui::Spacing();
    } else if (node.tag == "input") {
        std::string type = node.type;
        if (type.empty() || type == "text" || type == "password") {
            char buf[1024] = {0};
            std::strncpy(buf, node.value.c_str(), sizeof(buf) - 1);
            
            float width = merged.width > 0.0f ? merged.width : 200.0f;
            ImGui::PushItemWidth(width);
            
            ImGuiInputTextFlags flags = 0;
            if (node.type == "password") {
                flags |= ImGuiInputTextFlags_Password;
            }
            
            std::string input_label = "##" + (node.id.empty() ? std::to_string((uintptr_t)&node) : node.id);
            
            {
                InputStyleGuard style_guard(merged);
                if (ImGui::InputTextWithHint(input_label.c_str(), node.placeholder.c_str(), buf, sizeof(buf), flags)) {
                    node.value = buf;
                }
            }
            ImGui::PopItemWidth();
        }
    } else if (node.tag == "textarea") {
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
    } else {
        bool child_inline_flow = false;
        float child_accumulated_right = parent_accumulated_right + merged.margin_right + merged.padding_right;
        for (auto& child : node.children) {
            render_node(child, merged, child_inline_flow, tab, -1, child_accumulated_right);
        }
    }

    ImGui::EndGroup();

    if (draw_bg) {
        ImVec2 min_p = content_start;
        ImVec2 max_p = ImGui::GetItemRectMax();
        
        max_p.x += merged.padding_right;
        max_p.y += merged.padding_bottom;
        
        if (merged.width > 0.0f) max_p.x = min_p.x + merged.width;
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
        if (merged.padding_bottom > 0.0f) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + merged.padding_bottom);
        if (merged.margin_bottom > 0.0f) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + merged.margin_bottom);
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
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
