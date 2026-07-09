#pragma once

#include <string>
#include <unordered_map>
#include <sstream>
#include <vector>
#include <string_view>
#include <algorithm>
#include <cctype>

struct StwpRequest {
    std::string method;
    std::string path;
    std::string version = "STWP/1.0";
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    std::string serialize() const {
        std::ostringstream oss;
        oss << method << " " << path << " " << version << "\r\n";
        for (const auto& [name, value] : headers) {
            oss << name << ": " << value << "\r\n";
        }
        oss << "\r\n";
        oss << body;
        return oss.str();
    }
};

struct StwpResponse {
    std::string version = "STWP/1.0";
    int status_code = 200;
    std::string status_text = "OK";
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    std::string serialize() const {
        std::ostringstream oss;
        oss << version << " " << status_code << " " << status_text << "\r\n";
        for (const auto& [name, value] : headers) {
            oss << name << ": " << value << "\r\n";
        }
        oss << "\r\n";
        oss << body;
        return oss.str();
    }
};

inline std::string trim(std::string_view str) {
    auto start = str.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return "";
    auto end = str.find_last_not_of(" \t\r\n");
    return std::string(str.substr(start, end - start + 1));
}

inline std::pair<std::string, std::string> parse_header_line(std::string_view line) {
    auto colon = line.find(':');
    if (colon == std::string_view::npos) return {};
    std::string name = trim(line.substr(0, colon));
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
    std::string value = trim(line.substr(colon + 1));
    return {name, value};
}

inline bool parse_request(const std::string& raw_data, size_t& bytes_consumed, StwpRequest& req) {
    auto header_end = raw_data.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        header_end = raw_data.find("\n\n");
        if (header_end == std::string::npos) return false;
    }

    size_t header_len = (raw_data[header_end] == '\r') ? 4 : 2;
    std::string_view headers_part = std::string_view(raw_data).substr(0, header_end);

    std::vector<std::string_view> lines;
    size_t start = 0;
    while (true) {
        auto pos = headers_part.find('\n', start);
        if (pos == std::string_view::npos) {
            lines.push_back(headers_part.substr(start));
            break;
        }
        lines.push_back(headers_part.substr(start, pos - start));
        start = pos + 1;
    }

    if (lines.empty()) return false;

    // Parse request line: METHOD PATH VERSION
    std::string req_line = trim(lines[0]);
    auto first_space = req_line.find(' ');
    if (first_space == std::string::npos) return false;
    req.method = req_line.substr(0, first_space);

    auto second_space = req_line.find(' ', first_space + 1);
    if (second_space == std::string::npos) return false;
    req.path = req_line.substr(first_space + 1, second_space - (first_space + 1));
    req.version = req_line.substr(second_space + 1);

    // Parse headers
    size_t content_length = 0;
    for (size_t i = 1; i < lines.size(); ++i) {
        auto line = lines[i];
        if (trim(line).empty()) continue;
        auto [name, value] = parse_header_line(line);
        if (!name.empty()) {
            req.headers[name] = value;
            if (name == "content-length") {
                try {
                    content_length = std::stoull(value);
                } catch (...) {}
            }
        }
    }

    size_t total_required_len = header_end + header_len + content_length;
    if (raw_data.size() < total_required_len) {
        return false;
    }

    req.body = raw_data.substr(header_end + header_len, content_length);
    bytes_consumed = total_required_len;
    return true;
}

inline bool parse_response(const std::string& raw_data, size_t& bytes_consumed, StwpResponse& res) {
    auto header_end = raw_data.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        header_end = raw_data.find("\n\n");
        if (header_end == std::string::npos) return false;
    }

    size_t header_len = (raw_data[header_end] == '\r') ? 4 : 2;
    std::string_view headers_part = std::string_view(raw_data).substr(0, header_end);

    std::vector<std::string_view> lines;
    size_t start = 0;
    while (true) {
        auto pos = headers_part.find('\n', start);
        if (pos == std::string_view::npos) {
            lines.push_back(headers_part.substr(start));
            break;
        }
        lines.push_back(headers_part.substr(start, pos - start));
        start = pos + 1;
    }

    if (lines.empty()) return false;

    // Parse status line: VERSION STATUS_CODE STATUS_TEXT
    std::string res_line = trim(lines[0]);
    auto first_space = res_line.find(' ');
    if (first_space == std::string::npos) return false;
    res.version = res_line.substr(0, first_space);

    auto second_space = res_line.find(' ', first_space + 1);
    if (second_space == std::string::npos) {
        try {
            res.status_code = std::stoi(trim(res_line.substr(first_space + 1)));
        } catch (...) {
            return false;
        }
        res.status_text = "";
    } else {
        try {
            res.status_code = std::stoi(trim(res_line.substr(first_space + 1, second_space - (first_space + 1))));
        } catch (...) {
            return false;
        }
        res.status_text = trim(res_line.substr(second_space + 1));
    }

    size_t content_length = 0;
    for (size_t i = 1; i < lines.size(); ++i) {
        auto line = lines[i];
        if (trim(line).empty()) continue;
        auto [name, value] = parse_header_line(line);
        if (!name.empty()) {
            res.headers[name] = value;
            if (name == "content-length") {
                try {
                    content_length = std::stoull(value);
                } catch (...) {}
            }
        }
    }

    size_t total_required_len = header_end + header_len + content_length;
    if (raw_data.size() < total_required_len) {
        return false;
    }

    res.body = raw_data.substr(header_end + header_len, content_length);
    bytes_consumed = total_required_len;
    return true;
}
