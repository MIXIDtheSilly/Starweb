#include "fetcher.hpp"
#include "globals.hpp"
#include "parser.hpp"
#include "../common/url_parser.hpp"
#include "../common/stwp_msg.hpp"
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <thread>
#include <cstring>

std::string resolve_url(const std::string& base_url, const std::string& relative_url) {
    if (relative_url.empty()) return base_url;
    if (relative_url.find("://") != std::string::npos) {
        return relative_url;
    }
    
    auto base_opt = parse_url(base_url);
    if (!base_opt) return relative_url;
    
    std::string scheme = base_opt->scheme;
    std::string host = base_opt->host;
    int port = base_opt->port;
    
    std::string path;
    if (relative_url[0] == '/') {
        path = relative_url;
    } else {
        std::string base_path = base_opt->path;
        auto last_slash = base_path.find_last_of('/');
        if (last_slash != std::string::npos) {
            path = base_path.substr(0, last_slash + 1) + relative_url;
        } else {
            path = "/" + relative_url;
        }
    }
    
    return scheme + "://" + host + ":" + std::to_string(port) + path;
}

std::string find_title_in_dom(const DomNode& node) {
    if (node.tag == "title") {
        return node.text_content;
    }
    for (const auto& child : node.children) {
        std::string t = find_title_in_dom(child);
        if (!t.empty()) return t;
    }
    return "";
}

void find_stylesheets_in_dom(const DomNode& node, std::vector<std::string>& hrefs) {
    if (node.tag == "link") {
        if (!node.href.empty()) {
            hrefs.push_back(node.href);
        }
    }
    for (const auto& child : node.children) {
        find_stylesheets_in_dom(child, hrefs);
    }
}

FetchResult perform_fetch(int tab_id, const std::string& url_str, bool is_main_resource) {
    FetchResult result;
    auto opt_parsed = parse_url(url_str);
    if (!opt_parsed) {
        result.error_message = "Invalid URL format.";
        return result;
    }

    auto parsed = *opt_parsed;
    if (parsed.scheme != "moon") {
        result.error_message = "Only 'moon://' scheme is supported.";
        return result;
    }

    struct addrinfo hints{}, *res_info;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    std::string port_str = std::to_string(parsed.port);
    int status = getaddrinfo(parsed.host.c_str(), port_str.c_str(), &hints, &res_info);
    if (status != 0) {
        result.error_message = "Host resolution failed: " + std::string(gai_strerror(status));
        return result;
    }

    int socket_fd = -1;
    struct addrinfo* rp;
    for (rp = res_info; rp != nullptr; rp = rp->ai_next) {
        socket_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (socket_fd == -1) continue;

        {
            std::lock_guard<std::mutex> lock(fetch_mutex);
            Tab* tab = find_tab_by_id(tab_id);
            if (!tab) {
                close(socket_fd);
                freeaddrinfo(res_info);
                result.error_message = "Tab closed";
                return result;
            }
            if (is_main_resource && url_str != tab->current_url) {
                close(socket_fd);
                freeaddrinfo(res_info);
                result.error_message = "Cancelled";
                return result;
            }
            if (is_main_resource) {
                tab->active_socket_fd = socket_fd;
            }
        }

        struct timeval tv;
        tv.tv_sec = 4;
        tv.tv_usec = 0;
        setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

        if (connect(socket_fd, rp->ai_addr, rp->ai_addrlen) != -1) {
            break;
        }

        {
            std::lock_guard<std::mutex> lock(fetch_mutex);
            Tab* tab = find_tab_by_id(tab_id);
            if (tab && is_main_resource && tab->active_socket_fd == socket_fd) {
                tab->active_socket_fd = -1;
            }
        }
        close(socket_fd);
    }

    freeaddrinfo(res_info);

    if (rp == nullptr) {
        result.error_message = "Connection failed to " + parsed.host + ":" + port_str;
        return result;
    }

    StwpRequest req;
    req.method = "GET";
    req.path = parsed.path;
    req.headers["Host"] = parsed.host + (parsed.port == 8090 ? "" : ":" + port_str);
    req.headers["User-Agent"] = "Starmap/1.0";
    req.headers["Connection"] = "close";

    std::string serialized_req = req.serialize();
    if (send(socket_fd, serialized_req.data(), serialized_req.size(), 0) < 0) {
        result.error_message = "Failed to send request.";
        {
            std::lock_guard<std::mutex> lock(fetch_mutex);
            Tab* tab = find_tab_by_id(tab_id);
            if (tab && is_main_resource && tab->active_socket_fd == socket_fd) {
                tab->active_socket_fd = -1;
            }
        }
        close(socket_fd);
        return result;
    }

    std::string raw_response;
    char recv_buf[4096];
    while (true) {
        ssize_t bytes_received = recv(socket_fd, recv_buf, sizeof(recv_buf), 0);
        if (bytes_received < 0) {
            result.error_message = "Socket read failure.";
            break;
        }
        if (bytes_received == 0) {
            break;
        }
        raw_response.append(recv_buf, bytes_received);
    }

    {
        std::lock_guard<std::mutex> lock(fetch_mutex);
        Tab* tab = find_tab_by_id(tab_id);
        if (tab && is_main_resource && tab->active_socket_fd == socket_fd) {
            tab->active_socket_fd = -1;
        }
    }
    close(socket_fd);

    if (result.error_message == "Socket read failure.") {
        return result;
    }

    StwpResponse res_msg;
    size_t bytes_consumed = 0;
    if (!parse_response(raw_response, bytes_consumed, res_msg)) {
        result.error_message = "Failed to parse STWP response.";
        return result;
    }

    result.success = true;
    result.status_code = res_msg.status_code;
    result.status_text = res_msg.status_text;
    result.headers = res_msg.headers;
    result.body = res_msg.body;
    return result;
}

