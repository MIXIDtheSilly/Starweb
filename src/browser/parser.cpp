#include "parser.hpp"
#include <sstream>
#include <algorithm>
#include <cctype>

std::string trim_spaces(std::string_view str) {
    auto start = str.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return "";
    auto end = str.find_last_not_of(" \t\r\n");
    return std::string(str.substr(start, end - start + 1));
}

std::string collapse_whitespace(std::string_view str) {
    std::string result;
    result.reserve(str.size());
    bool last_was_space = false;
    
    auto start = str.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return "";
    auto end = str.find_last_not_of(" \t\r\n");
    
    for (size_t i = start; i <= end; ++i) {
        char c = str[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!last_was_space) {
                result += ' ';
                last_was_space = true;
            }
        } else {
            result += c;
            last_was_space = false;
        }
    }
    return result;
}

ImVec4 parse_color(std::string_view str) {
    std::string s = trim_spaces(str);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    
    if (s == "white") return ImVec4(1, 1, 1, 1);
    if (s == "black") return ImVec4(0, 0, 0, 1);
    if (s == "red") return ImVec4(1, 0, 0, 1);
    if (s == "green") return ImVec4(0, 1, 0, 1);
    if (s == "blue") return ImVec4(0, 0, 1, 1);
    if (s == "gray" || s == "grey") return ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
    if (s == "lightgray" || s == "lightgrey") return ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
    if (s == "darkgray" || s == "darkgrey") return ImVec4(0.25f, 0.25f, 0.25f, 1.0f);
    if (s == "yellow") return ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
    
    if (s.rfind("rgba", 0) == 0) {
        try {
            auto start = s.find('(');
            auto end = s.find(')');
            if (start != std::string::npos && end != std::string::npos) {
                std::string parts = s.substr(start + 1, end - start - 1);
                std::stringstream ss(parts);
                std::string r_s, g_s, b_s, a_s;
                std::getline(ss, r_s, ',');
                std::getline(ss, g_s, ',');
                std::getline(ss, b_s, ',');
                std::getline(ss, a_s, ',');
                return ImVec4(
                    std::stof(r_s) / 255.0f,
                    std::stof(g_s) / 255.0f,
                    std::stof(b_s) / 255.0f,
                    std::stof(a_s)
                );
            }
        } catch(...) {}
    }
    if (s.rfind("rgb", 0) == 0) {
        try {
            auto start = s.find('(');
            auto end = s.find(')');
            if (start != std::string::npos && end != std::string::npos) {
                std::string parts = s.substr(start + 1, end - start - 1);
                std::stringstream ss(parts);
                std::string r_s, g_s, b_s;
                std::getline(ss, r_s, ',');
                std::getline(ss, g_s, ',');
                std::getline(ss, b_s, ',');
                return ImVec4(
                    std::stof(r_s) / 255.0f,
                    std::stof(g_s) / 255.0f,
                    std::stof(b_s) / 255.0f,
                    1.0f
                );
            }
        } catch(...) {}
    }
    if (!s.empty() && s[0] == '#') {
        std::string hex = s.substr(1);
        if (hex.length() == 3) {
            std::string expanded = "";
            expanded += hex[0]; expanded += hex[0];
            expanded += hex[1]; expanded += hex[1];
            expanded += hex[2]; expanded += hex[2];
            hex = expanded;
        }
        if (hex.length() == 6) {
            try {
                unsigned int r = std::stoul(hex.substr(0, 2), nullptr, 16);
                unsigned int g = std::stoul(hex.substr(2, 2), nullptr, 16);
                unsigned int b = std::stoul(hex.substr(4, 2), nullptr, 16);
                return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
            } catch (...) {}
        }
    }
    return ImVec4(1, 1, 1, 1);
}

void parse_background(std::string_view value, CssStyle& style) {
    std::string val_str = trim_spaces(value);
    if (val_str.find("linear-gradient") != std::string::npos) {
        style.has_gradient = true;
        size_t first_hash = val_str.find('#');
        if (first_hash != std::string::npos) {
            size_t second_hash = val_str.find('#', first_hash + 1);
            if (second_hash != std::string::npos) {
                style.gradient_start = parse_color(val_str.substr(first_hash, 7));
                style.gradient_end = parse_color(val_str.substr(second_hash, 7));
            }
        }
    } else {
        style.has_bg = true;
        style.bg_color = parse_color(val_str);
    }
}

