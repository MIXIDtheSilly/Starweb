#include <iostream>
#include <string>
#include <memory>
#include <cstdlib>
#include "../common/net.hpp"
#include "../common/resolver.hpp"
#include "../common/conn.hpp"
#include "../common/tls.hpp"
#include "../common/url_parser.hpp"
#include "../common/stwp_msg.hpp"

int main(int argc, char* argv[]) {
    net::Startup net_startup;

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <moon://url | star://url>" << std::endl;
        return 1;
    }

    std::string url_str = argv[1];
    auto opt_parsed = parse_url(url_str);
    if (!opt_parsed) {
        std::cerr << "Error: Invalid URL format. Expected: moon://host[:port]/path" << std::endl;
        return 1;
    }

    auto parsed = *opt_parsed;
    if (parsed.scheme != "moon" && parsed.scheme != "star") {
        std::cerr << "Error: Unsupported scheme: " << parsed.scheme << "://" << std::endl;
        return 1;
    }
    const bool use_tls = (parsed.scheme == "star");

    std::cout << "[Client] Connecting to host: " << parsed.host << ", port: " << parsed.port << "..." << std::endl;

    // .star names are answered by StarDNS; everything else by getaddrinfo.
    std::string port_str = std::to_string(parsed.port);
    std::string resolve_err;
    auto endpoints = stardns::resolve(parsed.host, (uint16_t)parsed.port, resolve_err);
    if (endpoints.empty()) {
        std::cerr << "Error: could not resolve " << parsed.host;
        if (!resolve_err.empty()) std::cerr << ": " << resolve_err;
        std::cerr << std::endl;
        return 1;
    }
    if (stardns::is_starweb_name(parsed.host)) {
        std::cout << "[Client] " << parsed.host << " resolved by StarDNS." << std::endl;
    }

    net::socket_t socket_fd = net::kInvalidSocket;
    bool connected = false;
    for (const auto& ep : endpoints) {
        socket_fd = socket(ep.family, SOCK_STREAM, 0);
        if (!net::is_valid(socket_fd)) continue;

        if (connect(socket_fd, (const sockaddr*)&ep.addr, ep.len) == 0) {
            connected = true;
            break;
        }
        net::close(socket_fd);
    }

    if (!connected) {
        std::cerr << "Error: Could not connect to " << parsed.host << " on port " << parsed.port << std::endl;
        return 1;
    }

    std::cout << "[Client] Socket connected successfully." << std::endl;

    std::unique_ptr<TlsContext> tls_ctx;  // must outlive conn
    std::unique_ptr<Conn> conn;
    if (use_tls) {
        const char* env = std::getenv("STARWEB_CA");
        std::string ca = env ? env : "certs/starweb_root.pem";
        std::string err;
        tls_ctx = TlsContext::make_client(ca, err);
        if (!tls_ctx) {
            std::cerr << "Error: " << err << std::endl;
            net::close(socket_fd);
            return 1;
        }
        // No session key: the CLI makes a single connection and exits, so there is
        // nothing for a cache to be reused by.
        auto tconn = TlsConn::connect(*tls_ctx, socket_fd, parsed.host, "", err);
        if (!tconn) {
            std::cerr << "Error: " << err << std::endl;
            net::close(socket_fd);
            return 1;
        }
        const TlsInfo& t = tconn->info();
        std::cout << "[Client] TLS: " << t.version << ", " << t.cipher
                  << ", ALPN " << (t.alpn.empty() ? "(none)" : t.alpn) << std::endl;
        std::cout << "[Client] Cert: " << t.peer_subject << std::endl;
        std::cout << "[Client] Issuer: " << t.peer_issuer << std::endl;
        std::cout << "[Client] Expires: " << t.not_after << std::endl;
        conn = std::move(tconn);
    } else {
        conn = std::make_unique<PlainConn>(socket_fd);
    }

    std::cout << "[Client] Sending GET request for: " << parsed.path << std::endl;

    // Construct STWP Request
    StwpRequest req;
    req.method = "GET";
    req.path = parsed.path;
    req.headers["Host"] = format_host(parsed.host) +
        (parsed.port == (use_tls ? 8490 : 8090) ? "" : ":" + port_str);
    req.headers["User-Agent"] = "StarClient/1.0";
    req.headers["Connection"] = "close";

    std::string serialized_req = req.serialize();
    if (!write_all(*conn, serialized_req.data(), serialized_req.size())) {
        std::cerr << "Error: Failed to send data to server." << std::endl;
        return 1;
    }

    // Read full response until the connection closes
    std::string raw_response;
    char recv_buf[4096];
    while (true) {
        net::ssize_t_ bytes_received = conn->read(recv_buf, sizeof(recv_buf));
        if (bytes_received < 0) {
            std::cerr << "Error: Socket read failure." << std::endl;
            return 1;
        }
        if (bytes_received == 0) {
            break;
        }
        raw_response.append(recv_buf, bytes_received);
    }
    conn.reset();

    // Parse the received STWP Response
    StwpResponse res_msg;
    size_t bytes_consumed = 0;
    if (!parse_response(raw_response, bytes_consumed, res_msg)) {
        std::cerr << "Error: Response is not valid STWP." << std::endl;
        std::cout << "----- Raw Content -----\n" << raw_response << "\n-----------------------" << std::endl;
        return 1;
    }

    std::cout << "----- STWP RESPONSE HEADERS -----" << std::endl;
    std::cout << "Protocol Version: " << res_msg.version << std::endl;
    std::cout << "Status Code:      " << res_msg.status_code << std::endl;
    std::cout << "Status Message:   " << res_msg.status_text << std::endl;
    std::cout << "Headers:" << std::endl;
    for (const auto& [name, val] : res_msg.headers) {
        std::cout << "  " << name << ": " << val << std::endl;
    }
    std::cout << "----- STWP RESPONSE BODY -----" << std::endl;
    std::cout << res_msg.body << std::endl;
    std::cout << "------------------------------" << std::endl;

    return 0;
}
