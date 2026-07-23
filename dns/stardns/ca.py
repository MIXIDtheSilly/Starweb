"""Leaf certificates for star:// hosts, signed by the StarWeb root CA.

The root is name-constrained to .star (see PROTOCOL.md), so it can only vouch
for names inside this network — which is exactly the set the panel hands out.
"""
import os
import secrets
import shutil
import subprocess
import tempfile
from datetime import datetime, timezone
from pathlib import Path

from . import config
from .db import db
from .errors import PanelError

EXTENSIONS = """basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName={sans}
subjectKeyIdentifier=hash
authorityKeyIdentifier=keyid,issuer
"""


def _openssl() -> str:
    path = shutil.which("openssl")
    if path is None:
        raise PanelError("openssl is not on PATH, so certificates cannot be "
                         "issued here.", 503)
    return path


def _run(args: list[str]) -> str:
    proc = subprocess.run([_openssl(), *args], capture_output=True, text=True)
    if proc.returncode != 0:
        detail = (proc.stderr or proc.stdout).strip().splitlines()
        raise PanelError(f"openssl {args[0]} failed: {detail[-1] if detail else '?'}", 500)
    return proc.stdout


def ca_ready() -> tuple[bool, str]:
    if not config.CA_CERT.is_file():
        return False, f"root CA certificate missing: {config.CA_CERT}"
    if not config.CA_KEY.is_file():
        return False, f"root CA key missing: {config.CA_KEY}"
    if shutil.which("openssl") is None:
        return False, "openssl is not on PATH"
    return True, "ready"


def _sans(domain: str, extra: list[str] | None = None) -> str:
    names = [f"DNS:{domain}", f"DNS:*.{domain}"]
    for name in extra or []:
        entry = f"DNS:{name}"
        if entry not in names:
            names.append(entry)
    return ",".join(names)


def issue(domain: str, extra_names: list[str] | None = None) -> dict:
    """Generate a keypair and a signed leaf for `domain`, plus *.domain.

    The private key is written to disk once and never stored in Mongo.
    """
    ok, why = ca_ready()
    if not ok:
        raise PanelError(f"Cannot issue: {why}.", 503)

    config.ISSUED.mkdir(parents=True, exist_ok=True)
    key_path = config.ISSUED / f"{domain}.key"
    cert_path = config.ISSUED / f"{domain}.pem"

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        csr = tmp / "leaf.csr"
        ext = tmp / "leaf.ext"
        key = tmp / "leaf.key"
        crt = tmp / "leaf.pem"

        _run(["ecparam", "-name", "prime256v1", "-genkey", "-noout",
              "-out", str(key)])
        _run(["req", "-new", "-key", str(key), "-sha256",
              "-subj", f"/O=StarWeb/CN={domain}", "-out", str(csr)])

        ext.write_text(EXTENSIONS.format(sans=_sans(domain, extra_names)))
        # A random serial rather than a shared .srl file: two panels signing at
        # once must not hand out the same serial.
        serial = secrets.randbits(127) | (1 << 126)
        _run(["x509", "-req", "-in", str(csr), "-CA", str(config.CA_CERT),
              "-CAkey", str(config.CA_KEY), "-set_serial", str(serial),
              "-days", str(config.CERT_DAYS), "-sha256",
              "-extfile", str(ext), "-out", str(crt)])
        _run(["verify", "-CAfile", str(config.CA_CERT), str(crt)])

        cert_pem = crt.read_text()
        key_pem = key.read_text()

    cert_path.write_text(cert_pem)
    key_path.write_text(key_pem)
    os.chmod(key_path, 0o600)

    not_after = _run(["x509", "-noout", "-enddate", "-in", str(cert_path)])
    not_after = not_after.split("=", 1)[1].strip() if "=" in not_after else ""

    doc = {
        "domain": domain,
        "serial": f"{serial:x}",
        "sans": _sans(domain, extra_names),
        "not_after": not_after,
        "issued_at": datetime.now(timezone.utc),
        "cert_pem": cert_pem,
        "cert_path": str(cert_path),
        "key_path": str(key_path),
    }
    db().certs.insert_one(doc)
    return doc


def list_certs(domain: str) -> list[dict]:
    return list(db().certs.find({"domain": domain}).sort("issued_at", -1))


def latest(domain: str) -> dict | None:
    return db().certs.find_one({"domain": domain}, sort=[("issued_at", -1)])


def read_material(domain: str, what: str) -> tuple[str, str]:
    """('cert'|'key') -> (filename, PEM). The key only exists on disk."""
    cert = latest(domain)
    if cert is None:
        raise PanelError("No certificate has been issued for this domain.", 404)
    if what == "cert":
        return f"{domain}.pem", cert["cert_pem"]
    if what == "key":
        path = Path(cert.get("key_path", ""))
        if not path.is_file():
            raise PanelError("The private key is no longer on this server. "
                             "Issue a new certificate to get a fresh one.", 404)
        return f"{domain}.key", path.read_text()
    raise PanelError("Ask for 'cert' or 'key'.", 400)
