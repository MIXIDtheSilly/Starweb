#pragma once

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <string_view>

namespace proxy {

class Log {
public:
    Log() = default;
    Log(const Log&) = delete;
    Log& operator=(const Log&) = delete;
    ~Log() {
        if (owned_ && out_) std::fclose(out_);
    }

    bool open(const std::string& path, FILE* fallback, std::string& err) {
        if (path == "off") return true;
        if (path.empty()) {
            out_ = fallback;
            return true;
        }
        out_ = std::fopen(path.c_str(), "a");
        if (!out_) {
            err = "cannot open log \"" + path + "\": " + std::strerror(errno);
            return false;
        }
        owned_ = true;
        return true;
    }

    void write(std::string line) {
        if (!out_) return;
        line += '\n';
        std::lock_guard<std::mutex> lock(mu_);
        std::fwrite(line.data(), 1, line.size(), out_);
        std::fflush(out_);
    }

private:
    FILE* out_ = nullptr;
    bool owned_ = false;
    std::mutex mu_;
};

inline std::string local_time(const char* fmt) {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[64];
    size_t n = std::strftime(buf, sizeof(buf), fmt, &tm);
    return std::string(buf, n);
}

// Request fields are client-controlled, so they must not be able to forge a log line.
inline std::string log_safe(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7f) {
            char hex[5];
            std::snprintf(hex, sizeof(hex), "\\x%02x", c);
            out += hex;
        } else {
            if (c == '"' || c == '\\') out += '\\';
            out += (char)c;
        }
    }
    return out;
}

} // namespace proxy
