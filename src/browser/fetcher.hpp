#pragma once
#include "types.hpp"
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

// Transport-level knobs for perform_request. The socket hooks let a caller track
// the live fd (the tab keeps one so navigation can cancel it) without perform_request
// itself knowing about tabs; on_socket returning false aborts the attempt.
struct RequestOptions {
    std::string method = "GET";
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    int timeout_secs = 4;
    std::size_t max_response_bytes = 128u * 1024u * 1024u;
    std::function<bool(net::socket_t)> on_socket;
    std::function<void(net::socket_t)> on_socket_done;
};

FetchResult perform_request(const std::string& url_str, const RequestOptions& opt);

std::string resolve_url(const std::string& base_url, const std::string& relative_url);
std::string find_title_in_dom(const DomNode& node);
void find_stylesheets_in_dom(const DomNode& node, std::vector<std::string>& hrefs);
void find_images_in_dom(const DomNode& node, std::vector<std::string>& srcs);
FetchResult perform_fetch(int tab_id, const std::string& url_str, bool is_main_resource = true);
void start_async_fetch(int tab_id, const std::string& url_str, bool is_history_nav = false);
