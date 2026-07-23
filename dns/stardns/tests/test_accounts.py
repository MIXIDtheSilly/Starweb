import pytest

from stardns import auth, config, zones
from stardns.errors import PanelError


def test_register_then_login(fake_db):
    auth.register("alice", "correct horse")
    token = auth.login("alice", "correct horse")
    assert auth.user_for(token) == "alice"


def test_wrong_password_is_rejected(fake_db):
    auth.register("alice", "correct horse")
    with pytest.raises(PanelError) as e:
        auth.login("alice", "wrong horse")
    assert e.value.status == 401


def test_password_is_not_stored(fake_db):
    auth.register("alice", "correct horse")
    stored = fake_db.users.find_one({"username": "alice"})
    assert "correct horse" not in str(stored)
    assert set(stored) >= {"salt", "hash"}


def test_username_is_taken(fake_db):
    auth.register("alice", "correct horse")
    with pytest.raises(PanelError) as e:
        auth.register("Alice", "another password")
    assert e.value.status == 409


def test_short_password_rejected(fake_db):
    with pytest.raises(PanelError):
        auth.register("alice", "short")


def test_logout_kills_the_session(fake_db):
    token = auth.register("alice", "correct horse")
    auth.logout(token)
    with pytest.raises(PanelError):
        auth.user_for(token)


def test_unknown_token_is_rejected(fake_db):
    with pytest.raises(PanelError) as e:
        auth.user_for("nonsense")
    assert e.value.status == 401


def test_domain_limit_is_three(account):
    user = auth.user_for(account)
    for i in range(config.MAX_DOMAINS):
        zones.add_domain(user, f"site{i}")
    with pytest.raises(PanelError) as e:
        zones.add_domain(user, "onemore")
    assert e.value.status == 403
    assert str(config.MAX_DOMAINS) in e.value.message


def test_deleting_frees_a_slot(account):
    user = auth.user_for(account)
    for i in range(config.MAX_DOMAINS):
        zones.add_domain(user, f"site{i}")
    zones.delete_domain(user, "site0")
    assert zones.add_domain(user, "onemore")["name"] == f"onemore.{config.ZONE}"


def test_another_account_cannot_touch_your_domain(account):
    zones.add_domain("tester", "mine")
    auth.register("mallory", "hunter2hunter2")
    with pytest.raises(PanelError) as e:
        zones.get_domain("mallory", "mine")
    assert e.value.status == 404  # not 403: ownership isn't disclosed


def test_domain_names_are_globally_unique(account):
    zones.add_domain("tester", "mine")
    auth.register("mallory", "hunter2hunter2")
    with pytest.raises(PanelError) as e:
        zones.add_domain("mallory", "mine")
    assert e.value.status == 409
