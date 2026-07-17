#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstring>
#include <memory>
#include "../common/net.hpp"
#include "../common/conn.hpp"
#include "../common/tls.hpp"
#include "../common/stwp_msg.hpp"

std::string sanitize_path(std::string path) {
    // Prevent directory traversal
    if (path.find("..") != std::string::npos) {
        return "";
    }
    // Strip query parameters and fragment identifiers
    auto query_pos = path.find('?');
    if (query_pos != std::string::npos) {
        path = path.substr(0, query_pos);
    }
    auto hash_pos = path.find('#');
    if (hash_pos != std::string::npos) {
        path = path.substr(0, hash_pos);
    }
    // Default to /index.html if path is root
    if (path == "/" || path.empty()) {
        path = "/index.html";
    }
    return path;
}

std::string get_content_type(const std::string& path) {
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string ext = path.substr(dot);
    
    // Standard and custom mime types
    if (ext == ".html" || ext == ".htm") return "text/html";
    if (ext == ".css") return "text/css";
    if (ext == ".lua") return "application/x-lua";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif") return "image/gif";
    if (ext == ".txt") return "text/plain";
    if (ext == ".mov" || ext == ".mp4") return "video/mp4";
    if (ext == ".mp3") return "audio/mpeg";
    return "application/octet-stream";
}

void handle_client(std::unique_ptr<Conn> conn, const char* transport) {
    std::string buffer;
    char temp_buf[4096];
    StwpRequest req;
    size_t bytes_consumed = 0;
    bool request_parsed = false;

    // Set socket receive timeout (5 seconds)
    net::set_recv_timeout(conn->fd(), 5);

    while (true) {
        net::ssize_t_ bytes_received = conn->read(temp_buf, sizeof(temp_buf));
        if (bytes_received <= 0) {
            break; // Connection closed or timeout
        }
        buffer.append(temp_buf, bytes_received);
        if (parse_request(buffer, bytes_consumed, req)) {
            request_parsed = true;
            break;
        }
    }

    if (!request_parsed) {
        StwpResponse res;
        res.status_code = 400;
        res.status_text = "Bad Request";
        res.body = "Failed to parse STWP request.";
        res.headers["Content-Length"] = std::to_string(res.body.size());
        res.headers["Content-Type"] = "text/plain";
        res.headers["Connection"] = "close";
        std::string res_str = res.serialize();
        write_all(*conn, res_str.data(), res_str.size());
        return;
    }

    std::cout << "[Server] [" << transport << "] Request: " << req.method << " " << req.path
              << " " << req.version << std::endl;

    StwpResponse res;
    res.headers["Server"] = "StarWeb/1.0";
    res.headers["Connection"] = "close";

    // An HTTP/1.1 request line parses fine here, so without this an HTTP client
    // would be served content.
    if (req.version != "STWP/1.0") {
        res.status_code = 505;
        res.status_text = "Version Not Supported";
        res.body = "This server speaks STWP/1.0 only.";
        res.headers["Content-Length"] = std::to_string(res.body.size());
        res.headers["Content-Type"] = "text/plain";
            } else if (req.method != "GET") {
        res.status_code = 405;
        res.status_text = "Method Not Allowed";
        res.body = "Only GET method is supported.";
        res.headers["Content-Length"] = std::to_string(res.body.size());
        res.headers["Content-Type"] = "text/plain";
    } else {
        std::string safe_path = sanitize_path(req.path);
        if (safe_path.empty()) {
            res.status_code = 403;
            res.status_text = "Forbidden";
            res.body = "Access Denied.";
            res.headers["Content-Length"] = std::to_string(res.body.size());
            res.headers["Content-Type"] = "text/plain";
        } else {
            std::string file_path = "www" + safe_path;
            std::ifstream file(file_path, std::ios::binary);
            if (!file) {
                res.status_code = 404;
                res.status_text = "Not Found";
                res.body = "File not found: " + safe_path;
                res.headers["Content-Length"] = std::to_string(res.body.size());
                res.headers["Content-Type"] = "text/plain";
            } else {
                std::stringstream ss;
                ss << file.rdbuf();
                res.body = ss.str();
                res.status_code = 200;
                res.status_text = "OK";
                res.headers["Content-Length"] = std::to_string(res.body.size());
                res.headers["Content-Type"] = get_content_type(safe_path);
            }
        }
    }

    std::string res_str = res.serialize();
    write_all(*conn, res_str.data(), res_str.size());
}

