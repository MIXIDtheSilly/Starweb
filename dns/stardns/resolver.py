"""Authoritative DNS server for the .star zone, answering out of MongoDB."""
import socket
import struct
import threading
from concurrent.futures import ThreadPoolExecutor

from . import config, wire
from .db import db
from .wire import RR

MAX_CNAME_HOPS = 8

# Static for the TLD itself: it is not a registerable domain, it is the registry.
TLD_SOA_SERIAL = 1


def _zone() -> str:
    return config.ZONE


def _soa(name: str, serial: int) -> RR:
    return RR(name, wire.SOA, 3600,
              (config.NS_NAME, f"hostmaster.{name}", serial, 3600, 600, 604800, 60))


def _split_domain(qname: str) -> str | None:
    """The registerable name under the zone, e.g. a.b.example.star -> example.star."""
    zone = _zone()
    if qname == zone or not qname.endswith("." + zone):
        return None
    parts = qname.split(".")
    if len(parts) < 2:
        return None
    return ".".join(parts[-2:])


def _relative(qname: str, domain: str) -> str:
    return "@" if qname == domain else qname[: -(len(domain) + 1)]


def _wildcards(rel: str) -> list[str]:
    """Names to try after an exact miss, closest match first."""
    if rel == "@":
        return ["*"]
    labels = rel.split(".")
    return [".".join(["*"] + labels[i:]) for i in range(1, len(labels) + 1)]


def _records(domain: str, rel: str) -> list[dict]:
    found = list(db().records.find({"domain": domain, "name": rel}))
    if found:
        return found
    for candidate in _wildcards(rel):
        found = list(db().records.find({"domain": domain, "name": candidate}))
        if found:
            return found
    return []


def _has_children(domain: str, rel: str) -> bool:
    """True when rel is an empty non-terminal — no records of its own, but
    something lives beneath it, so the name exists and the answer is NODATA."""
    if rel == "@":
        return True
    return db().records.count_documents(
        {"domain": domain, "name": {"$regex": r"\." + rel.replace(".", r"\.") + "$"}}
    ) > 0


def _to_rr(rec: dict, qname: str, domain: str) -> RR:
    rtype = wire.TYPE_CODES[rec["type"]]
    value = rec["value"]
    if rtype == wire.CNAME:
        # A relative target is relative to the domain, like a zone file.
        if not value.endswith("." + _zone()) and value != _zone():
            value = f"{value}.{domain}"
    return RR(qname, rtype, rec.get("ttl", config.DEFAULT_TTL), value)


def _apex_extras(domain: str, serial: int, qtype: int) -> list[RR]:
    out = []
    if qtype in (wire.NS, wire.ANY):
        out.append(RR(domain, wire.NS, 3600, config.NS_NAME))
    if qtype in (wire.SOA, wire.ANY):
        out.append(_soa(domain, serial))
    return out


def answer(query: wire.Query) -> bytes:
    qname = query.name.rstrip(".").lower()
    qtype = query.qtype
    zone = _zone()

    if query.opcode != 0:
        return wire.build_response(query, rcode=wire.NOTIMP, aa=False,
                                   max_size=query.udp_size)
    if query.qclass not in (wire.IN, wire.ANY):
        return wire.build_response(query, rcode=wire.REFUSED, aa=False,
                                   max_size=query.udp_size)

    # Names outside .star are somebody else's problem; we are not a recursor.
    if qname != zone and not qname.endswith("." + zone):
        return wire.build_response(query, rcode=wire.REFUSED, aa=False,
                                   max_size=query.udp_size)

    if qname == zone:
        answers = []
        if qtype in (wire.NS, wire.ANY):
            answers.append(RR(zone, wire.NS, 3600, config.NS_NAME))
        if qtype in (wire.SOA, wire.ANY):
            answers.append(_soa(zone, TLD_SOA_SERIAL))
        authority = [] if answers else [_soa(zone, TLD_SOA_SERIAL)]
        return wire.build_response(query, answers, authority,
                                   additional=_glue(qtype, answers),
                                   max_size=query.udp_size)

    domain_name = _split_domain(qname)
    domain = db().domains.find_one({"name": domain_name}) if domain_name else None
    if domain is None:
        return wire.build_response(query, authority=[_soa(zone, TLD_SOA_SERIAL)],
                                   rcode=wire.NXDOMAIN, max_size=query.udp_size)

    serial = domain.get("serial", TLD_SOA_SERIAL)
    answers, rcode = _resolve(domain["name"], qname, qtype, serial)
    authority = [] if answers else [_soa(domain["name"], serial)]
    return wire.build_response(query, answers, authority,
                               additional=_glue(qtype, answers),
                               rcode=rcode, max_size=query.udp_size)


