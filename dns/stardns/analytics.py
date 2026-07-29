"""Query analytics: counters bumped by the resolver and read back by the panel
for graphs.

Two resolutions are kept. `stats` holds one document per (domain, day) and is
what every window of a day or more is built from; it is never expired, so the
all-time total stays exact. `stats_min` holds one per (domain, minute) and only
exists to make the hour and day windows possible, so it carries a timestamp and
a TTL index and ages out after config.STATS_MINUTE_DAYS.
"""
from datetime import date, datetime, timedelta, timezone

from .db import db

DEFAULT_DAYS = 14
DEFAULT_RANGE = "14d"

DAY = 86400

# key, pill label, full label, bucket length in seconds, bucket count, bucket noun.
# Past a month the buckets widen: 180 daily points read as a smear at chart
# width, and a year of them would not fit at all.
RANGES = (
    ("1h",   "1h",  "Last hour",     300,      12, "5 minutes"),
    ("24h",  "24h", "Last 24 hours", 3600,     24, "hour"),
    ("7d",   "7d",  "Last 7 days",   DAY,       7, "day"),
    ("14d",  "14d", "Last 14 days",  DAY,      14, "day"),
    ("30d",  "30d", "Last 30 days",  DAY,      30, "day"),
    ("90d",  "3m",  "Last 3 months", DAY * 7,  13, "week"),
    ("180d", "6m",  "Last 6 months", DAY * 7,  26, "week"),
    ("365d", "1y",  "Last year",     DAY * 30, 12, "30 days"),
)

_BY_KEY = {r[0]: r for r in RANGES}


def spec(key: str) -> tuple:
    """The RANGES row for a key, falling back to the default for anything
    unrecognised, since the key arrives from a URL."""
    return _BY_KEY.get(key, _BY_KEY[DEFAULT_RANGE])


def _today():
    return datetime.now(timezone.utc).date()


def _iso(d) -> str:
    return d.isoformat()


def _minute(dt: datetime) -> str:
    return dt.strftime("%Y-%m-%dT%H:%M")


def record_query(domain: str) -> None:
    """Bump today's counter for domain. Best-effort: a dropped count is fine,
    a SERVFAIL from an analytics hiccup is not, so failures are swallowed."""
    try:
        now = datetime.now(timezone.utc)
        db().stats.update_one(
            {"domain": domain, "day": _iso(now.date())},
            {"$inc": {"count": 1}}, upsert=True)
        db().stats_min.update_one(
            {"domain": domain, "minute": _minute(now)},
            {"$inc": {"count": 1}, "$setOnInsert": {"at": now}}, upsert=True)
    except Exception:
        pass


def day_labels(days: int = DEFAULT_DAYS) -> list[str]:
    today = _today()
    return [(today - timedelta(days=days - 1 - i)).strftime("%m/%d")
            for i in range(days)]


def _minute_buckets(domains: list[str], step: int,
                    count: int) -> tuple[list[int], list[str]]:
    """Sub-day windows, read off stats_min."""
    now = datetime.now(timezone.utc).replace(second=0, microsecond=0)
    # Floored to the bucket boundary so the buckets stay put as minutes pass;
    # every sub-day step divides an hour, so minutes-into-the-day is enough.
    step_min = step // 60
    now -= timedelta(minutes=(now.hour * 60 + now.minute) % step_min)
    start = now - timedelta(seconds=step * (count - 1))
    end = start + timedelta(seconds=step * count)

    values = [0] * count
    labels = [(start + timedelta(seconds=step * i)).strftime("%H:%M")
              for i in range(count)]
    if not domains:
        return values, labels

    for r in db().stats_min.find({"domain": {"$in": domains},
                                  "minute": {"$gte": _minute(start),
                                             "$lt": _minute(end)}}):
        when = datetime.strptime(r["minute"], "%Y-%m-%dT%H:%M")
        i = int((when - start.replace(tzinfo=None)).total_seconds()) // step
        if 0 <= i < count:
            values[i] += r["count"]
    return values, labels


def _day_buckets(domains: list[str], step_days: int,
                 count: int) -> tuple[list[int], list[str]]:
    """Windows of a day or more, read off stats. A bucket is labelled with the
    date it starts on, so a weekly bucket reads as its Monday-equivalent."""
    span = step_days * count
    first = _today() - timedelta(days=span - 1)
    values = [0] * count
    labels = [(first + timedelta(days=step_days * i)).strftime("%m/%d")
              for i in range(count)]
    if not domains:
        return values, labels

    last = first + timedelta(days=span)
    for r in db().stats.find({"domain": {"$in": domains},
                              "day": {"$gte": _iso(first), "$lt": _iso(last)}}):
        i = (date.fromisoformat(r["day"]) - first).days // step_days
        if 0 <= i < count:
            values[i] += r["count"]
    return values, labels


def series(domains: list[str], key: str = DEFAULT_RANGE) -> tuple[list[int], list[str]]:
    """One window's counts and bucket labels, oldest bucket first, summed over
    every domain in the list."""
    _, _, _, step, count, _ = spec(key)
    if step < DAY:
        return _minute_buckets(domains, step, count)
    return _day_buckets(domains, step // DAY, count)


def daily_counts(domain: str, days: int = DEFAULT_DAYS) -> list[int]:
    """One domain's series, oldest day first, zero-filled where uncounted."""
    return _day_buckets([domain], 1, days)[0]


def daily_totals(domains: list[str], days: int = DEFAULT_DAYS) -> list[int]:
    """The combined series across every domain in the list, for an
    account-wide graph. Zero-filled the same way as daily_counts."""
    return _day_buckets(domains, 1, days)[0]


def total_queries(domain: str, days: int | None = None) -> int:
    """All-time total by default; pass days to window it like the graphs."""
    query = {"domain": domain}
    if days is not None:
        query["day"] = {"$gte": _iso(_today() - timedelta(days=days - 1))}
    return sum(r["count"] for r in db().stats.find(query))
