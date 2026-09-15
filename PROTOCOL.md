# STWP and the `moon://` / `star://` schemes

StarWeb speaks **STWP/1.0**, a request/response protocol structured like HTTP but
implemented from scratch. It is carried over two URL schemes:

| Scheme | Transport | Default port | Encrypted |
|--------|-----------|--------------|-----------|
| `moon://` | TCP | 8090 | no; plaintext on the wire |
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
Star-Theme: name=Nebula; scheme=dark; accent=#9461db; focus=#4937db
Connection: keep-alive

```

`Star-Theme` is the browser's current theme: its name, whether it is the light or
dark variant, the accent, and the colour the chrome outlines a focused control
with. A server that colours its own artwork can read it and serve the first paint
already in the right colours. It is advisory; a client with no theme sends no
header.

A response:

```
STWP/1.0 200 OK
Content-Length: 4142
Content-Type: text/html
Connection: keep-alive

<!DOCTYPE html>...
```

Header names are case-insensitive and lowercased on parse. Bodies are delimited by
`Content-Length`, which every response carries.

### Persistent connections

A request that says `Connection: keep-alive` asks the server to hold the
connection open for the next one; the response says `keep-alive` back when it
will. Keep-alive is opt-in on both sides, so a client that sends nothing, or
sends `Connection: close`, gets the original one-request-per-connection
behaviour and an older server is free to answer `close` to a client that asked
for more.

Reuse is only sound while both ends agree where a message ends, so the server
drops back to closing whenever it cannot promise that: a request it could not
parse, a version it does not speak, or a file whose bytes ran out mid-send. The
client parks a connection only after reading a body to its exact declared length.

Requests are never pipelined. The client sends one request per connection at a
time and reads its response in full before sending again; concurrency comes from
opening several connections, not from stacking requests on one.

Because a parked connection can be reaped by the far end at any moment, a client
must be ready for a reused connection to fail on the first write or read, and
retry once on a fresh one. The browser holds idle connections for 10 seconds and
the server for 20, so under normal conditions the client is the side that lets
go first.

## Range requests

File responses advertise `Accept-Ranges: bytes`, and a request may ask for a slice:

```
GET /video.mp4 STWP/1.0
Range: bytes=0-1048575
```

```
STWP/1.0 206 Partial Content
Content-Range: bytes 0-1048575/137404199
Content-Length: 1048576
```

Three forms are accepted: `bytes=a-b`, `bytes=a-` (to EOF), and `bytes=-n` (the
trailing *n* bytes, which is how a player finds an MP4's `moov` atom). An end past
EOF is clamped. A range that cannot be satisfied (a start past EOF, a malformed
spec, or a multi-range request, which is deliberately unsupported) is answered
`416 Range Not Satisfiable` with `Content-Range: bytes */<size>`; the server never
answers a range request with the wrong slice. A request with no `Range` header
still gets the whole file as `200`.

## The `star://` connection

Establishing a `star://` connection is two handshakes stacked:

1. **TCP**: the ordinary three-way handshake (SYN, SYN-ACK, ACK) from `connect()`.
2. **TLS 1.3**: a 1-RTT handshake on top of the established socket: ClientHello,
   ServerHello, certificate, Finished.

Only then does the first STWP byte go out, encrypted.

### TLS profile

- **TLS 1.3 only.** Both the client and the server pin minimum *and* maximum
  version to TLS 1.3. There is no negotiation down to 1.2, and no cipher
  downgrade, stricter than the web, which is affordable because `star://` has no
  legacy clients to support.
- **ALPN `stwp/1.0`.** The client offers it; the server selects it and fails the
  handshake if the client offers nothing it recognises.
- **SNI** is sent for DNS hostnames, and omitted for IP literals (per RFC 6066).

### Certificate verification

A `star://` server's certificate must satisfy all of:

- **Chain**: signed by a CA in the client's trust store. The trust anchor is the
  StarWeb root CA (`certs/starweb_root.pem`), overridable via the `STARWEB_CA`
  environment variable. The browser embeds that certificate at build time, so it
  needs no file at runtime; `STARWEB_CA` still replaces it. The system root store
  is not consulted.
