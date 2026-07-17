#include "tls.hpp"
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace {

// ALPN wire form: one length-prefixed protocol, "stwp/1.0".
const unsigned char kAlpn[] = {8, 's', 't', 'w', 'p', '/', '1', '.', '0'};

// Every fetch opens its own connection, so without this a page's subresources
// each pay a full handshake.
class SessionCache {
public:
    SSL_SESSION* get(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(key);
        if (it == sessions_.end()) return nullptr;
        SSL_SESSION_up_ref(it->second);
        return it->second;
    }
    void put(const std::string& key, SSL_SESSION* session) {
        std::lock_guard<std::mutex> lock(mutex_);
        SSL_SESSION*& slot = sessions_[key];
        if (slot) SSL_SESSION_free(slot);
        slot = session;
    }
private:
    std::mutex mutex_;
    std::unordered_map<std::string, SSL_SESSION*> sessions_;
};

SessionCache g_sessions;

std::string ssl_err() {
    unsigned long e = ERR_get_error();
    if (e == 0) return "unknown TLS error";
    char buf[256];
    ERR_error_string_n(e, buf, sizeof(buf));
    return buf;
}

int alpn_select_cb(SSL*, const unsigned char** out, unsigned char* outlen,
                   const unsigned char* in, unsigned int inlen, void*) {
    if (SSL_select_next_proto(const_cast<unsigned char**>(out), outlen,
                              kAlpn, sizeof(kAlpn), in, inlen) != OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_ALERT_FATAL;
    }
    return SSL_TLSEXT_ERR_OK;
}

bool would_block() {
#if defined(_WIN32)
    int e = WSAGetLastError();
    return e == WSAETIMEDOUT || e == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

// The select callback only fires when the peer offers ALPN, so a peer offering
// none negotiates none and must be caught after the handshake.
bool negotiated_stwp(SSL* ssl) {
    const unsigned char* proto = nullptr;
    unsigned int len = 0;
    SSL_get0_alpn_selected(ssl, &proto, &len);
    return proto && len == 8 && std::memcmp(proto, "stwp/1.0", 8) == 0;
}

bool looks_like_ip(const std::string& h) {
    if (h.find(':') != std::string::npos) return true;  // IPv6
    for (char c : h) {
        if (!(std::isdigit((unsigned char)c) || c == '.')) return false;
    }
    return !h.empty();
}

std::string asn1_time_str(const ASN1_TIME* t) {
    if (!t) return "";
    BIO* bio = BIO_new(BIO_s_mem());
    if (!bio) return "";
    std::string out;
    if (ASN1_TIME_print(bio, t)) {
        char* data = nullptr;
        long n = BIO_get_mem_data(bio, &data);
        out.assign(data, (size_t)n);
    }
    BIO_free(bio);
    return out;
}

std::string x509_name_str(X509_NAME* name) {
    if (!name) return "";
    BIO* bio = BIO_new(BIO_s_mem());
    if (!bio) return "";
    std::string out;
    if (X509_NAME_print_ex(bio, name, 0, XN_FLAG_ONELINE & ~ASN1_STRFLGS_ESC_MSB)) {
        char* data = nullptr;
        long n = BIO_get_mem_data(bio, &data);
        out.assign(data, (size_t)n);
    }
    BIO_free(bio);
    return out;
}

} // namespace

std::unique_ptr<TlsContext> TlsContext::make_server(const std::string& cert_path,
                                                    const std::string& key_path,
                                                    std::string& err) {
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) { err = "SSL_CTX_new: " + ssl_err(); return nullptr; }

    SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);

    if (SSL_CTX_use_certificate_chain_file(ctx, cert_path.c_str()) != 1) {
        err = "loading cert " + cert_path + ": " + ssl_err();
        SSL_CTX_free(ctx);
        return nullptr;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, key_path.c_str(), SSL_FILETYPE_PEM) != 1) {
        err = "loading key " + key_path + ": " + ssl_err();
        SSL_CTX_free(ctx);
        return nullptr;
    }
    if (SSL_CTX_check_private_key(ctx) != 1) {
        err = "cert/key mismatch: " + ssl_err();
        SSL_CTX_free(ctx);
        return nullptr;
    }

    SSL_CTX_set_alpn_select_cb(ctx, alpn_select_cb, nullptr);
    return std::unique_ptr<TlsContext>(new TlsContext(ctx));
}

std::unique_ptr<TlsContext> TlsContext::make_client(const std::string& ca_path,
                                                    std::string& err) {
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) { err = "SSL_CTX_new: " + ssl_err(); return nullptr; }

    SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);

    if (SSL_CTX_load_verify_locations(ctx, ca_path.c_str(), nullptr) != 1) {
        err = "loading CA " + ca_path + ": " + ssl_err();
        SSL_CTX_free(ctx);
        return nullptr;
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    return std::unique_ptr<TlsContext>(new TlsContext(ctx));
}

TlsContext::~TlsContext() {
    if (ctx_) SSL_CTX_free(ctx_);
}