void parse_css_properties(const std::string& properties, CssStyle& style) {
    std::stringstream ss(properties);
    std::string prop;
    while (std::getline(ss, prop, ';')) {
        auto colon = prop.find(':');
        if (colon == std::string::npos) continue;
        std::string name = trim_spaces(prop.substr(0, colon));
        std::string val = trim_spaces(prop.substr(colon + 1));
        
        if (name == "background" || name == "background-color") {
            parse_background(val, style);
        } else if (name == "color") {
            style.has_color = true;
            style.color = parse_color(val);
        } else if (name == "border-radius") {
            try {
                style.border_radius = std::stof(val);
            } catch(...) {}
        } else if (name == "text-align") {
            style.text_align = val;
        } else if (name == "padding") {
            try {
                float p = std::stof(val);
                style.padding_left = style.padding_right = style.padding_top = style.padding_bottom = p;
            } catch(...) {}
        } else if (name == "padding-left") {
            try { style.padding_left = std::stof(val); } catch(...) {}
        } else if (name == "padding-right") {
            try { style.padding_right = std::stof(val); } catch(...) {}
        } else if (name == "padding-top") {
            try { style.padding_top = std::stof(val); } catch(...) {}
        } else if (name == "padding-bottom") {
            try { style.padding_bottom = std::stof(val); } catch(...) {}
        } else if (name == "margin") {
            try {
                float m = std::stof(val);
                style.margin_left = style.margin_right = style.margin_top = style.margin_bottom = m;
            } catch(...) {}
        } else if (name == "margin-left") {
            try { style.margin_left = std::stof(val); } catch(...) {}
        } else if (name == "margin-right") {
            try { style.margin_right = std::stof(val); } catch(...) {}
        } else if (name == "margin-top") {
            try { style.margin_top = std::stof(val); } catch(...) {}
        } else if (name == "margin-bottom") {
            try { style.margin_bottom = std::stof(val); } catch(...) {}
        } else if (name == "width") {
            try { style.width = std::stof(val); } catch(...) {}
        } else if (name == "height") {
            try { style.height = std::stof(val); } catch(...) {}
        } else if (name == "border-width") {
            try { style.border_width = std::stof(val); } catch(...) {}
        } else if (name == "border-color") {
            style.border_color = parse_color(val);
            style.has_border_color = true;
        } else if (name == "font-size") {
            try {
                style.font_size = std::stof(val);
            } catch(...) {}
        } else if (name == "display") {
            style.display = val;
        }
    }
}

void parse_css(const std::string& css_content, std::unordered_map<std::string, CssStyle>& styles) {
    size_t i = 0;
    size_t len = css_content.size();
    while (i < len) {
        size_t open_brace = css_content.find('{', i);
        if (open_brace == std::string::npos) break;
        
        std::string selector = trim_spaces(css_content.substr(i, open_brace - i));
        size_t close_brace = css_content.find('}', open_brace);
        if (close_brace == std::string::npos) break;
        
        std::string properties = css_content.substr(open_brace + 1, close_brace - open_brace - 1);
        i = close_brace + 1;
        
        CssStyle style;
        parse_css_properties(properties, style);
        styles[selector] = style;
    }
}

