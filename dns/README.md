# StarDNS

Names for StarWeb. An authoritative DNS server for the `.web` zone, a
Cloudflare-shaped control panel to manage them, and a certificate authority that
issues `star://` leaves off the StarWeb root CA.

Three parts, one process:

| Part | What it is |
|------|------------|
| `stardns/resolver.py` | Authoritative DNS over UDP and TCP, answering out of MongoDB |
| `stardns/panel.py` | STWP site — accounts, domains, records, certificates |
| `stardns/ca.py` | Leaf certificates signed by `certs/starweb_root.pem` |

The panel is served with the `starweb` package, so it speaks `moon://` and
`star://` and is browsed with `stwp_browser` like any other StarWeb page.

Everything DNS lives under this directory:

```
dns/
├── run.py            start it from anywhere
├── requirements.txt
├── issued/           certificates the panel has signed (gitignored)
└── stardns/          the package
    ├── config.py     every path and port, all env-overridable
    ├── wire.py       DNS message codec
    ├── resolver.py   the server
    ├── zones.py      domains and records, and the rules they follow
    ├── auth.py       accounts and sessions
    ├── ca.py         certificate issuance
    ├── panel.py      routes
    ├── ui.py         HTML
    └── tests/
```

The one thing it reaches outside for is `certs/` at the checkout root — the CA
it signs with, shared with the C++ side, which reads the same root.

## Running it

```sh
pip install -r dns/requirements.txt
brew services start mongodb/brew/mongodb-community@7.0
./tools/make_certs.sh                      # once, if certs/ is empty

python3 dns/run.py --log
```

That brings up DNS on `0.0.0.0:5354` and the panel on `moon://…:8091` and
`star://…:8491`. Open `moon://localhost:8091/` in the browser and register.

Port 53 needs root, so 5354 is the default:

```sh
sudo python3 dns/run.py --dns-port 53
```

### Installing MongoDB on this machine

7.0, not the current 8.x: this is macOS 12, and the 8.x binaries need macOS 13.
And `--ignore-dependencies`, because `mongosh` is only a *recommended*
dependency but drags in `node`, which has no bottle on macOS 12 — Homebrew
starts compiling LLVM from source to build it. The server tarball needs none of
that, and pymongo is the client here anyway.

```sh
brew trust mongodb/brew
brew install --ignore-dependencies mongodb-community@7.0
brew services start mongodb/brew/mongodb-community@7.0
```

`brew services stop mongodb/brew/mongodb-community@7.0` stops it and unloads the
login agent.

Everything is overridable by flag or environment variable — see `config.py` for
the full list. `--no-dns` and `--no-panel` run one half on its own, which is how
you'd put the resolver on one box and the panel on another against the same
MongoDB.

## Using the names

`stwp_browser` and `stwp_client` resolve `.web` by asking this server
themselves — no system configuration, no `sudo`. They default to
`127.0.0.1:5354`; point them elsewhere with `STARWEB_DNS`:

```sh
STARWEB_DNS=192.168.1.20:5354 ./stwp_browser     # another machine's registry
STARWEB_DNS=off ./stwp_browser                   # back to the system resolver
```

Everything that isn't `.web` still goes to the system resolver, so
`localhost`, IP literals and the rest are untouched. Answers are cached in
process for the record's TTL, so a page's subresources cost one lookup between
them.

Other programs on the machine — `curl`, `ping`, Python — still know nothing
about `.web`. If you want them to, that is the resolver file, and it is the
only thing needing root:

```sh
sudo mkdir -p /etc/resolver
printf 'nameserver 127.0.0.1\nport 5354\n' | sudo tee /etc/resolver/star
```

## The panel

Sign in with a username and password. An account may hold **three domains**,
each a single label under `.web` — `mysite.web`, not `a.b.web`; deeper names
are records inside a domain you own.

Records are `A`, `AAAA`, `CNAME` and `TXT`, with a TTL between 60 and 86400
seconds. `@` is the domain itself, both as a name and as a CNAME target, and a
CNAME target with no dot in it is read as relative to the domain. `*.dev` is a
wildcard. The rules a real resolver relies on are enforced on the way in: a
CNAME may not share a name with any other record, and may not sit at the apex.

Deleting a domain takes its records and certificates with it, and frees the
slot.

### API

Every page action is a JSON route, so the panel is scriptable:

```sh
starweb post moon://localhost:8091/api/login \
  --json '{"username": "you", "password": "…"}'
```

| Route | Body |
|-------|------|
| `POST /api/register`, `/api/login` | `username`, `password` → `token` |
| `POST /api/logout` | `token` |
| `POST /api/domains` | `token` |
| `POST /api/domain/add`, `/api/domain/delete` | `token`, `domain` |
| `POST /api/records` | `token`, `domain` |
| `POST /api/record/add` | `token`, `domain`, `name`, `type`, `value`, `ttl` |
| `POST /api/record/delete` | `token`, `domain`, `id` |
| `POST /api/cert/issue` | `token`, `domain` |
| `GET /cert/<domain>/cert\|key?t=` | PEM download |

Errors come back as `{"error": "..."}` with a real status code.

## Certificates

"Issue certificate" generates a P-256 key and a leaf covering `mysite.web` and
`*.mysite.web`, signed by the StarWeb root, valid 825 days. The certificate is
stored in MongoDB and written with the key to `dns/issued/mysite.web.{pem,key}`;
the key is never stored in the database, and re-issuing replaces it.

Serve with it exactly like the localhost pair:

```sh
./stwp_server --tls-port 8490 \
    --cert dns/issued/mysite.web.pem --key dns/issued/mysite.web.key
```

Any StarWeb client trusting the root — which is all of them — accepts it. The
root is name-constrained to `.web` (and `.star`, for names issued before the
rename), so the panel structurally cannot issue for
a public name; `ca.issue("www.google.com")` fails its own verify step, and there
is a test that holds it to that.

The CA key has to be readable by the panel process. If `certs/starweb_root.key`
is missing the panel still runs, and says so on the domain page instead of
offering a button that cannot work.

## Notes on the design

**The session token is in the URL.** The renderer has no cookies and no local
storage, and `display:none` is not honoured, so the panel cannot be one page
with hidden views holding a token in memory across actions — it is server-
rendered pages, and the token rides in `?t=`. That means it lands in history and
in the omnibox. Fine for a private network, wrong for a public one; cookies in
STWP would be the fix.

**Passwords** are scrypt (n=2^14, r=8, p=1) with a 16-byte salt. A login for an
unknown user still pays the hash so the miss cannot be timed. Session tokens are
32 random bytes, stored as a SHA-256 hash with a TTL index, so a database dump
does not hand over live sessions.

**Someone else's domain is 404, not 403** — the panel does not disclose that a
name is registered, or by whom, to a signed-in stranger.

## Tests

```sh
python3 -m pytest dns -q
```

The suite runs against an in-memory stand-in for MongoDB (`stardns/tests/fakemongo.py`),
so no mongod is needed. It covers the wire codec byte for byte, resolver
behaviour (CNAME chasing, wildcards, NODATA vs NXDOMAIN, REFUSED off-zone,
truncation), the record rules, the account and domain limits, and the panel
routes. `test_ca.py` issues a real certificate and completes a real TLS 1.3
handshake against it with a client that trusts only the StarWeb root.
