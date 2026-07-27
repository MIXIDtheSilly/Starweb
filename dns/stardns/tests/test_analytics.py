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
