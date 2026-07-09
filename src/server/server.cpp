#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
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
    return "application/octet-stream";
}

void handle_client(int client_fd) {
    std::string buffer;
    char temp_buf[4096];
    StwpRequest req;
    size_t bytes_consumed = 0;
    bool request_parsed = false;

    // Set socket receive timeout (5 seconds)
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    while (true) {
        ssize_t bytes_received = recv(client_fd, temp_buf, sizeof(temp_buf), 0);
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
        send(client_fd, res_str.data(), res_str.size(), 0);
        close(client_fd);
        return;
    }

    std::cout << "[Server] Request: " << req.method << " " << req.path << std::endl;

    StwpResponse res;
    res.headers["Server"] = "StarWeb/1.0";
    res.headers["Connection"] = "close";

    if (req.method != "GET") {
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
    send(client_fd, res_str.data(), res_str.size(), 0);
    close(client_fd);
}

int main(int argc, char* argv[]) {
    int port = 8090;
    if (argc > 1) {
        try {
            port = std::stoi(argv[1]);
        } catch (...) {
            std::cerr << "Invalid port argument. Using default 8090." << std::endl;
        }
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "Failed to create socket." << std::endl;
        return 1;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "setsockopt (SO_REUSEADDR) failed." << std::endl;
        close(server_fd);
        return 1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "Bind failed on port " << port << "." << std::endl;
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 10) < 0) {
        std::cerr << "Listen failed." << std::endl;
        close(server_fd);
        return 1;
    }

    std::cout << "[Server] StarWeb STWP Server listening on port " << port << std::endl;

    while (true) {
        sockaddr_in client_address{};
        socklen_t addr_len = sizeof(client_address);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_address, &addr_len);
        if (client_fd < 0) {
            // Check for accept errors, but keep server running
            std::cerr << "Accept connection failed." << std::endl;
            continue;
        }

        std::thread t(handle_client, client_fd);
        t.detach();
    }

    close(server_fd);
    return 0;
}
