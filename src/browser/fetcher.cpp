#include "fetcher.hpp"
#include "globals.hpp"
#include "parser.hpp"
#include "../common/url_parser.hpp"
#include "../common/stwp_msg.hpp"
#include "../common/net.hpp"
#include "../common/resolver.hpp"
#include "../common/conn.hpp"
#include "../common/tls.hpp"
#include <thread>
#include <memory>
#include <mutex>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>

namespace {

// One SSL_CTX for the whole browser; it's refcounted and thread-safe, and each
// fetch makes its own SSL from it.
std::once_flag tls_ctx_once;
std::unique_ptr<TlsContext> g_client_tls;
std::string g_client_tls_err;

// The root CA has to be found wherever the browser was launched from: failing to
// load it kills every star:// fetch for the life of the process, not just one.
// Anchored on the executable, with the build-tree layout (binary at the repo
// root, certs/ beside it) and a bare CWD lookup as fallbacks.
std::string default_ca_path() {
    namespace fs = std::filesystem;
    const fs::path rel = fs::path("certs") / "starweb_root.pem";
    for (const fs::path& base : {app_dir(), app_dir().parent_path(), fs::path(".")}) {
        std::error_code ec;
        fs::path candidate = base / rel;
        if (fs::exists(candidate, ec) && !ec) return candidate.string();
    }
    return rel.string();  // nothing found; report the familiar path in the error
}

TlsContext* client_tls_ctx(std::string& err) {
    std::call_once(tls_ctx_once, []() {
        const char* env = std::getenv("STARWEB_CA");
        std::string ca = (env && *env) ? std::string(env) : default_ca_path();
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

FetchResult perform_request(const std::string& url_str, const RequestOptions& opt) {
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

    // .web names are answered by StarDNS; everything else by getaddrinfo.
    std::string port_str = std::to_string(parsed.port);
    std::string resolve_err;
    auto endpoints = stardns::resolve(parsed.host, (uint16_t)parsed.port, resolve_err);
    if (endpoints.empty()) {
        result.error_message = "Host resolution failed: " +
            (resolve_err.empty() ? std::string("no addresses for ") + parsed.host
                                 : resolve_err);
        return result;
    }

    net::socket_t socket_fd = net::kInvalidSocket;
    bool connected = false;
    for (const auto& ep : endpoints) {
        socket_fd = socket(ep.family, SOCK_STREAM, 0);
        if (!net::is_valid(socket_fd)) continue;

        if (opt.on_socket && !opt.on_socket(socket_fd)) {
            net::close(socket_fd);
            result.error_message = "Cancelled";
            return result;
        }

        net::set_recv_timeout(socket_fd, opt.timeout_secs);
        net::set_send_timeout(socket_fd, opt.timeout_secs);

        if (connect(socket_fd, (const sockaddr*)&ep.addr, ep.len) != -1) {
            connected = true;
            break;
        }

        if (opt.on_socket_done) opt.on_socket_done(socket_fd);
        net::close(socket_fd);
    }

    if (!connected) {
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
            if (opt.on_socket_done) opt.on_socket_done(socket_fd);
            net::close(socket_fd);
            return result;
        }
    } else {
        conn = std::make_unique<PlainConn>(socket_fd);
    }

    StwpRequest req;
    req.method = opt.method;
    req.path = parsed.path;
    for (const auto& [name, value] : opt.headers) req.headers[name] = value;
    req.headers["Host"] = format_host(parsed.host) +
        (parsed.port == default_port_for(parsed.scheme) ? "" : ":" + port_str);
    req.headers["User-Agent"] = "Starmap/1.0";
    req.headers["Connection"] = "close";
    if (!opt.body.empty()) {
        req.body = opt.body;
        req.headers["Content-Length"] = std::to_string(opt.body.size());
    }

    std::string serialized_req = req.serialize();
    if (!write_all(*conn, serialized_req.data(), serialized_req.size())) {
        result.error_message = "Failed to send request.";
        if (opt.on_socket_done) opt.on_socket_done(socket_fd);
        return result;
    }

    std::string raw_response;
    char recv_buf[65536];
    bool too_large = false;

    // Headers come first regardless: whether the body can be streamed past the
    // caller depends on what the response turns out to be.
    constexpr size_t kMaxHeaderBytes = 64u * 1024u;
    size_t header_end = std::string::npos;
    size_t sep_len = 0;
    bool headers_done = false;

    while (true) {
        if ((header_end = raw_response.find("\r\n\r\n")) != std::string::npos) {
            sep_len = 4;
            headers_done = true;
            break;
        }
        if ((header_end = raw_response.find("\n\n")) != std::string::npos) {
            sep_len = 2;
            headers_done = true;
            break;
        }
        if (raw_response.size() > kMaxHeaderBytes) {
            too_large = true;
            break;
        }
        net::ssize_t_ n = conn->read(recv_buf, sizeof(recv_buf));
        if (n < 0) {
            result.error_message = "Socket read failure.";
            break;
        }
        if (n == 0) break;
        raw_response.append(recv_buf, n);
    }

    StwpResponse head_msg;
    const bool head_ok =
        headers_done &&
        parse_response_headers(std::string_view(raw_response).substr(0, header_end), head_msg);

    BodySink sink = opt.on_body_chunk;
    if (head_ok && !sink && opt.on_headers) {
        sink = opt.on_headers(head_msg.status_code, head_msg.headers);
    }

    if (sink) {
        size_t declared = 0;
        auto cl = head_msg.headers.find("content-length");
        if (cl != head_msg.headers.end()) {
            try { declared = std::stoull(cl->second); } catch (...) {}
        }

        bool aborted = false;
        size_t written = 0;
        std::string prefix = raw_response.substr(header_end + sep_len);
        if (!prefix.empty()) {
            size_t take = declared ? std::min(prefix.size(), declared) : prefix.size();
            if (!sink(prefix.data(), take)) aborted = true;
            written += take;
        }

        while (!aborted && (declared == 0 || written < declared)) {
            net::ssize_t_ n = conn->read(recv_buf, sizeof(recv_buf));
            if (n < 0) {
                result.error_message = "Socket read failure.";
                aborted = true;
                break;
            }
            if (n == 0) break;
            size_t take = (size_t)n;
            if (declared && written + take > declared) take = declared - written;
            if (!sink(recv_buf, take)) {
                aborted = true;
                break;
            }
            written += take;
        }

        if (opt.on_socket_done) opt.on_socket_done(socket_fd);
        conn.reset();

        if (!aborted && (declared == 0 || written == declared)) {
            result.success = true;
            result.status_code = head_msg.status_code;
            result.status_text = head_msg.status_text;
            result.headers = head_msg.headers;
        } else if (result.error_message.empty()) {
            result.error_message = "Body transfer incomplete.";
        }
        return result;
    }

    // Buffered: pull in the rest of the message for parse_response below.
    while (headers_done && !too_large && result.error_message.empty()) {
        net::ssize_t_ bytes_received = conn->read(recv_buf, sizeof(recv_buf));
        if (bytes_received < 0) {
            result.error_message = "Socket read failure.";
            break;
        }
        if (bytes_received == 0) {
            break;
        }
        if (raw_response.size() + (size_t)bytes_received > opt.max_response_bytes) {
            too_large = true;
            break;
        }
        raw_response.append(recv_buf, bytes_received);
    }

    if (opt.on_socket_done) opt.on_socket_done(socket_fd);
    conn.reset();

    if (too_large) {
        result.error_message = "Response exceeds size limit.";
        return result;
    }
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

// Mirrors the content-type/extension dispatch below, so the streaming decision and
// the "this page is a video" decision cannot disagree.
static bool looks_like_media(const std::string& content_type, const std::string& url) {
    if (!content_type.empty() && content_type != "application/octet-stream") {
        return content_type.rfind("video/", 0) == 0 || content_type.rfind("audio/", 0) == 0;
    }
    auto parsed = parse_url(url);
    if (!parsed) return false;
    auto dot = parsed->path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = parsed->path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return ext == ".mp4" || ext == ".mov" || ext == ".m4v" ||
           ext == ".mp3" || ext == ".wav" || ext == ".aac" || ext == ".m4a";
}

FetchResult perform_fetch(int tab_id, const std::string& url_str, bool is_main_resource,
                          RequestOptions opt) {
    opt.on_socket = [tab_id, &url_str, is_main_resource](net::socket_t fd) {
        std::lock_guard<std::mutex> lock(fetch_mutex);
        Tab* tab = find_tab_by_id(tab_id);
        if (!tab) return false;
        if (is_main_resource && url_str != tab->current_url) return false;
        if (is_main_resource) tab->active_socket_fd = fd;
        return true;
    };
    opt.on_socket_done = [tab_id, is_main_resource](net::socket_t fd) {
        if (!is_main_resource) return;
        std::lock_guard<std::mutex> lock(fetch_mutex);
        Tab* tab = find_tab_by_id(tab_id);
        if (tab && tab->active_socket_fd == fd) tab->active_socket_fd = net::kInvalidSocket;
    };

    FetchResult result = perform_request(url_str, opt);
    if (!result.success && result.error_message == "Cancelled") {
        // Distinguishes a closed tab from a superseded navigation for the caller.
        std::lock_guard<std::mutex> lock(fetch_mutex);
        if (!find_tab_by_id(tab_id)) result.error_message = "Tab closed";
    }
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
        // Navigating straight at a video means the response is the whole file, so it
        // goes to disk as it arrives rather than through memory. Only decidable once
        // the headers name a content type.
        // Navigating straight at a video: the headers are all that is needed to build
        // the page, and the body is dropped so the player can stream it by range
        // instead of the whole file arriving before anything renders.
        std::string media_ct;
        bool media_nav = false;

        RequestOptions opt;
        opt.on_headers = [&](int status,
                             const std::unordered_map<std::string, std::string>& headers) -> BodySink {
            if (status != 200) return {};
            std::string ct;
            auto it = headers.find("content-type");
            if (it != headers.end()) {
                ct = it->second;
                std::transform(ct.begin(), ct.end(), ct.begin(),
                               [](unsigned char c) { return std::tolower(c); });
            }
            if (!looks_like_media(ct, final_url)) return {};
            media_ct = ct.empty() ? "video/mp4" : ct;
            media_nav = true;
            return [](const char*, std::size_t) { return false; };  // stop the transfer
        };

        FetchResult res = perform_fetch(tab_id, final_url, true, opt);

        if (media_nav) {
            res.success = true;
            res.status_code = 200;
            res.status_text = "OK";
            res.error_message.clear();
            res.headers["content-type"] = media_ct;
        }

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
                // No size: the renderer takes it from the video itself, the same way
                // navigating straight at an image sizes from the image.
                res.dom.children.push_back(video_node);
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

                // Media is not fetched here at all: the player streams it on demand.
                // The scan still runs so a mixed-content load is reported rather than
                // silently attempted later by the renderer.
                std::vector<std::string> media_srcs;
                find_media_in_dom(res.dom, media_srcs);
                for (const auto& src : media_srcs) {
                    std::string media_url = resolve_url(final_url, src);
                    if (is_mixed_content(page_secure, media_url)) {
                        std::cerr << "[mixed-content] blocked media " << media_url << "\n";
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
