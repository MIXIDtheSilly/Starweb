import time
from pathlib import Path

from starweb import App, Response

# Inside a checkout this resolves to the repo root, which carries certs/ and
# www/. Installed from PyPI it points somewhere arbitrary, so every use below
# is guarded on the directory actually being there.
REPO = Path(__file__).resolve().parents[2]
CERTS = REPO / "certs"
WWW = REPO / "www"


def _cert_default(name: str) -> str | None:
    path = CERTS / name
    return str(path) if path.is_file() else None


app = App()


@app.route("/api/time")
def now(req):
    return {"unix": time.time()}


@app.route("/api/echo", methods=["POST"])
def echo(req):
    return Response(200, body=req.body,
                    headers={"Content-Type": req.headers.get("content-type", "text/plain")})


@app.route("/api/greet/<name>")
def greet(req, name):
    return {"hello": name, "loud": req.query.get("loud") == "1"}


# Built once at import so a request costs a slice, not a fill: the speed test
# wants to measure the socket, not how fast Python can make bytes. Varied enough
# that a compressing transport can't trivially collapse it.
_BLOB = (bytes(range(256)) * 4096)          # 1 MiB
_BLOB = _BLOB * 8                           # 8 MiB, the fetch response ceiling


@app.route("/api/blob/<size>")
def blob(req, size):
    try:
        n = int(size)
    except ValueError:
        return Response(400, body=b"size must be an integer")
    n = max(0, min(n, len(_BLOB)))
    return Response(200, body=_BLOB[:n],
                    headers={"Content-Type": "application/octet-stream"})


# Readable from a page on any origin; without cors= a cross-origin fetch is refused.
@app.route("/api/public", cors="*")
def public(req):
    return {"open": True, "origin": req.headers.get("origin", "")}


# Routes win over static, so /api/* still reaches the handlers above.
if WWW.is_dir():
    app.mount_static("/", str(WWW))


if __name__ == "__main__":
    import argparse

    p = argparse.ArgumentParser()
    p.add_argument("--scheme", choices=["moon", "star", "both"], default="moon")
    p.add_argument("--port", type=int, default=8090)
    p.add_argument("--tls-port", type=int, default=8490)
    p.add_argument("--cert", default=_cert_default("localhost.pem"))
    p.add_argument("--key", default=_cert_default("localhost.key"))
    p.add_argument("--log", action="store_true", help="one line per request")
    args = p.parse_args()

    if args.scheme in ("star", "both") and not (args.cert and args.key):
        raise SystemExit(
            f"error: {args.scheme!r} needs TLS certs; none found under {CERTS}.\n"
            "Pass --cert/--key, or use --scheme moon for plaintext."
        )

    try:
        app.run(scheme=args.scheme, port=args.port, tls_port=args.tls_port,
                cert=args.cert, key=args.key, log=args.log)
    except (RuntimeError, OSError) as e:
        raise SystemExit(f"error: {e}")