// An IPv6 socket with V6ONLY off also serves IPv4 peers as v4-mapped addresses.
// Falls back to IPv4 where that is refused or IPv6 is unavailable.
net::socket_t make_listener(int port) {
    net::socket_t fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (net::is_valid(fd)) {
        net::enable_reuseaddr(fd);
        int v6only = 0;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&v6only, sizeof(v6only));

        sockaddr_in6 address6{};
        address6.sin6_family = AF_INET6;
        address6.sin6_addr = in6addr_any;
        address6.sin6_port = htons(port);

        if (bind(fd, (struct sockaddr*)&address6, sizeof(address6)) == 0 &&
            listen(fd, 10) == 0) {
            return fd;
                }
        net::close(fd);
        }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (!net::is_valid(fd)) return net::kInvalidSocket;
    net::enable_reuseaddr(fd);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        net::close(fd);
        return net::kInvalidSocket;
    }
    if (listen(fd, 10) < 0) {
        net::close(fd);
        return net::kInvalidSocket;
    }
    return fd;
}

// The handshake runs here, in the worker thread, so a slow client can't stall
// the accept loop.
void serve_conn(net::socket_t fd, TlsContext* tls) {
    std::unique_ptr<Conn> conn;
    const char* transport = "moon/plain";
    if (tls) {
        net::set_recv_timeout(fd, 5);
        std::string err;
        auto tconn = TlsConn::accept(*tls, fd, err);
        if (!tconn) {
            std::cerr << "[Server] TLS handshake failed: " << err << std::endl;
            net::close(fd);
            return;
        }
        transport = tconn->info().resumed ? "star/TLS resumed" : "star/TLS full";
        conn = std::move(tconn);
    } else {
        conn = std::make_unique<PlainConn>(fd);
    }
    handle_client(std::move(conn), transport);
}

void accept_loop(net::socket_t listener, TlsContext* tls) {
    while (true) {
        sockaddr_storage client_address{};  // fits an IPv4 or IPv6 peer
        socklen_t addr_len = sizeof(client_address);
        net::socket_t client_fd = accept(listener, (struct sockaddr*)&client_address, &addr_len);
        if (!net::is_valid(client_fd)) {
            std::cerr << "Accept connection failed." << std::endl;
            continue;
        }
        std::thread(serve_conn, client_fd, tls).detach();
    }
}

int main(int argc, char* argv[]) {
    net::Startup net_startup;

    int port = 8090;
    int tls_port = 8490;
    std::string cert_path = "certs/localhost.pem";
    std::string key_path = "certs/localhost.key";
    bool tls_enabled = true;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--tls-port" && i + 1 < argc) {
            try { tls_port = std::stoi(argv[++i]); } catch (...) {}
        } else if (a == "--cert" && i + 1 < argc) {
            cert_path = argv[++i];
        } else if (a == "--key" && i + 1 < argc) {
            key_path = argv[++i];
        } else if (a == "--no-tls") {
            tls_enabled = false;
        } else {
            try { port = std::stoi(a); }
            catch (...) { std::cerr << "Ignoring argument: " << a << std::endl; }
        }
    }

    std::unique_ptr<TlsContext> tls_ctx;
    if (tls_enabled) {
        std::string err;
        tls_ctx = TlsContext::make_server(cert_path, key_path, err);
        if (!tls_ctx) {
            std::cerr << "[Server] TLS (star://) disabled: " << err << "\n"
                      << "          run tools/make_certs.sh to generate " << cert_path << std::endl;
        }
    }

    net::socket_t plain_listener = make_listener(port);
    if (!net::is_valid(plain_listener)) {
        std::cerr << "Failed to listen on port " << port << "." << std::endl;
        return 1;
    }
    std::cout << "[Server] STWP (moon://) listening on port " << port << std::endl;

    std::thread tls_thread;
    if (tls_ctx) {
        net::socket_t tls_listener = make_listener(tls_port);
        if (!net::is_valid(tls_listener)) {
            std::cerr << "Failed to listen on TLS port " << tls_port << "." << std::endl;
        } else {
            std::cout << "[Server] STWP-over-TLS (star://) listening on port " << tls_port << std::endl;
            tls_thread = std::thread(accept_loop, tls_listener, tls_ctx.get());
        }
    }

    accept_loop(plain_listener, nullptr);

    if (tls_thread.joinable()) tls_thread.join();
    net::close(plain_listener);
    return 0;
}
