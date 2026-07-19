import time
from pathlib import Path

from starweb import App, Response

REPO = Path(__file__).resolve().parents[2]
CERTS = REPO / "certs"

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


# Routes win over static, so /api/* still reaches the handlers above.
app.mount_static("/", str(REPO / "www"))


if __name__ == "__main__":
    import argparse

    p = argparse.ArgumentParser()
    p.add_argument("--scheme", choices=["moon", "star", "both"], default="moon")
    p.add_argument("--port", type=int, default=8090)
    p.add_argument("--tls-port", type=int, default=8490)
    p.add_argument("--cert", default=str(CERTS / "localhost.pem"))
    p.add_argument("--key", default=str(CERTS / "localhost.key"))
    p.add_argument("--log", action="store_true", help="one line per request")
    args = p.parse_args()

    try:
        app.run(scheme=args.scheme, port=args.port, tls_port=args.tls_port,
                cert=args.cert, key=args.key, log=args.log)
    except (RuntimeError, OSError) as e:
        raise SystemExit(f"error: {e}")
