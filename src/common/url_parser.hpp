#pragma once

#include <string>
#include <string_view>
#include <optional>
#include <algorithm>
#include <cctype>

struct ParsedURL {
    std::string scheme;   // "moon" or "star"
    std::string host;     // e.g., "localhost"
    int port = 8090;      // default: 8090 for moon, 8490 for star
    std::string path;     // e.g., "/index.html" (defaults to "/")
};

inline std::optional<ParsedURL> parse_url(std::string_view url) {
    constexpr std::string_view scheme_delim = "://";
    auto scheme_pos = url.find(scheme_delim);
    if (scheme_pos == std::string_view::npos) {
        return std::nullopt;
    }

    ParsedURL parsed;
    parsed.scheme = std::string(url.substr(0, scheme_pos));
    
    // Normalize scheme to lowercase
    std::transform(parsed.scheme.begin(), parsed.scheme.end(), parsed.scheme.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (parsed.scheme != "moon" && parsed.scheme != "star") {
        return std::nullopt;
    }

    // Assign default ports
    if (parsed.scheme == "moon") {
        parsed.port = 8090;
    } else if (parsed.scheme == "star") {
        parsed.port = 8490;
    }

    std::string_view rest = url.substr(scheme_pos + scheme_delim.length());
    if (rest.empty()) {
        return std::nullopt;
    }

    // Split at the first '/' to separate host/port from path
    auto path_pos = rest.find('/');
    std::string_view host_port;
    if (path_pos == std::string_view::npos) {
        host_port = rest;
        parsed.path = "/";
    } else {
        host_port = rest.substr(0, path_pos);
        parsed.path = std::string(rest.substr(path_pos));
    }

    if (host_port.empty()) {
        return std::nullopt;
    }

    // Parse host and optional port
    auto colon_pos = host_port.find(':');
    if (colon_pos == std::string_view::npos) {
        parsed.host = std::string(host_port);
    } else {
        parsed.host = std::string(host_port.substr(0, colon_pos));
        std::string port_str(host_port.substr(colon_pos + 1));
        if (port_str.empty()) {
            return std::nullopt;
        }
        try {
            parsed.port = std::stoi(port_str);
        } catch (...) {
            return std::nullopt;
        }
    }

    return parsed;
}