- **Hostname**: the URL's host must match the certificate's SAN. DNS names are
  checked with `X509_VERIFY_PARAM_set1_host`, IP literals with
  `X509_VERIFY_PARAM_set1_ip_asc`. This is a separate check from the chain: a
  certificate legitimately signed by the StarWeb root but issued for a *different*
  host is rejected.
- **Validity period** and the usual X.509 constraints, enforced by OpenSSL.

Any failure aborts the handshake. The browser shows a full-page interstitial and
loads nothing; there is no click-through to proceed anyway.

### Session resumption

Keep-alive already spares most subresources a handshake, but a page still opens
several connections per origin, and a later navigation starts again from cold.
The client keeps an in-memory session cache keyed by `host:port`, so the first
connection to an origin handshakes in full and the rest resume:

```
[Server] [star/TLS full]    Request: GET /index.html
[Server] [kept alive]       Request: GET /style.css
[Server] [kept alive]       Request: GET /cat.jpg
[Server] [star/TLS resumed] Request: GET /photo.jpg
```

`kept alive` marks a request that arrived on a connection already open, so it
handshook not at all; `resumed` marks a *new* connection that skipped the full
handshake. The cache lives for the life of the process and is never written to
disk. 0-RTT
early data is deliberately **not** used: it is replay-unsafe, and these GETs are
cheap enough that it would buy little.

## Isolation from the public web

StarWeb is a separate web, and the boundary is enforced rather than assumed:

- **Requests must say `STWP/1.0`.** An HTTP request line parses fine here (the
  version is just a token), so the server checks it explicitly and answers anything
  else with `505 Version Not Supported`. A plain HTTP client gets no content.
- **Connections must negotiate ALPN `stwp/1.0`.** The server rejects a client that
  offers `h2`/`http/1.1`, *and* one that offers no ALPN at all; the select callback
  never fires in that case, so the check happens after the handshake instead. The
  client enforces the same in reverse. A real browser cannot complete a TLS
  handshake with a StarWeb server.
- **The root CA is name-constrained** to `localhost`, `.local`, `.web`, `.star`, and
  private/loopback IP ranges. It is structurally unable to issue for a public name,
  so installing this root cannot expose the public web even if the CA key leaks.
  Verification of a `www.google.com` leaf signed by it fails with
  `permitted subtree violation`.
- **Trust is mutual and closed.** StarWeb clients trust only the StarWeb root, not
  the system store; no public CA vouches for StarWeb hosts.
- **Pages cannot reach out.** `perform_fetch` accepts only `moon://` and `star://`,
  and page scripts cannot navigate to any other scheme.

- **Names are resolved by StarWeb's own DNS.** A host under `.web` is looked up
  by asking Nebula directly over UDP (`src/common/resolver.hpp`), never
  `getaddrinfo`, so the namespace is not ICANN's and a `.web` lookup is not
  leaked to a public resolver. There is deliberately no fallback: if Nebula
  cannot answer, the load fails rather than quietly asking the public DNS.
  Everything else (`localhost`, IP literals, any other name) still goes to
  the system resolver and behaves exactly as before.

  ```sh
  STARWEB_DNS=159.195.49.100:5354   # server to ask; "off" reverts to the system resolver
  STARWEB_DNS_ZONE=star        # the zone routed to it
  ```

  The server is `dns/` in this repo, which also registers the names and
  issues their certificates.

## Cookies

A server keeps a session by sending `Set-Cookie`; the browser then sends `Cookie`
on every later request to the same origin, navigations included. This is the only
mechanism that identifies a caller on a plain navigation, because nothing else a
page can reach adds a header to one.

```
STWP/1.0 200 OK
Set-Cookie: sid=nu4H2y...; Max-Age=604800; StwpOnly

```

```
GET /panel STWP/1.0
Cookie: sid=nu4H2y...; theme=dark

```

**Cookies are scoped to the origin** (scheme, host and port together). There is no
`Domain`, `Path` or `Secure` attribute, because the origin already says what all
three say in HTTP and says it more strictly: `star://a.web:443` and
`star://a.web:9000` share nothing, and neither does `moon://a.web:443`.

Two attributes remain:

