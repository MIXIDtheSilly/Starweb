import threading

from pymongo import ASCENDING, MongoClient

from . import config

_lock = threading.Lock()
_db = None


def db():
    """One client per process, built on first use so importing doesn't dial Mongo."""
    global _db
    if _db is None:
        with _lock:
            if _db is None:
                client = MongoClient(config.MONGO_URI, serverSelectionTimeoutMS=3000)
                _db = client[config.MONGO_DB]
                _ensure_indexes(_db)
    return _db


def _ensure_indexes(d) -> None:
    d.users.create_index([("username", ASCENDING)], unique=True)
    d.sessions.create_index([("token_hash", ASCENDING)], unique=True)
    d.sessions.create_index([("expires_at", ASCENDING)], expireAfterSeconds=0)
    d.domains.create_index([("name", ASCENDING)], unique=True)
    d.domains.create_index([("owner", ASCENDING)])
    d.records.create_index([("domain", ASCENDING), ("name", ASCENDING),
                            ("type", ASCENDING)])
    d.certs.create_index([("domain", ASCENDING), ("issued_at", ASCENDING)])


def ping() -> None:
    db().command("ping")
