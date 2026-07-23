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

// Append a Unicode code point to a UTF-8 string.
static void append_utf8(std::string& out, unsigned int cp) {
    if (cp <= 0x7F) {
        out += (char)cp;
    } else if (cp <= 0x7FF) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

// Decode the common named/numeric HTML entities (&lt; &amp; &#39; &#x27; …).
std::string decode_entities(std::string_view str) {
    std::string out;
    out.reserve(str.size());
    for (size_t i = 0; i < str.size();) {
        if (str[i] != '&') { out += str[i++]; continue; }
        size_t semi = str.find(';', i + 1);
        if (semi == std::string_view::npos || semi - i > 12) { out += str[i++]; continue; }
        std::string_view ent = str.substr(i + 1, semi - i - 1);
        bool handled = true;
        if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "amp") out += '&';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (ent == "nbsp") out += ' ';
        else if (ent == "copy") append_utf8(out, 0x00A9);
        else if (ent == "reg") append_utf8(out, 0x00AE);
        else if (ent == "mdash") append_utf8(out, 0x2014);
        else if (ent == "ndash") append_utf8(out, 0x2013);
        else if (ent == "hellip") append_utf8(out, 0x2026);
        else if (!ent.empty() && ent[0] == '#') {
            try {
                unsigned int cp = (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X'))
                    ? (unsigned int)std::stoul(std::string(ent.substr(2)), nullptr, 16)
                    : (unsigned int)std::stoul(std::string(ent.substr(1)), nullptr, 10);
                append_utf8(out, cp);
            } catch (...) { handled = false; }
        } else {
            handled = false;
        }
        if (handled) i = semi + 1;
        else out += str[i++];
    }
    return out;
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
            try {
                size_t consumed = 0;
                float num = std::stof(val, &consumed);
                std::string unit = trim_spaces(std::string_view(val).substr(consumed));
                if (unit == "vw") style.width_vw = num;
                else              style.width = num;
            } catch(...) {}
        } else if (name == "height") {
            try {
                size_t consumed = 0;
                float num = std::stof(val, &consumed);
                std::string unit = trim_spaces(std::string_view(val).substr(consumed));
                if (unit == "vh") style.height_vh = num;
                else              style.height = num;
            } catch(...) {}
        } else if (name == "border-width") {
            try { style.border_width = std::stof(val); } catch(...) {}
        } else if (name == "border-color") {
            style.border_color = parse_color(val);
            style.has_border_color = true;
        } else if (name == "font-size") {
            try {
                size_t consumed = 0;
                float num = std::stof(val, &consumed);
                std::string unit = trim_spaces(std::string_view(val).substr(consumed));
                // font_size is a scale relative to the 16px base UI font.
                if (unit == "px")                 style.font_size = num / 16.0f;
                else if (unit == "pt")            style.font_size = (num * 96.0f / 72.0f) / 16.0f;
                else if (unit == "%")             style.font_size = num / 100.0f;
                else if (unit == "em" || unit == "rem") style.font_size = num;
                else                               style.font_size = num; // bare number: legacy multiplier
            } catch(...) {}
        } else if (name == "display") {
            style.display = val;
        } else if (name == "flex-direction") {
            style.flex_direction = val;
        } else if (name == "justify-content") {
            style.justify_content = val;
        } else if (name == "align-items") {
            style.align_items = val;
        } else if (name == "align-self") {
            style.align_self = val;
        } else if (name == "flex-wrap") {
            style.flex_wrap = val;
        } else if (name == "gap") {
            try { float g = std::stof(val); style.row_gap = style.column_gap = g; } catch(...) {}
        } else if (name == "row-gap") {
            try { style.row_gap = std::stof(val); } catch(...) {}
        } else if (name == "column-gap") {
            try { style.column_gap = std::stof(val); } catch(...) {}
        } else if (name == "flex-grow") {
            try { style.flex_grow = std::stof(val); } catch(...) {}
        } else if (name == "flex-shrink") {
            try { style.flex_shrink = std::stof(val); } catch(...) {}
        } else if (name == "flex-basis") {
            if (val == "auto") { style.flex_basis = -1.0f; }
            else { try { style.flex_basis = std::stof(val); } catch(...) {} }
        } else if (name == "flex") {
            // flex: <grow> [shrink] [basis]
            try {
                std::istringstream iss(val);
                std::string g, s, b;
                iss >> g >> s >> b;
                if (!g.empty()) style.flex_grow = std::stof(g);
                if (!s.empty()) { try { style.flex_shrink = std::stof(s); } catch(...) {} }
                if (!b.empty() && b != "auto") { try { style.flex_basis = std::stof(b); } catch(...) {} }
            } catch(...) {}
        }
    }
}

