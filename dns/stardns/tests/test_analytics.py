from datetime import timedelta

from stardns import analytics


def test_record_query_bumps_todays_counter(fake_db):
    analytics.record_query("mysite.web")
    analytics.record_query("mysite.web")
    assert analytics.total_queries("mysite.web") == 2


def test_daily_counts_zero_fills_uncounted_days(fake_db):
    analytics.record_query("mysite.web")
    series = analytics.daily_counts("mysite.web", days=5)
    assert len(series) == 5
    assert series[-1] == 1
    assert series[:-1] == [0, 0, 0, 0]


def test_daily_counts_ignores_other_domains(fake_db):
    analytics.record_query("mysite.web")
    analytics.record_query("other.web")
    assert analytics.daily_counts("mysite.web")[-1] == 1


def test_daily_totals_combines_domains(fake_db):
    analytics.record_query("a.web")
    analytics.record_query("b.web")
    analytics.record_query("b.web")
    assert analytics.daily_totals(["a.web", "b.web"])[-1] == 3


def test_daily_totals_empty_domain_list(fake_db):
    assert analytics.daily_totals([]) == [0] * analytics.DEFAULT_DAYS


def test_total_queries_can_be_windowed(fake_db, monkeypatch):
    from datetime import datetime, timezone

    real_today = analytics._today()
    analytics.record_query("mysite.web")
    # Backdate a second day's row outside a 1-day window.
    old_day = (real_today - timedelta(days=10)).isoformat()
    from stardns.db import db
    db().stats.update_one({"domain": "mysite.web", "day": old_day},
                          {"$inc": {"count": 5}}, upsert=True)

    assert analytics.total_queries("mysite.web") == 6
    assert analytics.total_queries("mysite.web", days=1) == 1


def test_day_labels_length(fake_db):
    assert len(analytics.day_labels(7)) == 7


def test_series_defaults_to_fourteen_daily_buckets(fake_db):
    analytics.record_query("mysite.web")
    values, labels = analytics.series(["mysite.web"])
    assert len(values) == 14 and len(labels) == 14
    assert values[-1] == 1 and sum(values) == 1


def test_series_buckets_an_hour_into_five_minute_slots(fake_db):
    analytics.record_query("mysite.web")
    values, labels = analytics.series(["mysite.web"], "1h")
    assert len(values) == 12
    assert values[-1] == 1
    assert ":" in labels[0]


def test_series_buckets_a_day_into_hours(fake_db):
    analytics.record_query("mysite.web")
    analytics.record_query("mysite.web")
    values, _ = analytics.series(["mysite.web"], "24h")
    assert len(values) == 24 and values[-1] == 2


def test_series_widens_the_buckets_for_long_windows(fake_db):
    from stardns.db import db

    values, labels = analytics.series(["mysite.web"], "90d")
    assert len(values) == 13 and len(labels) == 13

    # 10 days back is two weekly buckets from the end of a 91-day window.
    old = (analytics._today() - timedelta(days=10)).isoformat()
    db().stats.update_one({"domain": "mysite.web", "day": old},
                          {"$inc": {"count": 4}}, upsert=True)
    values, _ = analytics.series(["mysite.web"], "90d")
    assert values[-2] == 4 and sum(values) == 4


def test_series_ignores_an_unknown_range_key(fake_db):
    assert len(analytics.series([], "nonsense")[0]) == 14


def test_minute_counters_carry_a_timestamp_for_the_ttl(fake_db):
    from stardns.db import db

    analytics.record_query("mysite.web")
    doc = db().stats_min.find_one({"domain": "mysite.web"})
    assert doc["count"] == 1 and doc["at"] is not None
