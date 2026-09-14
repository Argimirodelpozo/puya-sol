from chd_common import (deployment_clock_target, replay_clock_targets,
                        replay_time_base)

import io
from urllib.error import HTTPError

import pytest
import chd_common


def test_deployment_clock_preserves_creation_lead_and_epoch_shift():
    calls = [{"i": 0, "ts": 500}, {"i": 1, "ts": 510}]

    assert deployment_clock_target(200, calls, 500) == 200
    assert deployment_clock_target(200, calls, 1_500) == 1_200

    assert replay_time_base(0, 200, calls) == 500
    shifted_base = replay_time_base(600, 200, calls)
    assert shifted_base == 901
    assert deployment_clock_target(200, calls, shifted_base) == 601


def test_replay_clock_targets_preserves_gaps_and_breaks_ties():
    calls = [
        {"i": 0, "ts": 100},
        {"i": 1, "ts": 100},
        {"i": 2, "ts": 105},
        {"i": 3, "ts": 104},
    ]

    assert replay_clock_targets(calls, 1_000) == {
        0: 1_000,
        1: 1_001,
        2: 1_005,
        3: 1_006,
    }


def test_replay_clock_targets_are_stable_when_an_entry_is_skipped():
    calls = [
        {"i": 7, "ts": 50},
        {"i": 8, "ts": 50, "skip": "closed-world"},
        {"i": 9, "ts": 50},
    ]

    assert replay_clock_targets(calls, 2_000) == {
        7: 2_000,
        8: 2_001,
        9: 2_002,
    }


@pytest.mark.parametrize("retry_after,delay", [
    ("5", 5), ("Thu, 01 Jan 1970 00:01:45 GMT", 5), ("malformed", 1)])
def test_http_retry_respects_server_delay(monkeypatch, retry_after, delay):
    responses = [HTTPError("https://example.test", 429, "rate limit",
                           {"Retry-After": retry_after}, None), io.BytesIO(b'{"ok": true}')]
    sleeps = []

    def open_request(*args, **kwargs):
        response = responses.pop(0)
        if isinstance(response, Exception):
            raise response
        return response

    monkeypatch.setattr(chd_common.urllib.request, "urlopen", open_request)
    monkeypatch.setattr(chd_common.time, "sleep", sleeps.append)
    monkeypatch.setattr(chd_common.time, "time", lambda: 100)
    assert chd_common.http_json("https://example.test") == {"ok": True}
    assert sleeps == [delay]


@pytest.mark.parametrize("code,retry_after,expected_sleeps", [
    (429, "", [1, 2]), (503, "", [1, 2]), (404, "", []), (429, "120", [])])
def test_http_retry_is_bounded_and_preserves_failures(monkeypatch, code, retry_after, expected_sleeps):
    sleeps = []

    def fail(*args, **kwargs):
        raise HTTPError("https://example.test", code, "unavailable",
                        {"Retry-After": retry_after}, None)

    monkeypatch.setattr(chd_common.urllib.request, "urlopen", fail)
    monkeypatch.setattr(chd_common.time, "sleep", sleeps.append)
    with pytest.raises(HTTPError) as exc:
        chd_common.http_json("https://example.test")
    assert exc.value.code == code
    assert sleeps == expected_sleeps
    if retry_after == "120":
        assert "Retry-After seconds: 120" in exc.value.__notes__
