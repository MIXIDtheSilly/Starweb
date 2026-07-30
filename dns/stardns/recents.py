"""Tracks the handful of pages a user last visited, for the account home's
Recent column. A page calls touch() as it's viewed; repeat visits just bump
the timestamp on the same key rather than piling up duplicates."""
import time
from datetime import datetime, timezone

from .db import db

MAX_RECENTS = 3


def _now():
    return datetime.now(timezone.utc)


def touch(username: str, key: str, label: str, icon: str, path: str) -> None:
    # seq is a same-process monotonic tiebreaker: two touches can land in the
    # same wall-clock microsecond (e.g. clicking straight through two tabs),
    # and visited_at alone would then leave their order to chance.
    db().recents.update_one(
        {"username": username, "key": key},
        {"$set": {"label": label, "icon": icon, "path": path,
                  "visited_at": _now(), "seq": time.monotonic_ns()}},
        upsert=True,
    )


def list_recent(username: str, limit: int = MAX_RECENTS) -> list[dict]:
    docs = (db().recents.find({"username": username})
            .sort([("visited_at", -1), ("seq", -1)]))
    return list(docs)[:limit]
