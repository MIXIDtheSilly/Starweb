import ipaddress
import re
from datetime import datetime, timezone

from . import config
from .db import db
from .errors import PanelError

TYPES = ("A", "AAAA", "CNAME", "TXT")

LABEL_RE = re.compile(r"^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?$")
RESERVED = {"registry", "panel", "ns", "ns1", "ns2", "dns", "root", "www",
            "localhost", "web", "star", "moon", "admin"}

MAX_TXT = 2048


def _now():
    return datetime.now(timezone.utc)


def _serial() -> int:
    return int(_now().timestamp())


def normalize_domain(name: str) -> str:
    """'Mysite' and 'mysite.star' both mean mysite.star."""
    name = (name or "").strip().lower().rstrip(".")
    suffix = "." + config.ZONE
    if name.endswith(suffix):
        name = name[: -len(suffix)]
    if not name:
        raise PanelError("Enter a domain name.")
    if "." in name:
        raise PanelError(f"Register a single label under .{config.ZONE}, "
                         f"e.g. mysite.{config.ZONE} — subdomains are records.")
    if not LABEL_RE.match(name):
        raise PanelError("Domain may use a-z, 0-9 and dashes, and cannot start "
                         "or end with a dash.")
    if name in RESERVED:
        raise PanelError(f"'{name}' is reserved by the registry.")
    return f"{name}.{config.ZONE}"


def normalize_record_name(name: str, domain: str) -> str:
    """Returns '@' for the apex, otherwise the relative label path."""
    name = (name or "").strip().lower().rstrip(".")
    if name in ("", "@", domain):
        return "@"
    if name.endswith("." + domain):
        name = name[: -(len(domain) + 1)]
    for label in name.split("."):
        if label == "*":
            continue  # a wildcard is only meaningful as the leftmost label
        if not LABEL_RE.match(label):
            raise PanelError(f"Bad name label: {label!r}")
    if name.count("*") and not name.startswith("*"):
        raise PanelError("A wildcard is only allowed as the leftmost label.")
    if len(name) + len(domain) + 1 > 253:
        raise PanelError("Name is too long.")
    return name


def fqdn(record_name: str, domain: str) -> str:
    return domain if record_name == "@" else f"{record_name}.{domain}"


def _valid_hostname(value: str) -> str:
    value = (value or "").strip().lower().rstrip(".")
    if not value or len(value) > 253:
        raise PanelError("Target must be a hostname.")
    for label in value.split("."):
        if not LABEL_RE.match(label):
            raise PanelError(f"Bad hostname label: {label!r}")
    return value


def validate_value(rtype: str, value: str) -> str:
    value = (value or "").strip()
    if rtype == "A":
        try:
            return str(ipaddress.IPv4Address(value))
        except ValueError:
            raise PanelError("An A record needs an IPv4 address, e.g. 127.0.0.1.") from None
    if rtype == "AAAA":
        try:
            return str(ipaddress.IPv6Address(value))
        except ValueError:
            raise PanelError("An AAAA record needs an IPv6 address, e.g. ::1.") from None
    if rtype == "CNAME":
        return _valid_hostname(value)
    if rtype == "TXT":
        if not value:
            raise PanelError("A TXT record needs some text.")
        if len(value.encode()) > MAX_TXT:
            raise PanelError(f"TXT value is over {MAX_TXT} bytes.")
        return value
    raise PanelError(f"Unsupported record type: {rtype!r}")


def validate_ttl(ttl) -> int:
    try:
        ttl = int(ttl)
    except (TypeError, ValueError):
        raise PanelError("TTL must be a number of seconds.") from None
    if not 60 <= ttl <= 86400:
        raise PanelError("TTL must be between 60 and 86400 seconds.")
    return ttl


def list_domains(username: str) -> list[dict]:
    return list(db().domains.find({"owner": username}).sort("name"))


def get_domain(username: str, name: str) -> dict:
    domain = db().domains.find_one({"name": normalize_domain(name)})
    if domain is None:
        raise PanelError("No such domain.", 404)
    if domain["owner"] != username:
        # Same answer as a missing domain: don't leak who owns what.
        raise PanelError("No such domain.", 404)
    return domain


def add_domain(username: str, name: str) -> dict:
    name = normalize_domain(name)
    owned = db().domains.count_documents({"owner": username})
    if owned >= config.MAX_DOMAINS:
        raise PanelError(f"You already have {config.MAX_DOMAINS} domains, "
                         "the limit per account. Delete one first.", 403)
    if db().domains.find_one({"name": name}):
        raise PanelError(f"{name} is already registered.", 409)

    doc = {"name": name, "owner": username, "created_at": _now(),
           "serial": _serial()}
    try:
        db().domains.insert_one(doc)
    except Exception:
        raise PanelError(f"{name} is already registered.", 409) from None
    return doc


def delete_domain(username: str, name: str) -> None:
    domain = get_domain(username, name)
    db().records.delete_many({"domain": domain["name"]})
    db().certs.delete_many({"domain": domain["name"]})
    db().domains.delete_one({"_id": domain["_id"]})


def list_records(domain: str) -> list[dict]:
    return list(db().records.find({"domain": domain}).sort([("name", 1), ("type", 1)]))


def add_record(username: str, domain_name: str, name: str, rtype: str,
               value: str, ttl=None) -> dict:
    domain = get_domain(username, domain_name)
    dn = domain["name"]

    rtype = (rtype or "").strip().upper()
    if rtype not in TYPES:
        raise PanelError(f"Type must be one of {', '.join(TYPES)}.")

    name = normalize_record_name(name, dn)
    if rtype == "CNAME" and (value or "").strip() == "@":
        value = dn  # '@' as a target means the domain itself, as in a zone file
    value = validate_value(rtype, value)
    ttl = validate_ttl(config.DEFAULT_TTL if ttl in (None, "") else ttl)

    if db().records.count_documents({"domain": dn}) >= config.MAX_RECORDS:
        raise PanelError(f"{dn} is at the {config.MAX_RECORDS}-record limit.", 403)

    # A CNAME owns its name outright — nothing else may share it, and the apex
    # is already occupied by the zone's own SOA and NS.
    siblings = list(db().records.find({"domain": dn, "name": name}))
    if rtype == "CNAME":
        if name == "@":
            raise PanelError("The apex cannot be a CNAME; use an A or AAAA record.")
        if siblings:
            raise PanelError(f"'{fqdn(name, dn)}' already has records, so it "
                             "cannot also be a CNAME.")
    elif any(s["type"] == "CNAME" for s in siblings):
        raise PanelError(f"'{fqdn(name, dn)}' is a CNAME, so it cannot have "
                         "other records.")

    if any(s["type"] == rtype and s["value"] == value for s in siblings):
        raise PanelError("That exact record already exists.", 409)

    doc = {"domain": dn, "name": name, "type": rtype, "value": value,
           "ttl": ttl, "created_at": _now()}
    db().records.insert_one(doc)
    _touch(dn)
    return doc


def delete_record(username: str, domain_name: str, record_id: str) -> None:
    from bson import ObjectId
    from bson.errors import InvalidId

    domain = get_domain(username, domain_name)
    try:
        oid = ObjectId(record_id)
    except (InvalidId, TypeError):
        raise PanelError("Bad record id.", 404) from None
    res = db().records.delete_one({"_id": oid, "domain": domain["name"]})
    if res.deleted_count == 0:
        raise PanelError("No such record.", 404)
    _touch(domain["name"])


def _touch(domain: str) -> None:
    """Bump the zone serial so a caching resolver sees the change."""
    db().domains.update_one({"name": domain}, {"$set": {"serial": _serial()}})
