# STWP and the `moon://` / `star://` schemes

StarWeb speaks **STWP/1.0**, a request/response protocol structured like HTTP but
implemented from scratch. It is carried over two URL schemes:

| Scheme | Transport | Default port | Encrypted |
|--------|-----------|--------------|-----------|
| `moon://` | TCP | 8090 | no — plaintext on the wire |
| `star://` | TCP + TLS 1.3 | 8490 | yes |

`star://` is to `moon://` what `https` is to `http`: the messages are identical,
only the transport underneath changes. `stwp_server` serves both at once, from a
single process, on the two ports.

## Message format

Unchanged between the schemes. A request:

```
GET /index.html STWP/1.0
Host: localhost
User-Agent: Starmap/1.0
Connection: close

```

A response:

```
STWP/1.0 200 OK
Content-Length: 4142
Content-Type: text/html
Connection: close

<!DOCTYPE html>...
```

Header names are case-insensitive and lowercased on parse. Bodies are delimited by
`Content-Length`. Every request currently uses `Connection: close`, so one
connection carries exactly one request/response pair.

## The `star://` connection

Establishing a `star://` connection is two handshakes stacked:

1. **TCP** — the ordinary three-way handshake (SYN, SYN-ACK, ACK) from `connect()`.
2. **TLS 1.3** — a 1-RTT handshake on top of the established socket: ClientHello,
   ServerHello, certificate, Finished.

Only then does the first STWP byte go out, encrypted.

### TLS profile

- **TLS 1.3 only.** Both the client and the server pin minimum *and* maximum
  version to TLS 1.3. There is no negotiation down to 1.2, and no cipher
  downgrade — stricter than the web, which is affordable because `star://` has no
  legacy clients to support.
- **ALPN `stwp/1.0`.** The client offers it; the server selects it and fails the
  handshake if the client offers nothing it recognises.
- **SNI** is sent for DNS hostnames, and omitted for IP literals (per RFC 6066).

### Certificate verification

A `star://` server's certificate must satisfy all of:

- **Chain** — signed by a CA in the client's trust store. The trust anchor is the
  StarWeb root CA (`certs/starweb_root.pem`), overridable via the `STARWEB_CA`
  environment variable. The system root store is not consulted.
- **Hostname** — the URL's host must match the certificate's SAN. DNS names are
  checked with `X509_VERIFY_PARAM_set1_host`, IP literals with
  `X509_VERIFY_PARAM_set1_ip_asc`. This is a separate check from the chain: a
  certificate legitimately signed by the StarWeb root but issued for a *different*
  host is rejected.
- **Validity period** and the usual X.509 constraints, enforced by OpenSSL.

Any failure aborts the handshake. The browser shows a full-page interstitial and
loads nothing; there is no click-through to proceed anyway.

### Session resumption

Each fetch opens its own connection, so a page with subresources would otherwise
pay a full handshake per resource. The client keeps an in-memory session cache
keyed by `host:port`, so the first connection to an origin handshakes in full and
the rest resume:

```
[Server] [star/TLS full]    Request: GET /index.html
[Server] [star/TLS resumed] Request: GET /style.css
[Server] [star/TLS resumed] Request: GET /cat.jpg
```

The cache lives for the life of the process and is never written to disk. 0-RTT
early data is deliberately **not** used: it is replay-unsafe, and these GETs are
cheap enough that it would buy little.

## Isolation from the public web

StarWeb is a separate web, and the boundary is enforced rather than assumed:

- **Requests must say `STWP/1.0`.** An HTTP request line parses fine here (the
  version is just a token), so the server checks it explicitly and answers anything
  else with `505 Version Not Supported`. A plain HTTP client gets no content.
- **Connections must negotiate ALPN `stwp/1.0`.** The server rejects a client that
  offers `h2`/`http/1.1`, *and* one that offers no ALPN at all — the select callback
  never fires in that case, so the check happens after the handshake instead. The
  client enforces the same in reverse. A real browser cannot complete a TLS
  handshake with a StarWeb server.
- **The root CA is name-constrained** to `localhost`, `.local`, `.star`, and
  private/loopback IP ranges. It is structurally unable to issue for a public name,
  so installing this root cannot expose the public web even if the CA key leaks.
  Verification of a `www.google.com` leaf signed by it fails with
  `permitted subtree violation`.
- **Trust is mutual and closed.** StarWeb clients trust only the StarWeb root, not
  the system store; no public CA vouches for StarWeb hosts.
- **Pages cannot reach out.** `perform_fetch` accepts only `moon://` and `star://`,
  and page scripts cannot navigate to any other scheme.

What is *not* isolated: **names**. Hosts resolve through the system resolver and
therefore the public DNS, so the namespace is still ICANN's.

## Security policy

Two rules apply to `star://` pages, both enforced in the browser:

- **Mixed content is blocked.** A `star://` page may not load `moon://`
  subresources — stylesheets, images, media, or scripts. A blocked load is logged
  and dropped; it never reaches the network.
- **Script-driven downgrades are blocked.** A page script (`location.assign`,
  `location.href = ...`) on a `star://` page cannot navigate to `moon://`. This is
  stricter than the web, where an `https` page may navigate to `http`. Typing a
  `moon://` URL by hand still works — the restriction is on pages, not on users.

## URLs

```
star://host[:port]/path
```

The port is elided from the canonical form when it is the scheme default (8490 for
`star`, 8090 for `moon`). The `Host` header follows the same rule. A URL typed
without a scheme is assumed to be `moon://`.

## Certificates for local development

`certs/` is generated locally and git-ignored — the root CA private key is never
committed.

```sh
./tools/make_certs.sh          # root CA + localhost leaf, reuses an existing root
./tools/make_certs.sh --force  # regenerate the root CA too
```

This produces a P-256 root CA (10 years) and a `localhost` leaf (825 days) with
`SAN = DNS:localhost, IP:127.0.0.1, IP:::1` and `extendedKeyUsage=serverAuth`.

The root is name-constrained (see *Isolation*, above), so any leaf must fall under
`localhost`, `.local`, `.star`, or a private IP range — a leaf outside those is
signed happily but fails verification with `permitted subtree violation`. Roots
generated before constraints existed keep working; `--force` replaces them, and the
script warns when it reuses an unconstrained one.

```sh
./stwp_server                                   # moon:// on 8090, star:// on 8490
./stwp_server --tls-port 8490 --cert certs/localhost.pem --key certs/localhost.key
./stwp_server --no-tls                          # plaintext only
```

If the certificate or key is missing, the server logs the reason, disables
`star://`, and continues serving `moon://`.

Inspect a running server's handshake directly with:

```sh
openssl s_client -connect localhost:8490 -alpn stwp/1.0 -CAfile certs/starweb_root.pem
```
