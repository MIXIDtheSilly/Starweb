#include <iostream>
#include <string>
#include <thread>
#include <cstring>
#include <cstdint>
#include <csignal>
#include <algorithm>
#include <memory>
#include "../common/net.hpp"
#include "../common/conn.hpp"
#include "../common/tls.hpp"
#include "../common/stwp_msg.hpp"
#include "../common/static_files.hpp"

// Longer than the browser's pool timeout, so the client gives up first.
constexpr int kIdleTimeoutSecs = 20;
constexpr int kRequestTimeoutSecs = 5;

// `buffer` carries bytes read past the end of the previous request.
bool handle_one(Conn& conn, std::string& buffer, const char* transport, int served) {
    char temp_buf[4096];
    StwpRequest req;
    size_t bytes_consumed = 0;
    bool request_parsed = false;
    bool anything_read = false;

    if (!buffer.empty() && parse_request(buffer, bytes_consumed, req)) {
        request_parsed = true;
    }

    if (!request_parsed) {
        net::set_recv_timeout(conn.fd(), kIdleTimeoutSecs);
        while (true) {
            net::ssize_t_ bytes_received = conn.read(temp_buf, sizeof(temp_buf));
            if (bytes_received <= 0) {
                break; // Connection closed or timeout
            }
            if (!anything_read) {
                anything_read = true;
                net::set_recv_timeout(conn.fd(), kRequestTimeoutSecs);
            }
            buffer.append(temp_buf, bytes_received);
            if (parse_request(buffer, bytes_consumed, req)) {
                request_parsed = true;
                break;
            }
        }
    }

    if (!request_parsed) {
        // An idle connection closing is how keep-alive ends, not a client error.
        if (!anything_read && buffer.empty()) return false;
        static_files::send_text(conn, StwpResponse{}, 400, "Bad Request",
                                "Failed to parse STWP request.", false);
        return false;
    }

    buffer.erase(0, bytes_consumed);

    std::cout << "[Server] [" << (served ? "kept alive" : transport) << "] Request: "
              << req.method << " " << req.path << " " << req.version << std::endl;

    bool keep_alive = false;
    if (auto cn = req.headers.find("connection"); cn != req.headers.end()) {
        std::string v = cn->second;
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        keep_alive = v.find("keep-alive") != std::string::npos &&
                     v.find("close") == std::string::npos;
    }

    StwpResponse res;
    res.headers["Server"] = "StarWeb/1.0";

    // An HTTP/1.1 request line parses fine here, so without this an HTTP client
    // would be served content.
    if (req.version != "STWP/1.0") {
        return static_files::send_text(conn, res, 505, "Version Not Supported",
                                       "This server speaks STWP/1.0 only.", false).keep_alive;
    }
    if (req.method != "GET") {
        return static_files::send_text(conn, res, 405, "Method Not Allowed",
                                       "Only GET method is supported.", keep_alive).keep_alive;
    }
    return static_files::send_file(conn, "www", req.path, req, res, keep_alive).keep_alive;
}

void handle_client(std::unique_ptr<Conn> conn, const char* transport) {
    std::string buffer;
    for (int served = 0; handle_one(*conn, buffer, transport, served); ++served) {
    }
}

// The handshake runs here, in the worker thread, so a slow client can't stall
// the accept loop.
void serve_conn(net::socket_t fd, TlsContext* tls) {
    std::unique_ptr<Conn> conn;
    const char* transport = "moon/plain";
    net::set_nodelay(fd);
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

#ifndef _WIN32
    // Writing to a socket whose peer has gone would otherwise raise SIGPIPE, whose
    // default action kills the server. write_all reports it as a failed send instead.
    signal(SIGPIPE, SIG_IGN);
#endif

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

    net::socket_t plain_listener = net::listen_tcp(port);
    if (!net::is_valid(plain_listener)) {
        std::cerr << "Failed to listen on port " << port << "." << std::endl;
        return 1;
    }
    std::cout << "[Server] STWP (moon://) listening on port " << port << std::endl;

    std::thread tls_thread;
    if (tls_ctx) {
        net::socket_t tls_listener = net::listen_tcp(tls_port);
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
