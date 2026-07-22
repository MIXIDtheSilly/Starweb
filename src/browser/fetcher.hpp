#pragma once
#include "types.hpp"
#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// Receives body bytes as they arrive; returning false aborts the transfer.
using BodySink = std::function<bool(const char*, std::size_t)>;

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
    // A sink streams the body past the caller instead of buffering it. It bypasses
    // max_response_bytes — the point is bodies too big to hold — and leaves
    // FetchResult::body empty. Returning false aborts the transfer.
    BodySink on_body_chunk;
    // Consulted once the response headers are in, for callers that only know
    // whether to stream after seeing the content type. Return an empty sink to
    // buffer as usual. Ignored when on_body_chunk is already set.
    std::function<BodySink(int status, const std::unordered_map<std::string, std::string>&)> on_headers;
};

FetchResult perform_request(const std::string& url_str, const RequestOptions& opt);

std::string resolve_url(const std::string& base_url, const std::string& relative_url);
std::string find_title_in_dom(const DomNode& node);
void find_stylesheets_in_dom(const DomNode& node, std::vector<std::string>& hrefs);
void find_images_in_dom(const DomNode& node, std::vector<std::string>& srcs);
FetchResult perform_fetch(int tab_id, const std::string& url_str, bool is_main_resource = true,
                          RequestOptions opt = {});
void start_async_fetch(int tab_id, const std::string& url_str, bool is_history_nav = false);