| Attribute | Meaning |
| --- | --- |
| `Max-Age=<seconds>` | How long the cookie lives. Absent, it is a session cookie, dropped when the browser exits and never written to disk. `0` or negative deletes the cookie. |
| `StwpOnly` | The page's Lua cannot see the cookie. It is absent from `cookies`, and a script may not overwrite or delete it either, so it cannot be shadowed. |

STWP headers are a mapping, so a name cannot repeat the way HTTP repeats
`Set-Cookie` to set several at once. Several cookies ride in the one header joined
by ` | ` instead:

```
Set-Cookie: sid=abc; Max-Age=60; StwpOnly | theme=dark; Max-Age=31536000
```

`|` is rejected in a cookie name or value, as are `;`, `=`, `,`, whitespace and
control characters, which is what makes the split unambiguous. HTTP's `Expires`
has no equivalent here; it is exactly the attribute whose embedded comma made
folding `Set-Cookie` unsafe on the web.

**Cookies are sent same-origin only.** A navigation or subresource load carries
the cookies of the origin it is addressed to. A page script's `fetch` carries them
only when the target origin matches the page's own, like the web's
`credentials: "same-origin"` default. Since a cross-origin request never carries
credentials, CSRF has nothing to ride on, and there is no `SameSite` attribute to
get wrong.

Limits: 4096 bytes per value, 256 per name, 64 cookies per origin. A cookie past
any of those is dropped rather than truncated.

## Reverse proxies

`stwp_proxy` (see `PROXY.md`) is a separate hop at the connection level. It
negotiates keep-alive, TLS and `Connection` with each side independently. It also
writes each message out again from the headers it parsed, so a request whose length
it cannot be sure of (a `Content-Length` that is not a number, or two that disagree)
is rejected with 400 instead of being passed on.

It adds three request headers:

| Header | Value |
|--------|-------|
| `Star-Forwarded-For` | client address, appended to any existing value with `, ` (a header cannot repeat) |
| `Star-Forwarded-Proto` | `moon` or `star`, the scheme the client used |
| `Star-Forwarded-Host` | the `Host` the client sent |

Only the rightmost `Star-Forwarded-For` entry was written by the nearest proxy;
anything to its left came from the client.

Cookies need no rewriting. They are scoped to the origin, and the origin the browser
sees is the proxy's.

On `star://`, the TLS server name and `Host` must select the same site. A client that
handshakes for `a.web` and then asks for `b.web` on that connection gets **`421
Misdirected Request`**, and the connection closes. A certificate for one site
therefore cannot be used to reach another site behind the same proxy.

The proxy only forwards to `moon://` and `star://`, so the isolation rules above hold
through it.

## Security policy

Two rules apply to `star://` pages, both enforced in the browser:

- **Mixed content is blocked.** A `star://` page may not load `moon://`
  subresources: stylesheets, images, media, or scripts. A blocked load is logged
  and dropped; it never reaches the network.
- **Script-driven downgrades are blocked.** A page script (`location.assign`,
  `location.href = ...`) on a `star://` page cannot navigate to `moon://`. This is
  stricter than the web, where an `https` page may navigate to `http`. Typing a
  `moon://` URL by hand still works; the restriction is on pages, not on users.

## URLs

```
star://host[:port]/path
```

The port is elided from the canonical form when it is the scheme default (8490 for
`star`, 8090 for `moon`). The `Host` header follows the same rule. A URL typed
without a scheme is tried as `star://` first; the browser falls back to `moon://`
only when nothing accepts the connection on the `star://` port. A failed TLS
handshake or a bad certificate does not fall back, so a network attacker cannot
force the downgrade.

## Certificates for local development

`certs/` is generated locally and git-ignored; the root CA private key is never
committed.

```sh
./tools/make_certs.sh          # root CA + localhost leaf, reuses an existing root
./tools/make_certs.sh --force  # regenerate the root CA too
```

This produces a P-256 root CA (10 years) and a `localhost` leaf (825 days) with
`SAN = DNS:localhost, IP:127.0.0.1, IP:::1` and `extendedKeyUsage=serverAuth`.

The root is name-constrained (see *Isolation*, above), so any leaf must fall under
`localhost`, `.local`, `.web`, `.star`, or a private IP range; a leaf outside those is
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
