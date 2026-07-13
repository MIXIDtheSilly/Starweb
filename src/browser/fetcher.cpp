#include "fetcher.hpp"
#include "globals.hpp"
#include "parser.hpp"
#include "../common/url_parser.hpp"
#include "../common/stwp_msg.hpp"
#include "../common/net.hpp"
#include <thread>
#include <cstring>
#include <algorithm>
#include <cctype>

std::string resolve_url(const std::string& base_url, const std::string& relative_url) {
    if (relative_url.empty()) return base_url;
    if (relative_url.find("://") != std::string::npos) {
        auto opt_parsed = parse_url(relative_url);
        if (opt_parsed) {
            std::string scheme = opt_parsed->scheme;
            std::string host = opt_parsed->host;
            int port = opt_parsed->port;
            std::string path = opt_parsed->path;
            
            std::string port_part = "";
            if (scheme == "moon" && port != 8090) {
                port_part = ":" + std::to_string(port);
            } else if (scheme == "star" && port != 8490) {
                port_part = ":" + std::to_string(port);
            } else if (scheme != "moon" && scheme != "star") {
                port_part = ":" + std::to_string(port);
            }
            return scheme + "://" + host + port_part + path;
        }
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
    
    std::string port_part = "";
    if (scheme == "moon" && port != 8090) {
        port_part = ":" + std::to_string(port);
    } else if (scheme == "star" && port != 8490) {
        port_part = ":" + std::to_string(port);
    } else if (scheme != "moon" && scheme != "star") {
        port_part = ":" + std::to_string(port);
    }
    
    return scheme + "://" + host + port_part + path;
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

void find_images_in_dom(const DomNode& node, std::vector<std::string>& srcs) {
    if (node.tag == "img") {
        if (!node.src.empty()) {
            srcs.push_back(node.src);
        }
    }
    for (const auto& child : node.children) {
        find_images_in_dom(child, srcs);
    }
}

void find_media_in_dom(const DomNode& node, std::vector<std::string>& srcs) {
    if (node.tag == "video" || node.tag == "audio") {
        if (!node.src.empty()) {
            srcs.push_back(node.src);
        }
    }
    if (node.tag == "source") {
        if (!node.src.empty()) {
            srcs.push_back(node.src);
        }
    }
    for (const auto& child : node.children) {
        find_media_in_dom(child, srcs);
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
    hints.ai_family = AF_INET; // server only binds an AF_INET listening socket
    hints.ai_socktype = SOCK_STREAM;

    std::string port_str = std::to_string(parsed.port);
    int status = getaddrinfo(parsed.host.c_str(), port_str.c_str(), &hints, &res_info);
    if (status != 0) {
        result.error_message = "Host resolution failed: " + std::string(gai_strerror(status));
        return result;
    }

    net::socket_t socket_fd = net::kInvalidSocket;
    struct addrinfo* rp;
    for (rp = res_info; rp != nullptr; rp = rp->ai_next) {
        socket_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (!net::is_valid(socket_fd)) continue;

        {
            std::lock_guard<std::mutex> lock(fetch_mutex);
            Tab* tab = find_tab_by_id(tab_id);
            if (!tab) {
                net::close(socket_fd);
                freeaddrinfo(res_info);
                result.error_message = "Tab closed";
                return result;
            }
            if (is_main_resource && url_str != tab->current_url) {
                net::close(socket_fd);
                freeaddrinfo(res_info);
                result.error_message = "Cancelled";
                return result;
            }
            if (is_main_resource) {
                tab->active_socket_fd = socket_fd;
            }
        }

        net::set_recv_timeout(socket_fd, 4);
        net::set_send_timeout(socket_fd, 4);

        if (connect(socket_fd, rp->ai_addr, rp->ai_addrlen) != -1) {
            break;
        }

        {
            std::lock_guard<std::mutex> lock(fetch_mutex);
            Tab* tab = find_tab_by_id(tab_id);
            if (tab && is_main_resource && tab->active_socket_fd == socket_fd) {
                tab->active_socket_fd = net::kInvalidSocket;
            }
        }
        net::close(socket_fd);
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
                tab->active_socket_fd = net::kInvalidSocket;
            }
        }
        net::close(socket_fd);
        return result;
    }

    std::string raw_response;
    char recv_buf[4096];
    while (true) {
        net::ssize_t_ bytes_received = recv(socket_fd, recv_buf, sizeof(recv_buf), 0);
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
            tab->active_socket_fd = net::kInvalidSocket;
        }
    }
    net::close(socket_fd);

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

    auto opt_url = parse_url(final_url);
    if (opt_url) {
        std::string scheme = opt_url->scheme;
        std::string host = opt_url->host;
        int port = opt_url->port;
        std::string path = opt_url->path;
        
        std::string port_part = "";
        if (scheme == "moon" && port != 8090) {
            port_part = ":" + std::to_string(port);
        } else if (scheme == "star" && port != 8490) {
            port_part = ":" + std::to_string(port);
        } else if (scheme != "moon" && scheme != "star") {
            port_part = ":" + std::to_string(port);
        }
        final_url = scheme + "://" + host + port_part + path;
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

    if (net::is_valid(tab->active_socket_fd)) {
        net::close(tab->active_socket_fd);
        tab->active_socket_fd = net::kInvalidSocket;
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
            std::string content_type = "";
            auto it = res.headers.find("content-type");
            if (it != res.headers.end()) {
                content_type = it->second;
                std::transform(content_type.begin(), content_type.end(), content_type.begin(), [](unsigned char c) { return std::tolower(c); });
            }

            bool is_html = true;
            bool is_image = false;
            bool is_video = false;
            bool is_audio = false;

            if (!content_type.empty() && content_type != "application/octet-stream") {
                if (content_type.find("text/html") == std::string::npos) {
                    is_html = false;
                }
                if (content_type.rfind("image/", 0) == 0) {
                    is_image = true;
                } else if (content_type.rfind("video/", 0) == 0) {
                    is_video = true;
                } else if (content_type.rfind("audio/", 0) == 0) {
                    is_audio = true;
                }
            } else {
                auto opt_parsed = parse_url(final_url);
                if (opt_parsed) {
                    std::string path = opt_parsed->path;
                    auto dot = path.find_last_of('.');
                    if (dot != std::string::npos) {
                        std::string ext = path.substr(dot);
                        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
                        if (ext != ".html" && ext != ".htm" && !ext.empty()) {
                            is_html = false;
                        }
                        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif") {
                            is_image = true;
                        } else if (ext == ".mp4" || ext == ".mov" || ext == ".m4v") {
                            is_video = true;
                        } else if (ext == ".mp3" || ext == ".wav" || ext == ".aac" || ext == ".m4a") {
                            is_audio = true;
                        }
                    }
                }
            }

            if (is_image) {
                res.dom = DomNode();
                res.dom.tag = "root";
                DomNode img_node;
                img_node.tag = "img";
                img_node.src = final_url;
                res.dom.children.push_back(img_node);
                
                res.fetched_images[final_url] = res.body;
            } else if (is_video) {
                res.dom = DomNode();
                res.dom.tag = "root";
                DomNode video_node;
                video_node.tag = "video";
                video_node.src = final_url;
                video_node.controls = true;
                video_node.autoplay = true;
                video_node.inline_style = "width: 700; height: 500;";
                video_node.has_inline_style = true;
                parse_css_properties(video_node.inline_style, video_node.parsed_inline_style);
                
                res.dom.children.push_back(video_node);
                res.fetched_media[final_url] = res.body;
            } else if (is_audio) {
                res.dom = DomNode();
                res.dom.tag = "root";
                DomNode audio_node;
                audio_node.tag = "audio";
                audio_node.src = final_url;
                audio_node.controls = true;
                audio_node.autoplay = true;
                audio_node.inline_style = "width: 450;";
                audio_node.has_inline_style = true;
                parse_css_properties(audio_node.inline_style, audio_node.parsed_inline_style);
                
                res.dom.children.push_back(audio_node);
                res.fetched_media[final_url] = res.body;
            } else if (is_html) {
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
                
                std::vector<std::string> img_srcs;
                find_images_in_dom(res.dom, img_srcs);
                for (const auto& src : img_srcs) {
                    std::string img_url = resolve_url(final_url, src);
                    FetchResult img_res = perform_fetch(tab_id, img_url, false);
                    if (img_res.success) {
                        res.fetched_images[img_url] = img_res.body;
                    }
                }

                std::vector<std::string> media_srcs;
                find_media_in_dom(res.dom, media_srcs);
                for (const auto& src : media_srcs) {
                    std::string media_url = resolve_url(final_url, src);
                    FetchResult media_res = perform_fetch(tab_id, media_url, false);
                    if (media_res.success) {
                        res.fetched_media[media_url] = media_res.body;
                    }
                }
            } else {
                res.dom = DomNode();
                res.dom.tag = "root";
                DomNode pre_node;
                pre_node.tag = "pre";
                pre_node.text_content = res.body;
                res.dom.children.push_back(pre_node);
            }
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
