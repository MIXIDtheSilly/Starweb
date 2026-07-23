import os
from pathlib import Path

# dns/stardns/config.py -> HOME is dns/, REPO is the checkout root. The CA lives
# at the root because the C++ side reads it too; everything we produce stays
# under HOME.
HOME = Path(__file__).resolve().parents[1]
REPO = Path(__file__).resolve().parents[2]


def _int(name: str, default: int) -> int:
    try:
        return int(os.environ.get(name, default))
    except ValueError:
        return default


MONGO_URI = os.environ.get("STARDNS_MONGO", "mongodb://127.0.0.1:27017")
MONGO_DB = os.environ.get("STARDNS_DB", "stardns")

# The only TLD this server is authoritative for. Anything else is REFUSED, and
# the root CA's name constraints make it the only one we could issue certs for.
ZONE = os.environ.get("STARDNS_ZONE", "web").strip(".").lower()

DNS_HOST = os.environ.get("STARDNS_DNS_HOST", "0.0.0.0")
# 53 needs root; 5354 is the default so a normal user can just run it.
DNS_PORT = _int("STARDNS_DNS_PORT", 5354)
NS_NAME = os.environ.get("STARDNS_NS", f"ns1.registry.{ZONE}")
NS_ADDR = os.environ.get("STARDNS_NS_ADDR", "127.0.0.1")

PANEL_HOST = os.environ.get("STARDNS_PANEL_HOST", "0.0.0.0")
PANEL_PORT = _int("STARDNS_PANEL_PORT", 8091)
PANEL_TLS_PORT = _int("STARDNS_PANEL_TLS_PORT", 8491)
PANEL_SCHEME = os.environ.get("STARDNS_PANEL_SCHEME", "both")

CERTS = Path(os.environ.get("STARDNS_CERTS", REPO / "certs"))
CA_CERT = Path(os.environ.get("STARDNS_CA_CERT", CERTS / "starweb_root.pem"))
CA_KEY = Path(os.environ.get("STARDNS_CA_KEY", CERTS / "starweb_root.key"))
PANEL_CERT = Path(os.environ.get("STARDNS_PANEL_CERT", CERTS / "localhost.pem"))
PANEL_KEY = Path(os.environ.get("STARDNS_PANEL_KEY", CERTS / "localhost.key"))

# Where issued leaf certs and keys land. The key is written once, here, and
# never stored in Mongo.
ISSUED = Path(os.environ.get("STARDNS_ISSUED", HOME / "issued"))

MAX_DOMAINS = _int("STARDNS_MAX_DOMAINS", 3)
MAX_RECORDS = _int("STARDNS_MAX_RECORDS", 50)
SESSION_DAYS = _int("STARDNS_SESSION_DAYS", 7)
DEFAULT_TTL = _int("STARDNS_TTL", 300)
CERT_DAYS = _int("STARDNS_CERT_DAYS", 825)