std::unique_ptr<TlsConn> TlsConn::accept(TlsContext& ctx, net::socket_t fd,
                                         std::string& err) {
    SSL* ssl = SSL_new(ctx.raw());
    if (!ssl) { err = "SSL_new: " + ssl_err(); return nullptr; }
    SSL_set_fd(ssl, (int)fd);
    if (SSL_accept(ssl) != 1) {
        err = "TLS handshake failed: " + ssl_err();
        SSL_free(ssl);
        return nullptr;
    }
    if (!negotiated_stwp(ssl)) {
        err = "client did not negotiate ALPN stwp/1.0";
        SSL_free(ssl);
        return nullptr;
    }
    auto conn = std::unique_ptr<TlsConn>(new TlsConn(ssl, fd));
    conn->capture_info();
    return conn;
}

std::unique_ptr<TlsConn> TlsConn::connect(TlsContext& ctx, net::socket_t fd,
                                          const std::string& hostname,
                                          const std::string& session_key,
                                          std::string& err) {
    SSL* ssl = SSL_new(ctx.raw());
    if (!ssl) { err = "SSL_new: " + ssl_err(); return nullptr; }
    SSL_set_fd(ssl, (int)fd);

    if (!session_key.empty()) {
        if (SSL_SESSION* cached = g_sessions.get(session_key)) {
            SSL_set_session(ssl, cached);  // up-refs internally
            SSL_SESSION_free(cached);
        }
    }

    X509_VERIFY_PARAM* vp = SSL_get0_param(ssl);
    X509_VERIFY_PARAM_set_hostflags(vp, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    if (looks_like_ip(hostname)) {
        X509_VERIFY_PARAM_set1_ip_asc(vp, hostname.c_str());
    } else {
        X509_VERIFY_PARAM_set1_host(vp, hostname.c_str(), 0);
        SSL_set_tlsext_host_name(ssl, hostname.c_str());  // SNI: DNS names only
    }

    SSL_set_alpn_protos(ssl, kAlpn, sizeof(kAlpn));

    if (SSL_connect(ssl) != 1) {
        err = "TLS handshake failed: " + ssl_err();
        SSL_free(ssl);
        return nullptr;
    }
    if (!negotiated_stwp(ssl)) {
        err = "server did not negotiate ALPN stwp/1.0";
        SSL_free(ssl);
        return nullptr;
    }
    auto conn = std::unique_ptr<TlsConn>(new TlsConn(ssl, fd));
    conn->session_key_ = session_key;
    conn->capture_info();
    return conn;
}

void TlsConn::capture_info() {
    info_.version = SSL_get_version(ssl_);
    if (const char* c = SSL_get_cipher_name(ssl_)) info_.cipher = c;

    const unsigned char* proto = nullptr;
    unsigned int plen = 0;
    SSL_get0_alpn_selected(ssl_, &proto, &plen);
    if (proto && plen) info_.alpn.assign((const char*)proto, plen);

    info_.resumed = (SSL_session_reused(ssl_) == 1);
    info_.verify_result = SSL_get_verify_result(ssl_);
    X509* cert = SSL_get1_peer_certificate(ssl_);
    if (cert) {
        info_.peer_subject = x509_name_str(X509_get_subject_name(cert));
        info_.peer_issuer = x509_name_str(X509_get_issuer_name(cert));
        info_.not_before = asn1_time_str(X509_get0_notBefore(cert));
        info_.not_after = asn1_time_str(X509_get0_notAfter(cert));
        info_.verified = (info_.verify_result == X509_V_OK);
        X509_free(cert);
    }
}

TlsConn::~TlsConn() { close(); }

net::ssize_t_ TlsConn::read(void* buf, size_t len) {
    int n = SSL_read(ssl_, buf, (int)len);
    if (n > 0) return n;
    int e = SSL_get_error(ssl_, n);
    if (e == SSL_ERROR_ZERO_RETURN) return 0;  // clean TLS close_notify
    if (e == SSL_ERROR_SYSCALL) {
        // A timeout lands here with an empty error queue too, so it must be ruled
        // out before treating the empty queue as end-of-stream.
        if (would_block()) return -1;
        if (ERR_peek_error() == 0) return 0;  // peer closed without close_notify
    }
    return -1;
}

net::ssize_t_ TlsConn::write(const void* buf, size_t len) {
    int n = SSL_write(ssl_, buf, (int)len);
    if (n > 0) return n;
    return -1;
}

void TlsConn::close() {
    if (ssl_) {
        // TLS 1.3 sends tickets after the handshake, so collecting the session
        // any earlier than this caches nothing.
        if (!session_key_.empty()) {
            if (SSL_SESSION* session = SSL_get1_session(ssl_)) {
                if (SSL_SESSION_is_resumable(session)) {
                    g_sessions.put(session_key_, session);
                } else {
                    SSL_SESSION_free(session);
                }
            }
        }
        SSL_shutdown(ssl_);
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    if (net::is_valid(fd_)) {
        net::close(fd_);
        fd_ = net::kInvalidSocket;
    }
}
