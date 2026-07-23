import hashlib
import hmac
import re
import secrets
from datetime import datetime, timedelta, timezone

from . import config
from .db import db
from .errors import PanelError

USERNAME_RE = re.compile(r"^[a-z0-9][a-z0-9_.-]{2,31}$")
_SCRYPT = dict(n=1 << 14, r=8, p=1, dklen=32)


def _now():
    return datetime.now(timezone.utc)


def _hash_password(password: str, salt: bytes) -> bytes:
    return hashlib.scrypt(password.encode(), salt=salt, **_SCRYPT)


def _token_hash(token: str) -> str:
    return hashlib.sha256(token.encode()).hexdigest()


def register(username: str, password: str) -> str:
    username = (username or "").strip().lower()
    if not USERNAME_RE.match(username):
        raise PanelError("Username must be 3-32 chars: a-z, 0-9, dot, dash, underscore.")
    if len(password or "") < 8:
        raise PanelError("Password must be at least 8 characters.")

    salt = secrets.token_bytes(16)
    doc = {
        "username": username,
        "salt": salt.hex(),
        "hash": _hash_password(password, salt).hex(),
        "created_at": _now(),
    }
    if db().users.find_one({"username": username}):
        raise PanelError("That username is taken.", 409)
    try:
        db().users.insert_one(doc)
    except Exception:
        raise PanelError("That username is taken.", 409) from None
    return login(username, password)


def login(username: str, password: str) -> str:
    username = (username or "").strip().lower()
    user = db().users.find_one({"username": username})
    if user is None:
        # Spend the same work as a real login so a missing user isn't timeable.
        _hash_password(password or "", b"\x00" * 16)
        raise PanelError("Wrong username or password.", 401)

    expect = bytes.fromhex(user["hash"])
    got = _hash_password(password or "", bytes.fromhex(user["salt"]))
    if not hmac.compare_digest(expect, got):
        raise PanelError("Wrong username or password.", 401)

    token = secrets.token_urlsafe(32)
    db().sessions.insert_one({
        "token_hash": _token_hash(token),
        "username": username,
        "created_at": _now(),
        "expires_at": _now() + timedelta(days=config.SESSION_DAYS),
    })
    return token


def logout(token: str) -> None:
    if token:
        db().sessions.delete_one({"token_hash": _token_hash(token)})


def user_for(token: str) -> str:
    """The username behind a session token, or raise 401."""
    if not token:
        raise PanelError("Sign in first.", 401)
    s = db().sessions.find_one({"token_hash": _token_hash(token)})
    if s is None:
        raise PanelError("Session expired. Sign in again.", 401)
    expires = s["expires_at"]
    if expires.tzinfo is None:
        expires = expires.replace(tzinfo=timezone.utc)
    if expires < _now():
        db().sessions.delete_one({"_id": s["_id"]})
        raise PanelError("Session expired. Sign in again.", 401)
    return s["username"]
