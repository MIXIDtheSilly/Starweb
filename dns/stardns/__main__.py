import argparse
import sys

from . import ca, config, db
from .panel import app
from .resolver import DNSServer


def main(argv=None) -> int:
    p = argparse.ArgumentParser(prog="stardns",
                                description="DNS server and control panel for .star")
    p.add_argument("--dns-port", type=int, default=config.DNS_PORT)
    p.add_argument("--dns-host", default=config.DNS_HOST)
    p.add_argument("--panel-port", type=int, default=config.PANEL_PORT)
    p.add_argument("--panel-tls-port", type=int, default=config.PANEL_TLS_PORT)
    p.add_argument("--panel-host", default=config.PANEL_HOST)
    p.add_argument("--scheme", choices=["moon", "star", "both"],
                   default=config.PANEL_SCHEME)
    p.add_argument("--cert", default=str(config.PANEL_CERT))
    p.add_argument("--key", default=str(config.PANEL_KEY))
    p.add_argument("--no-dns", action="store_true", help="panel only")
    p.add_argument("--no-panel", action="store_true", help="DNS only")
    p.add_argument("--log", action="store_true", help="one line per request/query")
    args = p.parse_args(argv)

    try:
        db.ping()
    except Exception as e:
        print(f"error: cannot reach MongoDB at {config.MONGO_URI}: {e}", file=sys.stderr)
        return 1
    print(f"[stardns] mongo {config.MONGO_URI}/{config.MONGO_DB}", flush=True)

    ok, why = ca.ca_ready()
    print(f"[stardns] CA: {why}" + ("" if ok else " — certificate issuing is off"),
          flush=True)

    dns = None
    if not args.no_dns:
        try:
            dns = DNSServer(args.dns_host, args.dns_port, log=args.log)
        except PermissionError:
            print(f"error: port {args.dns_port} needs root; try --dns-port 5354",
                  file=sys.stderr)
            return 1
        except OSError as e:
            print(f"error: cannot bind DNS on {args.dns_host}:{args.dns_port}: {e}",
                  file=sys.stderr)
            return 1

    if args.no_panel:
        if dns is None:
            print("error: --no-dns and --no-panel leaves nothing to run", file=sys.stderr)
            return 1
        dns.serve_forever()
        return 0

    if dns is not None:
        dns.start()

    cert = args.cert if args.scheme in ("star", "both") else None
    key = args.key if args.scheme in ("star", "both") else None
    try:
        app.run(host=args.panel_host, scheme=args.scheme, port=args.panel_port,
                tls_port=args.panel_tls_port, cert=cert, key=key, log=args.log)
    except (RuntimeError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    finally:
        if dns is not None:
            dns.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