def _glue(qtype: int, answers: list[RR]) -> list[RR]:
    """Address for our own NS name, so a resolver isn't sent chasing it."""
    if not any(rr.rtype == wire.NS for rr in answers):
        return []
    try:
        import ipaddress
        addr = ipaddress.ip_address(config.NS_ADDR)
    except ValueError:
        return []
    rtype = wire.AAAA if addr.version == 6 else wire.A
    return [RR(config.NS_NAME, rtype, 3600, str(addr))]


def _resolve(domain: str, qname: str, qtype: int, serial: int) -> tuple[list[RR], int]:
    answers: list[RR] = []
    name = qname
    for _ in range(MAX_CNAME_HOPS):
        rel = _relative(name, domain)
        found = _records(domain, rel)

        cname = next((r for r in found if r["type"] == "CNAME"), None)
        if cname is not None and qtype != wire.CNAME:
            rr = _to_rr(cname, name, domain)
            answers.append(rr)
            target = str(rr.data)
            # Only chase inside this domain; anything else the client resolves.
            if target == domain or target.endswith("." + domain):
                name = target
                continue
            return answers, wire.NOERROR

        matched = [_to_rr(r, name, domain) for r in found
                   if qtype in (wire.ANY, wire.TYPE_CODES[r["type"]])]
        if rel == "@":
            matched += _apex_extras(domain, serial, qtype)

        if matched:
            return answers + matched, wire.NOERROR

        if found or _has_children(domain, rel) or rel == "@":
            return answers, wire.NOERROR  # NODATA: name exists, type does not
        # A CNAME already in the answer means the name did exist.
        return answers, wire.NOERROR if answers else wire.NXDOMAIN

    return answers, wire.NOERROR


class DNSServer:
    def __init__(self, host: str | None = None, port: int | None = None,
                 log: bool = False, workers: int = 16):
        self.host = host or config.DNS_HOST
        self.port = config.DNS_PORT if port is None else port
        self.log = log
        self._stop = threading.Event()
        self._pool = ThreadPoolExecutor(max_workers=workers)

        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.udp.bind((self.host, self.port))

        self.tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.tcp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.tcp.bind((self.host, self.port))
        self.tcp.listen(32)

    def _emit(self, msg: str) -> None:
        if self.log:
            print(f"[DNS] {msg}", flush=True)

    def _reply(self, data: bytes, over_tcp: bool) -> bytes:
        try:
            query = wire.parse_query(data)
        except (wire.FormatError, struct.error):
            return wire.error_response(data, wire.FORMERR)
        if over_tcp:
            query.udp_size = 65535
        try:
            out = answer(query)
        except Exception as e:
            self._emit(f"SERVFAIL {query.name}: {e}")
            return wire.build_response(query, rcode=wire.SERVFAIL, aa=False)
        self._emit(f"{'TCP' if over_tcp else 'UDP'} "
                   f"{wire.TYPE_NAMES.get(query.qtype, query.qtype)} {query.name} "
                   f"-> rcode {out[3] & 0xF}")
        return out

    def start(self) -> None:
        threading.Thread(target=self._udp_loop, daemon=True).start()
        threading.Thread(target=self._tcp_loop, daemon=True).start()
        print(f"[DNS] .{_zone()} on {self.host}:{self.port} (udp+tcp)", flush=True)

    def serve_forever(self) -> None:
        self.start()
        try:
            while not self._stop.wait(0.5):
                pass
        except KeyboardInterrupt:
            pass
        finally:
            self.stop()

    def stop(self) -> None:
        self._stop.set()
        for sock in (self.udp, self.tcp):
            try:
                sock.close()
            except OSError:
                pass
        self._pool.shutdown(wait=False)

    def _udp_loop(self) -> None:
        while not self._stop.is_set():
            try:
                data, addr = self.udp.recvfrom(4096)
            except OSError:
                return
            self._pool.submit(self._udp_one, data, addr)

    def _udp_one(self, data: bytes, addr) -> None:
        out = self._reply(data, over_tcp=False)
        if out:
            try:
                self.udp.sendto(out, addr)
            except OSError:
                pass

    def _tcp_loop(self) -> None:
        while not self._stop.is_set():
            try:
                conn, _ = self.tcp.accept()
            except OSError:
                return
            self._pool.submit(self._tcp_one, conn)

    def _tcp_one(self, conn: socket.socket) -> None:
        # TCP frames each message with a two-byte length prefix.
        try:
            conn.settimeout(5.0)
            head = _recv_exactly(conn, 2)
            if head is None:
                return
            length = struct.unpack("!H", head)[0]
            data = _recv_exactly(conn, length)
            if data is None:
                return
            out = self._reply(data, over_tcp=True)
            if out:
                conn.sendall(struct.pack("!H", len(out)) + out)
        except OSError:
            pass
        finally:
            try:
                conn.close()
            except OSError:
                pass


def _recv_exactly(conn: socket.socket, n: int) -> bytes | None:
    buf = b""
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf
