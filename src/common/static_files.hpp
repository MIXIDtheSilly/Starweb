#pragma once

#include "conn.hpp"
#include "stwp_msg.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace static_files {

constexpr uintmax_t kInlineBodyMax = 64u * 1024u;

inline std::string sanitize_path(std::string path, const std::string& index = "index.html") {
    if (path.find("..") != std::string::npos) return "";
    path = path.substr(0, path.find_first_of("?#"));
    if (path.empty() || path[0] != '/') path.insert(0, "/");
    if (path.back() == '/') path += index;
    return path;
}

inline std::string content_type(const std::string& path) {
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string ext = path.substr(dot);

    if (ext == ".html" || ext == ".htm") return "text/html";
    if (ext == ".css") return "text/css";
    if (ext == ".lua") return "application/x-lua";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif") return "image/gif";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".webp") return "image/webp";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".json") return "application/json";
    if (ext == ".txt") return "text/plain";
    if (ext == ".mov" || ext == ".mp4") return "video/mp4";
    if (ext == ".mp3") return "audio/mpeg";
    return "application/octet-stream";
}

// A single `bytes=a-b`, `bytes=a-` or `bytes=-n`. Multi-range is not supported;
// false means the caller answers 416, not a full 200.
inline bool parse_byte_range(const std::string& header, uintmax_t file_size,
                             uintmax_t& start, uintmax_t& end) {
    if (file_size == 0) return false;

    const std::string prefix = "bytes=";
    if (header.rfind(prefix, 0) != 0) return false;
    std::string spec = trim(header.substr(prefix.size()));
    if (spec.find(',') != std::string::npos) return false;

    auto dash = spec.find('-');
    if (dash == std::string::npos) return false;
    std::string first = trim(spec.substr(0, dash));
    std::string last = trim(spec.substr(dash + 1));
    if (first.empty() && last.empty()) return false;

    try {
        if (first.empty()) {
            uintmax_t n = std::stoull(last);
            if (n == 0) return false;
            start = n >= file_size ? 0 : file_size - n;
            end = file_size - 1;
        } else {
            start = std::stoull(first);
            end = last.empty() ? file_size - 1 : std::stoull(last);
        }
    } catch (...) {
        return false;
    }

    if (end >= file_size) end = file_size - 1;
    return start <= end && start < file_size;
}

struct Result {
    int status = 0;
    uint64_t body_bytes = 0;
    bool keep_alive = false;
};

inline Result send_text(Conn& conn, StwpResponse res, int code, const char* text,
                        const std::string& body, bool keep_alive) {
    res.status_code = code;
    res.status_text = text;
    res.body = body;
    res.headers["Content-Type"] = "text/plain";
    res.headers["Content-Length"] = std::to_string(body.size());
    res.headers["Connection"] = keep_alive ? "keep-alive" : "close";
    std::string wire = res.serialize();
    Result out{code, body.size(), false};
    out.keep_alive = write_all(conn, wire.data(), wire.size()) && keep_alive;
    return out;
}

inline Result send_file(Conn& conn, const std::string& root, const std::string& request_path,
                        const StwpRequest& req, StwpResponse res, bool keep_alive,
                        const std::string& index = "index.html") {
    std::string safe = sanitize_path(request_path, index);
    if (safe.empty()) return send_text(conn, std::move(res), 403, "Forbidden", "Access Denied.", keep_alive);

    std::string file_path = root + safe;
    std::error_code ec;
    std::ifstream file;
    if (std::filesystem::is_regular_file(file_path, ec)) file.open(file_path, std::ios::binary);
    if (!file.is_open()) return send_text(conn, std::move(res), 404, "Not Found", "File not found: " + safe, keep_alive);

    file.seekg(0, std::ios::end);
    uintmax_t file_size = (uintmax_t)file.tellg();
    res.headers["Accept-Ranges"] = "bytes";
    res.headers["Content-Type"] = content_type(safe);

    uintmax_t offset = 0, length = file_size;
    auto range_it = req.headers.find("range");
    if (range_it != req.headers.end()) {
        uintmax_t start = 0, end = 0;
        if (!parse_byte_range(range_it->second, file_size, start, end)) {
            res.headers["Content-Range"] = "bytes */" + std::to_string(file_size);
            return send_text(conn, std::move(res), 416, "Range Not Satisfiable", "", keep_alive);
        }
        res.status_code = 206;
        res.status_text = "Partial Content";
        res.headers["Content-Range"] = "bytes " + std::to_string(start) + "-" + std::to_string(end) +
                                       "/" + std::to_string(file_size);
        offset = start;
        length = end - start + 1;
    } else {
        res.status_code = 200;
        res.status_text = "OK";
    }
    res.headers["Content-Length"] = std::to_string(length);
    res.headers["Connection"] = keep_alive ? "keep-alive" : "close";

    Result out{res.status_code, 0, false};
    file.seekg((std::streamoff)offset);

    if (length <= kInlineBodyMax) {
        res.body.resize((size_t)length);
        file.read(res.body.data(), (std::streamsize)length);
        if ((uintmax_t)file.gcount() != length) return out;
        std::string wire = res.serialize();
        out.body_bytes = length;
        out.keep_alive = write_all(conn, wire.data(), wire.size()) && keep_alive;
        return out;
    }

    // Streamed, not copied into res.body: building a 350 MB body in memory cost ~3x the file.
    std::string head = res.serialize();
    if (!write_all(conn, head.data(), head.size())) return out;
    std::vector<char> buf(64 * 1024);
    uintmax_t remaining = length;
    while (remaining > 0) {
        std::streamsize take = (std::streamsize)std::min<uintmax_t>(buf.size(), remaining);
        file.read(buf.data(), take);
        std::streamsize got = file.gcount();
        if (got <= 0) break;
        if (!write_all(conn, buf.data(), (size_t)got)) return out;
        remaining -= (uintmax_t)got;
        out.body_bytes += (uint64_t)got;
    }
    out.keep_alive = keep_alive && remaining == 0;
    return out;
}

} // namespace static_files
