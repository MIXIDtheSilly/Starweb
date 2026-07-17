#include "fetcher.hpp"
#include "globals.hpp"
#include "parser.hpp"
#include "../common/url_parser.hpp"
#include "../common/stwp_msg.hpp"
#include "../common/net.hpp"
#include "../common/conn.hpp"
#include "../common/tls.hpp"
#include <thread>
#include <memory>
#include <mutex>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <iostream>

namespace {

// One SSL_CTX for the whole browser; it's refcounted and thread-safe, and each
// fetch makes its own SSL from it.
std::once_flag tls_ctx_once;
std::unique_ptr<TlsContext> g_client_tls;
std::string g_client_tls_err;

TlsContext* client_tls_ctx(std::string& err) {
    std::call_once(tls_ctx_once, []() {
        const char* env = std::getenv("STARWEB_CA");
        std::string ca = env ? env : "certs/starweb_root.pem";
        g_client_tls = TlsContext::make_client(ca, g_client_tls_err);
    });
    if (!g_client_tls) err = g_client_tls_err;
    return g_client_tls.get();
}

int default_port_for(const std::string& scheme) {
    return scheme == "star" ? 8490 : 8090;
}

std::string port_suffix(const std::string& scheme, int port) {
    return port == default_port_for(scheme) ? "" : ":" + std::to_string(port);
}

// Plaintext content inside a page shown as secure.
bool is_mixed_content(bool page_secure, const std::string& sub_url) {
    if (!page_secure) return false;
    auto p = parse_url(sub_url);
    return p && p->scheme == "moon";
}

} // namespace

std::string resolve_url(const std::string& base_url, const std::string& relative_url) {
    if (relative_url.empty()) return base_url;
    if (relative_url.find("://") != std::string::npos) {
        auto opt_parsed = parse_url(relative_url);
        if (opt_parsed) {
            std::string scheme = opt_parsed->scheme;
            std::string host = opt_parsed->host;
            int port = opt_parsed->port;
            std::string path = opt_parsed->path;
            return scheme + "://" + format_host(host) + port_suffix(scheme, port) + path;
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
    
    return scheme + "://" + format_host(host) + port_suffix(scheme, port) + path;
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
    if (parsed.scheme != "moon" && parsed.scheme != "star") {
        result.error_message = "Unsupported scheme: " + parsed.scheme + "://";
        return result;
    }
    const bool use_tls = (parsed.scheme == "star");

    struct addrinfo hints{}, *res_info;
    hints.ai_family = AF_UNSPEC; // IPv4 or IPv6; the loop below tries each result
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

    std::unique_ptr<Conn> conn;
    if (use_tls) {
        std::string err;
        TlsContext* ctx = client_tls_ctx(err);
        if (ctx) {
            auto tconn = TlsConn::connect(*ctx, socket_fd, parsed.host,
                                          parsed.host + ":" + port_str, err);
            if (tconn) {
                result.is_secure = true;
                result.tls = tconn->info();
                conn = std::move(tconn);
            }
        }
        if (!conn) {
            result.error_message = err;
            result.tls_error = true;
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
    } else {
        conn = std::make_unique<PlainConn>(socket_fd);
    }

    StwpRequest req;
    req.method = "GET";
    req.path = parsed.path;
    req.headers["Host"] = format_host(parsed.host) +
        (parsed.port == default_port_for(parsed.scheme) ? "" : ":" + port_str);
    req.headers["User-Agent"] = "Starmap/1.0";
    req.headers["Connection"] = "close";

    std::string serialized_req = req.serialize();
    if (!write_all(*conn, serialized_req.data(), serialized_req.size())) {
        result.error_message = "Failed to send request.";
        {
            std::lock_guard<std::mutex> lock(fetch_mutex);
            Tab* tab = find_tab_by_id(tab_id);
            if (tab && is_main_resource && tab->active_socket_fd == socket_fd) {
                tab->active_socket_fd = net::kInvalidSocket;
            }
        }
        return result;
    }

    std::string raw_response;
    char recv_buf[4096];
    while (true) {
        net::ssize_t_ bytes_received = conn->read(recv_buf, sizeof(recv_buf));
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
    conn.reset();

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
        final_url = scheme + "://" + format_host(host) + port_suffix(scheme, port) + path;
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
                const bool page_secure = res.is_secure;
                std::string css_content = "";
                res.dom = parse_html_to_dom(res.body, css_content, res.scripts);

                std::vector<std::string> stylesheet_hrefs;
                find_stylesheets_in_dom(res.dom, stylesheet_hrefs);

                for (const auto& href : stylesheet_hrefs) {
                    std::string sheet_url = resolve_url(final_url, href);
                    if (is_mixed_content(page_secure, sheet_url)) {
                        std::cerr << "[mixed-content] blocked stylesheet " << sheet_url << "\n";
                        continue;
                    }
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
                    if (is_mixed_content(page_secure, img_url)) {
                        std::cerr << "[mixed-content] blocked image " << img_url << "\n";
                        continue;
                    }
                    FetchResult img_res = perform_fetch(tab_id, img_url, false);
                    if (img_res.success) {
                        res.fetched_images[img_url] = img_res.body;
                    }
                }

                std::vector<std::string> media_srcs;
                find_media_in_dom(res.dom, media_srcs);
                for (const auto& src : media_srcs) {
                    std::string media_url = resolve_url(final_url, src);
                    if (is_mixed_content(page_secure, media_url)) {
                        std::cerr << "[mixed-content] blocked media " << media_url << "\n";
                        continue;
                    }
                    FetchResult media_res = perform_fetch(tab_id, media_url, false);
                    if (media_res.success) {
                        res.fetched_media[media_url] = media_res.body;
                    }
                }

                // Fetched in place so the engine still sees one list in document
                // order. perform_fetch's scheme gate is what stops a page pulling
                // code off an arbitrary host.
                for (PageScript& script : res.scripts) {
                    if (script.src.empty()) continue;
                    script.src = resolve_url(final_url, script.src);
                    if (is_mixed_content(page_secure, script.src)) {
                        std::cerr << "[mixed-content] blocked script " << script.src << "\n";
                        continue;
                    }
                    FetchResult script_res = perform_fetch(tab_id, script.src, false);
                    if (script_res.success && script_res.status_code == 200) {
                        script.source = std::move(script_res.body);
                    } else {
                        std::cerr << "[script] failed to load " << script.src << ": "
                                  << (script_res.success
                                          ? std::to_string(script_res.status_code) + " " + script_res.status_text
                                          : script_res.error_message)
                                  << "\n";
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
