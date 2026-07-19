import argparse
import runpy
import sys

from . import Session, StarWebError
from .server import App


def _get(args) -> int:
    try:
        with Session(cafile=args.ca, timeout=args.timeout) as s:
            res = s.request(args.method, args.url)
    except StarWebError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    if args.verbose:
        print(f"{res.version} {res.status_code} {res.status_text}",
              file=sys.stderr)
        for name, value in res.headers.items():
            print(f"{name}: {value}", file=sys.stderr)
        if res.tls:
            print(f"[tls] {res.tls.version} {res.tls.cipher} "
                  f"alpn={res.tls.alpn} resumed={res.tls.resumed}",
                  file=sys.stderr)
        print(file=sys.stderr)

    sys.stdout.buffer.write(res.body)
    return 0 if res.ok else 1


def _serve(args) -> int:
    module = runpy.run_path(args.app)
    app = next((v for v in module.values() if isinstance(v, App)), None)
    if app is None:
        print(f"error: no App instance found in {args.app}", file=sys.stderr)
        return 1
    scheme = "moon" if args.no_tls else args.scheme
    try:
        app.run(host=args.host, scheme=scheme, port=args.port,
                tls_port=args.tls_port, cert=args.cert, key=args.key,
                log=args.log)
    except RuntimeError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="starweb")
    sub = parser.add_subparsers(dest="command", required=True)

    g = sub.add_parser("get", help="fetch a moon:// or star:// URL")
    g.add_argument("url")
    g.add_argument("-X", "--method", default="GET")
    g.add_argument("-v", "--verbose", action="store_true")
    g.add_argument("--ca", default=None)
    g.add_argument("--timeout", type=float, default=10.0)
    g.set_defaults(func=_get)

    s = sub.add_parser("serve", help="run a Python file defining an App")
    s.add_argument("app")
    s.add_argument("--scheme", choices=["moon", "star", "both"], default="both",
                   help="which scheme to host (default: both)")
    s.add_argument("--no-tls", action="store_true",
                   help="plaintext moon:// only, same as --scheme moon")
    s.add_argument("--log", action="store_true",
                   help="print one line per request")
    s.add_argument("--host", default="0.0.0.0")
    s.add_argument("--port", type=int, default=8091)
    s.add_argument("--tls-port", type=int, default=8491)
    s.add_argument("--cert", default=None)
    s.add_argument("--key", default=None)
    s.set_defaults(func=_serve)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
