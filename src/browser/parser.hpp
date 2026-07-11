#pragma once
#include "types.hpp"
#include <string>
#include <string_view>
#include <unordered_map>

std::string trim_spaces(std::string_view str);
std::string collapse_whitespace(std::string_view str);
ImVec4 parse_color(std::string_view str);
void parse_background(std::string_view value, CssStyle& style);
void parse_css_properties(const std::string& properties, CssStyle& style);
void parse_css(const std::string& css_content, std::unordered_map<std::string, CssStyle>& styles);
DomNode parse_html_to_dom(const std::string& html, std::string& css_content);
std::string extract_alert_message(const std::string& html);
void apply_style(CssStyle& dest, const CssStyle& src);