// Rules are split on braces, so a surviving /* comment */ would be swallowed into
// the next rule's selector and silently kill it. Strip them up front.
static std::string strip_css_comments(const std::string& in) {
    if (in.find("/*") == std::string::npos) return in;
    std::string out;
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        if (in[i] == '/' && i + 1 < in.size() && in[i + 1] == '*') {
            size_t end = in.find("*/", i + 2);
            if (end == std::string::npos) break;  // unterminated: drop the remainder
            i = end + 2;
        } else {
            out += in[i++];
        }
    }
    return out;
}

void parse_css(const std::string& raw_css, std::unordered_map<std::string, CssStyle>& styles) {
    const std::string css_content = strip_css_comments(raw_css);
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

static size_t find_ci(const std::string& h, const std::string& needle, size_t from) {
    size_t n = needle.size();
    if (n == 0 || h.size() < n) return std::string::npos;
    for (size_t p = from; p + n <= h.size(); ++p) {
        size_t k = 0;
        for (; k < n; ++k) {
            if (std::tolower((unsigned char)h[p + k]) != (unsigned char)needle[k]) break;
        }
        if (k == n) return p;
    }
    return std::string::npos;
}

// Splits the attributes out of a tag's inner text, starting at `attr_pos` (just past
// the tag name). Values may be single-quoted, double-quoted, or bare; a valueless
// attribute yields an empty value.
static std::vector<std::pair<std::string, std::string>>
parse_attributes(const std::string& tag_inner, size_t attr_pos) {
    std::vector<std::pair<std::string, std::string>> attrs;
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

        // Decoded here rather than by each consumer, so nothing decodes twice:
        // href="?a=1&amp;b=2" has to reach the network as "?a=1&b=2", and an
        // alt or placeholder carrying &lt; has to render as '<'.
        if (attr_val.find('&') != std::string::npos) {
            attr_val = decode_entities(attr_val);
        }
        attrs.emplace_back(std::move(attr_name), std::move(attr_val));
    }
    return attrs;
}

