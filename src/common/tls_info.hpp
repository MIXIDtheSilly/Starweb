#pragma once
// Separate from tls.hpp so render code can hold a TlsInfo without pulling in
// <openssl/ssl.h>.

#include <string>

struct TlsInfo {
    std::string version;        // e.g. "TLSv1.3"
    std::string cipher;         // e.g. "TLS_AES_256_GCM_SHA384"
    std::string alpn;           // negotiated ALPN protocol, e.g. "stwp/1.0"
    std::string peer_subject;   // server leaf subject
    std::string peer_issuer;    // signing CA
    std::string not_before;
    std::string not_after;
    long verify_result = 0;     // X509_V_OK on success
    bool verified = false;
    bool resumed = false;       // handshake resumed a cached session
};
