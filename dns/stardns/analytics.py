"""Query analytics: a daily counter per domain, bumped by the resolver and
read back by the panel for graphs. One document per (domain, day), not per
query; daily resolution is all a hobby-scale zone needs.
"""
from datetime import datetime, timedelta, timezone

from .db import db

DEFAULT_DAYS = 14


def _today():
    return datetime.now(timezone.utc).date()


def _iso(d) -> str:
    return d.isoformat()


def record_query(domain: str) -> None:
    """Bump today's counter for domain. Best-effort: a dropped count is fine,
    a SERVFAIL from an analytics hiccup is not, so failures are swallowed."""
    try:
        db().stats.update_one(
            {"domain": domain, "day": _iso(_today())},
            {"$inc": {"count": 1}}, upsert=True)
    except Exception:
        pass


def day_labels(days: int = DEFAULT_DAYS) -> list[str]:
    today = _today()
    return [(today - timedelta(days=days - 1 - i)).strftime("%m/%d")
            for i in range(days)]


def daily_counts(domain: str, days: int = DEFAULT_DAYS) -> list[int]:
    """One domain's series, oldest day first, zero-filled where uncounted."""
    today = _today()
    start = _iso(today - timedelta(days=days - 1))
    rows = {r["day"]: r["count"]
            for r in db().stats.find({"domain": domain, "day": {"$gte": start}})}
    return [rows.get(_iso(today - timedelta(days=days - 1 - i)), 0)
            for i in range(days)]


def daily_totals(domains: list[str], days: int = DEFAULT_DAYS) -> list[int]:
    """The combined series across every domain in the list, for an
    account-wide graph. Zero-filled the same way as daily_counts."""
    if not domains:
        return [0] * days
    today = _today()
    start = _iso(today - timedelta(days=days - 1))
    totals: dict[str, int] = {}
    for r in db().stats.find({"domain": {"$in": domains}, "day": {"$gte": start}}):
        totals[r["day"]] = totals.get(r["day"], 0) + r["count"]
    return [totals.get(_iso(today - timedelta(days=days - 1 - i)), 0)
            for i in range(days)]


def total_queries(domain: str, days: int | None = None) -> int:
    """All-time total by default; pass days to window it like the graphs."""
    query = {"domain": domain}
    if days is not None:
        query["day"] = {"$gte": _iso(_today() - timedelta(days=days - 1))}
    return sum(r["count"] for r in db().stats.find(query))
