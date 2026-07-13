#include <iostream>
#include <string>
#include "../common/net.hpp"
#include "../common/url_parser.hpp"
#include "../common/stwp_msg.hpp"

int main(int argc, char* argv[]) {
    net::Startup net_startup;

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <moon://url>" << std::endl;
        return 1;
    }

    std::string url_str = argv[1];
    auto opt_parsed = parse_url(url_str);
    if (!opt_parsed) {
        std::cerr << "Error: Invalid URL format. Expected: moon://host[:port]/path" << std::endl;
        return 1;
    }

    auto parsed = *opt_parsed;
    if (parsed.scheme != "moon") {
        std::cerr << "Error: Only 'moon://' scheme is supported for now" << std::endl;
        return 1;
    }

    std::cout << "[Client] Connecting to host: " << parsed.host << ", port: " << parsed.port << "..." << std::endl;

    // Set up hints for address resolution
    struct addrinfo hints{}, *res_info;
    hints.ai_family = AF_INET; // server only binds an AF_INET listening socket
    hints.ai_socktype = SOCK_STREAM;

    std::string port_str = std::to_string(parsed.port);
    int status = getaddrinfo(parsed.host.c_str(), port_str.c_str(), &hints, &res_info);
    if (status != 0) {
        std::cerr << "Error: getaddrinfo failed: " << gai_strerror(status) << std::endl;
        return 1;
    }

    net::socket_t socket_fd = net::kInvalidSocket;
    struct addrinfo* rp;
    for (rp = res_info; rp != nullptr; rp = rp->ai_next) {
        socket_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (!net::is_valid(socket_fd)) continue;

        if (connect(socket_fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break; // Successfully connected
        }
        net::close(socket_fd);
    }

    freeaddrinfo(res_info);

    if (rp == nullptr) {
        std::cerr << "Error: Could not connect to " << parsed.host << " on port " << parsed.port << std::endl;
        return 1;
    }

    std::cout << "[Client] Socket connected successfully. Sending GET request for: " << parsed.path << std::endl;

    // Construct STWP Request
    StwpRequest req;
    req.method = "GET";
    req.path = parsed.path;
    req.headers["Host"] = parsed.host + (parsed.port == 8090 ? "" : ":" + port_str);
    req.headers["User-Agent"] = "StarClient/1.0";
    req.headers["Connection"] = "close";

    std::string serialized_req = req.serialize();
    if (send(socket_fd, serialized_req.data(), serialized_req.size(), 0) < 0) {
        std::cerr << "Error: Failed to send data to server." << std::endl;
        net::close(socket_fd);
        return 1;
    }

    // Read full response until socket closes
    std::string raw_response;
    char recv_buf[4096];
    while (true) {
        net::ssize_t_ bytes_received = recv(socket_fd, recv_buf, sizeof(recv_buf), 0);
        if (bytes_received < 0) {
            std::cerr << "Error: Socket read failure." << std::endl;
            net::close(socket_fd);
            return 1;
        }
        if (bytes_received == 0) {
            break; // Server closed connection
        }
        raw_response.append(recv_buf, bytes_received);
    }
    net::close(socket_fd);

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
