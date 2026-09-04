from chd_common import (deployment_clock_target, replay_clock_targets,
                        replay_time_base)


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
