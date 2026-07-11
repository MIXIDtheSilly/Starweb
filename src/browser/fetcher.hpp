#pragma once
#include "types.hpp"
#include <string>
#include <vector>

std::string resolve_url(const std::string& base_url, const std::string& relative_url);
std::string find_title_in_dom(const DomNode& node);
void find_stylesheets_in_dom(const DomNode& node, std::vector<std::string>& hrefs);
FetchResult perform_fetch(int tab_id, const std::string& url_str, bool is_main_resource = true);
void start_async_fetch(int tab_id, const std::string& url_str, bool is_history_nav = false);