DomNode parse_html_to_dom(const std::string& html, std::string& css_content,
                          std::vector<PageScript>& scripts) {
    DomNode root;
    root.tag = "root";

    uint64_t next_id = 1;
    root.node_id = next_id++;

    std::vector<DomNode*> node_stack;
    node_stack.push_back(&root);

    // Pending run of character data between tags. Flushed as a "#text" child node
    // (interleaved with element children so document order is preserved) and also
    // concatenated onto the parent's text_content for leaf consumers.
    std::string text_run;
    auto flush_text = [&]() {
        if (text_run.empty()) return;
        std::string decoded = decode_entities(text_run);
        node_stack.back()->text_content += decoded;
        if (!trim_spaces(decoded).empty()) {
            DomNode text_node;
            text_node.node_id = next_id++;
            text_node.tag = "#text";
            text_node.text_content = decoded;
            node_stack.back()->children.push_back(std::move(text_node));
        }
        text_run.clear();
    };

    size_t i = 0;
    size_t len = html.size();

    while (i < len) {
        if (html[i] == '<') {
            size_t tag_end = html.find('>', i);
            if (tag_end == std::string::npos) {
                text_run += html[i];
                i++;
                continue;
            }

            flush_text();

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

            if (tag_name == "script") {
                std::string script_src;
                for (auto& [name, val] : parse_attributes(tag_inner, tag_name_end)) {
                    if (name == "src") script_src = val;
                }
                std::string raw;
                if (!self_closing) {
                    size_t close = find_ci(html, "</script>", i);
                    raw = (close == std::string::npos)
                        ? html.substr(i) : html.substr(i, close - i);
                    i = (close == std::string::npos) ? len : close + 9;
                }
                // A src wins over any inline body, which HTML says to ignore. The URL
                // is left unresolved here; the parser has no base URL to resolve against.
                if (!script_src.empty()) {
                    scripts.push_back(PageScript{ std::move(script_src), "" });
                } else if (!trim_spaces(raw).empty()) {
                    scripts.push_back(PageScript{ "", std::move(raw) });
                }
                continue;
            }

            DomNode child;
            child.node_id = next_id++;
            child.tag = tag_name;

            for (const auto& [attr_name, attr_val] : parse_attributes(tag_inner, tag_name_end)) {
                if (attr_name == "class") {
                    child.class_name = attr_val;
                } else if (attr_name == "onclick") {
                    child.onclick = attr_val;
                } else if (attr_name == "href") {
                    child.href = attr_val;
                } else if (attr_name == "src") {
                    child.src = attr_val;
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
                } else if (attr_name == "name") {
                    child.name = attr_val;
                } else if (attr_name == "min") {
                    child.min_val = attr_val;
                } else if (attr_name == "max") {
                    child.max_val = attr_val;
                } else if (attr_name == "step") {
                    child.step_val = attr_val;
                } else if (attr_name == "checked") {
                    child.checked = true;
                } else if (attr_name == "autoplay") {
                    child.autoplay = true;
                } else if (attr_name == "loop") {
                    child.loop = true;
                } else if (attr_name == "controls") {
                    child.controls = true;
                } else if (attr_name == "muted") {
                    child.muted = true;
                }
            }

            node_stack.back()->children.push_back(std::move(child));
            
            if (!self_closing && tag_name != "hr" && tag_name != "img" && tag_name != "br" && tag_name != "meta" && tag_name != "link" && tag_name != "input" && tag_name != "source") {
                node_stack.push_back(&(node_stack.back()->children.back()));
            }
        } else {
            text_run += html[i];
            i++;
        }
    }
    flush_text();

    return root;
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
    
    // A later rule's unit wins outright, so setting one form clears the other.
    if (src.width > -1.0f) { dest.width = src.width; dest.width_vw = -1.0f; }
    if (src.width_vw > -1.0f) { dest.width_vw = src.width_vw; dest.width = -1.0f; }
    if (src.height > -1.0f) { dest.height = src.height; dest.height_vh = -1.0f; }
    if (src.height_vh > -1.0f) { dest.height_vh = src.height_vh; dest.height = -1.0f; }
    if (src.border_width > 0.0f) dest.border_width = src.border_width;
    if (src.has_border_color) {
        dest.border_color = src.border_color;
        dest.has_border_color = true;
    }
    if (src.font_size != 1.0f) dest.font_size = src.font_size;
    if (!src.display.empty()) dest.display = src.display;

    if (!src.flex_direction.empty()) dest.flex_direction = src.flex_direction;
    if (!src.justify_content.empty()) dest.justify_content = src.justify_content;
    if (!src.align_items.empty()) dest.align_items = src.align_items;
    if (!src.align_self.empty()) dest.align_self = src.align_self;
    if (!src.flex_wrap.empty()) dest.flex_wrap = src.flex_wrap;
    if (src.row_gap >= 0.0f) dest.row_gap = src.row_gap;
    if (src.column_gap >= 0.0f) dest.column_gap = src.column_gap;
    if (src.flex_grow >= 0.0f) dest.flex_grow = src.flex_grow;
    if (src.flex_shrink >= 0.0f) dest.flex_shrink = src.flex_shrink;
    if (src.flex_basis >= 0.0f) dest.flex_basis = src.flex_basis;
}