void start_async_fetch(int tab_id, const std::string& url_str, bool is_history_nav) {
    std::string final_url = url_str;
    if (final_url.find("://") == std::string::npos) {
        final_url = "moon://" + final_url;
    }

    std::lock_guard<std::mutex> lock(fetch_mutex);
    Tab* tab = find_tab_by_id(tab_id);
    if (!tab) return;

    if (!is_history_nav) {
        if (tab->history_index >= 0 && tab->history_index < (int)tab->navigation_history.size() - 1) {
            tab->navigation_history.erase(tab->navigation_history.begin() + tab->history_index + 1, tab->navigation_history.end());
        }
        tab->navigation_history.push_back(final_url);
        tab->history_index = (int)tab->navigation_history.size() - 1;
    }

    if (tab->active_socket_fd != -1) {
        close(tab->active_socket_fd);
        tab->active_socket_fd = -1;
    }
    tab->new_page_ready = false;

    tab->is_fetching = true;
    tab->status_text = "Fetching " + final_url + "...";
    tab->current_url = final_url;

    std::strncpy(tab->url_input, final_url.c_str(), sizeof(tab->url_input) - 1);
    tab->url_input[sizeof(tab->url_input) - 1] = '\0';

    std::thread([tab_id, final_url]() {
        FetchResult res = perform_fetch(tab_id, final_url, true);
        
        if (res.success) {
            std::string css_content = "";
            res.dom = parse_html_to_dom(res.body, css_content);
            
            std::vector<std::string> stylesheet_hrefs;
            find_stylesheets_in_dom(res.dom, stylesheet_hrefs);
            
            for (const auto& href : stylesheet_hrefs) {
                std::string sheet_url = resolve_url(final_url, href);
                FetchResult sheet_res = perform_fetch(tab_id, sheet_url, false);
                if (sheet_res.success) {
                    css_content += "\n" + sheet_res.body;
                }
            }
            
            parse_css(css_content, res.css_classes);
        }
        
        std::lock_guard<std::mutex> lock(fetch_mutex);
        Tab* t = find_tab_by_id(tab_id);
        if (t) {
            if (final_url == t->current_url) {
                t->active_page = std::move(res);
                t->new_page_ready = true;
            }
        }
    }).detach();
}
