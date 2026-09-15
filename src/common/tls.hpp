#pragma once
// TLS 1.3 transport for star://. TlsContext wraps a shared SSL_CTX; TlsConn is
// one handshaken connection.

#include "conn.hpp"
#include "tls_info.hpp"
#include <openssl/ssl.h>
#include <string>
#include <functional>
#include <memory>

// All contexts are TLS 1.3-only; a client trusts its CA and nothing else.
class TlsContext {
public:
    static std::unique_ptr<TlsContext> make_server(const std::string& cert_path,
                                                    const std::string& key_path,
                                                    std::string& err);
    static std::unique_ptr<TlsContext> make_client(const std::string& ca_path,
                                                    std::string& err);
    // CA as PEM text instead of a file path.
    static std::unique_ptr<TlsContext> make_client_pem(const std::string& ca_pem,
                                                        std::string& err);
    ~TlsContext();
    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;

    SSL_CTX* raw() const { return ctx_; }

    void set_sni_selector(std::function<TlsContext*(const std::string&)> selector);

private:
    explicit TlsContext(SSL_CTX* ctx) : ctx_(ctx) {}
    SSL_CTX* ctx_ = nullptr;
    std::function<TlsContext*(const std::string&)> sni_selector_;
};

class TlsConn : public Conn {
public:
    // Both leave the fd to the caller on failure.
    static std::unique_ptr<TlsConn> accept(TlsContext& ctx, net::socket_t fd,
                                           std::string& err);
    // hostname is verified against the cert. A non-empty session_key ("host:port")
    // opts into the session cache, so later connections to it resume.
    static std::unique_ptr<TlsConn> connect(TlsContext& ctx, net::socket_t fd,
                                            const std::string& hostname,
                                            const std::string& session_key,
                                            std::string& err);
    ~TlsConn() override;

    net::ssize_t_ read(void* buf, size_t len) override;
    net::ssize_t_ write(const void* buf, size_t len) override;
    net::socket_t fd() const override { return fd_; }
    void shutdown_write() override;
    void close() override;

    const TlsInfo& info() const { return info_; }
    const TlsInfo* tls_info() const override { return &info_; }

private:
    TlsConn(SSL* ssl, net::socket_t fd) : ssl_(ssl), fd_(fd) {}
    void capture_info();

    SSL* ssl_ = nullptr;
    net::socket_t fd_ = net::kInvalidSocket;
    std::string session_key_;
    TlsInfo info_;
    bool shutdown_sent_ = false;
};