DomNode parse_html_to_dom(const std::string& html, std::string& css_content) {
    DomNode root;
    root.tag = "root";

    std::vector<DomNode*> node_stack;
    node_stack.push_back(&root);

    size_t i = 0;
    size_t len = html.size();

    while (i < len) {
        if (html[i] == '<') {
            size_t tag_end = html.find('>', i);
            if (tag_end == std::string::npos) {
                node_stack.back()->text_content += html[i];
                i++;
                continue;
            }

            std::string tag_inner = html.substr(i + 1, tag_end - i - 1);
            i = tag_end + 1;

            if (tag_inner.empty()) continue;

            if (tag_inner[0] == '/') {
                std::string tag_name = trim_spaces(tag_inner.substr(1));
                std::transform(tag_name.begin(), tag_name.end(), tag_name.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                
                if (tag_name == "style" && node_stack.size() > 1 && node_stack.back()->tag == "style") {
                    css_content = node_stack.back()->text_content;
                }

                if (node_stack.size() > 1 && node_stack.back()->tag == tag_name) {
                    node_stack.pop_back();
                }
                continue;
            }

            bool self_closing = false;
            if (tag_inner.back() == '/') {
                self_closing = true;
                tag_inner = tag_inner.substr(0, tag_inner.size() - 1);
            }

            size_t tag_name_end = 0;
            while (tag_name_end < tag_inner.size() && !std::isspace(tag_inner[tag_name_end])) {
                tag_name_end++;
            }
            std::string tag_name = tag_inner.substr(0, tag_name_end);
            std::transform(tag_name.begin(), tag_name.end(), tag_name.begin(),
                           [](unsigned char c) { return std::tolower(c); });

            DomNode child;
            child.tag = tag_name;

            size_t attr_pos = tag_name_end;
            while (attr_pos < tag_inner.size()) {
                while (attr_pos < tag_inner.size() && std::isspace(tag_inner[attr_pos])) {
                    attr_pos++;
                }
                if (attr_pos >= tag_inner.size()) break;
                
                size_t name_start = attr_pos;
                while (attr_pos < tag_inner.size() && tag_inner[attr_pos] != '=' && !std::isspace(tag_inner[attr_pos])) {
                    attr_pos++;
                }
                std::string attr_name = tag_inner.substr(name_start, attr_pos - name_start);
                
                while (attr_pos < tag_inner.size() && std::isspace(tag_inner[attr_pos])) {
                    attr_pos++;
                }
                
                std::string attr_val = "";
                if (attr_pos < tag_inner.size() && tag_inner[attr_pos] == '=') {
                    attr_pos++;
                    while (attr_pos < tag_inner.size() && std::isspace(tag_inner[attr_pos])) {
                        attr_pos++;
                    }
                    if (attr_pos < tag_inner.size()) {
                        if (tag_inner[attr_pos] == '"' || tag_inner[attr_pos] == '\'') {
                            char quote = tag_inner[attr_pos];
                            attr_pos++;
                            size_t val_start = attr_pos;
                            while (attr_pos < tag_inner.size() && tag_inner[attr_pos] != quote) {
                                attr_pos++;
                            }
                            attr_val = tag_inner.substr(val_start, attr_pos - val_start);
                            if (attr_pos < tag_inner.size()) attr_pos++;
                        } else {
                            size_t val_start = attr_pos;
                            while (attr_pos < tag_inner.size() && !std::isspace(tag_inner[attr_pos])) {
                                attr_pos++;
                            }
                            attr_val = tag_inner.substr(val_start, attr_pos - val_start);
                        }
                    }
                }
                
                if (attr_name == "class") {
                    child.class_name = attr_val;
                } else if (attr_name == "onclick") {
                    child.onclick = attr_val;
                } else if (attr_name == "href") {
                    child.href = attr_val;
                } else if (attr_name == "id") {
                    child.id = attr_val;
                } else if (attr_name == "style") {
                    child.inline_style = attr_val;
                    child.has_inline_style = true;
                    parse_css_properties(attr_val, child.parsed_inline_style);
                } else if (attr_name == "type") {
                    child.type = attr_val;
                } else if (attr_name == "value") {
                    child.value = attr_val;
                } else if (attr_name == "placeholder") {
                    child.placeholder = attr_val;
                }
            }

            node_stack.back()->children.push_back(std::move(child));
            
            if (!self_closing && tag_name != "hr" && tag_name != "img" && tag_name != "br" && tag_name != "meta" && tag_name != "link" && tag_name != "input") {
                node_stack.push_back(&(node_stack.back()->children.back()));
            }
        } else {
            node_stack.back()->text_content += html[i];
            i++;
        }
    }

    return root;
}

std::string extract_alert_message(const std::string& html) {
    std::string script_content = html;
    size_t script_start = html.find("<script>");
    if (script_start != std::string::npos) {
        size_t script_end = html.find("</script>", script_start);
        if (script_end != std::string::npos) {
            script_content = html.substr(script_start + 8, script_end - script_start - 8);
        }
    }
    
    size_t alert_pos = script_content.find("alert(");
    if (alert_pos != std::string::npos) {
        size_t val_start = script_content.find_first_of("\"'", alert_pos);
        if (val_start != std::string::npos) {
            size_t val_end = script_content.find_first_of("\"'", val_start + 1);
            if (val_end != std::string::npos) {
                return script_content.substr(val_start + 1, val_end - val_start - 1);
            }
        }
    }
    return "Action performed.";
}

void apply_style(CssStyle& dest, const CssStyle& src) {
    if (src.has_color) {
        dest.color = src.color;
        dest.has_color = true;
    }
    if (src.has_bg) {
        dest.bg_color = src.bg_color;
        dest.has_bg = true;
        dest.has_gradient = false;
    }
    if (src.has_gradient) {
        dest.gradient_start = src.gradient_start;
        dest.gradient_end = src.gradient_end;
        dest.has_gradient = true;
        dest.has_bg = false;
    }
    if (src.border_radius >= 0.0f) {
        dest.border_radius = src.border_radius;
    }
    if (src.text_align != "left") {
        dest.text_align = src.text_align;
    }
    if (src.padding_left > 0.0f) dest.padding_left = src.padding_left;
    if (src.padding_right > 0.0f) dest.padding_right = src.padding_right;
    if (src.padding_top > 0.0f) dest.padding_top = src.padding_top;
    if (src.padding_bottom > 0.0f) dest.padding_bottom = src.padding_bottom;
    
    if (src.margin_left > 0.0f) dest.margin_left = src.margin_left;
    if (src.margin_right > 0.0f) dest.margin_right = src.margin_right;
    if (src.margin_top > 0.0f) dest.margin_top = src.margin_top;
    if (src.margin_bottom > 0.0f) dest.margin_bottom = src.margin_bottom;
    
    if (src.width > -1.0f) dest.width = src.width;
    if (src.height > -1.0f) dest.height = src.height;
    if (src.border_width > 0.0f) dest.border_width = src.border_width;
    if (src.has_border_color) {
        dest.border_color = src.border_color;
        dest.has_border_color = true;
    }
    if (src.font_size != 1.0f) dest.font_size = src.font_size;
    if (!src.display.empty()) dest.display = src.display;
}
