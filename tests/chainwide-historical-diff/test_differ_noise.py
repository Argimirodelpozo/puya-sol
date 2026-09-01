"""Guard: residual clock skew between the two legs stays classified as noise.

Both legs pin block time to the same replayed instants, but py-evm still
advances its own clock a second per mined block, so a ctor-stored
`block.timestamp` lands tens of seconds apart. cow/ena/pol each store one
(`timestampLastMinting`, `lastMintTimestamp`, `lastMint`) and were certified
green only because the differ absorbs that gap.

The classifier was once deleted with nothing replacing it, turning all three
corpus cases red on values 91 s, 61 s and 34 s apart. These tests pin the
behaviour at every site that consumes it, and the beyond-skew cases pin the
other direction: the rule must not grow into hiding a real divergence.
"""
import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from differ import diff_case  # noqa: E402

# ena's real pair, 61 s apart; the far value is well beyond the 300 s bound.
TS_EVM = 2_887_032_306
TS_NEAR = 2_887_032_245
TS_FAR = 2_887_040_000


def _case(tmp_path: Path, *, evm_val, avm_val) -> Path:
    """Minimal two-leg case carrying one timestamp observable at each site."""
    d = tmp_path / "case"
    d.mkdir()

    def leg(val):
        return {
            "results": {},
            "snapshots": {"24": {"lastMint()": [val]}},
            "block_no": {},
            "storage": {
                "scalars": {"lastMint": val},
                "maps": {"_mintedAt": {"alice": val,
                                       "bob": [1, val]}},
                "writes": {},
            },
            "storage_delta": {},
        }

    (d / "case.json").write_text(json.dumps(
        {"tag": "t", "name": "N", "address": "0x0", "abi": []}))
    (d / "calls.json").write_text(json.dumps({"meta": {}, "calls": []}))
    (d / "evm_results.json").write_text(json.dumps(leg(evm_val)))
    (d / "avm_results.json").write_text(json.dumps(leg(avm_val)))
    return d


def _counts(tmp_path, *, evm_val, avm_val):
    return diff_case(_case(tmp_path, evm_val=evm_val, avm_val=avm_val))["counts"]


@pytest.mark.parametrize("bucket", ["snapshot_div", "storage_div",
                                    "storage_map_div"])
def test_within_skew_is_not_a_divergence(tmp_path, bucket):
    assert _counts(tmp_path, evm_val=TS_EVM, avm_val=TS_NEAR)[bucket] == 0


def test_within_skew_is_still_reported_as_noise(tmp_path):
    counts = _counts(tmp_path, evm_val=TS_EVM, avm_val=TS_NEAR)
    # Absorbed, never dropped: the pair stays visible for triage.
    assert counts["snapshot_noise"] >= 1
    assert counts["storage_noise"] >= 2      # scalar + both map entries


@pytest.mark.parametrize("bucket", ["snapshot_div", "storage_div",
                                    "storage_map_div"])
def test_beyond_skew_is_a_real_divergence(tmp_path, bucket):
    assert _counts(tmp_path, evm_val=TS_EVM, avm_val=TS_FAR)[bucket] >= 1


def test_equal_timestamps_produce_nothing(tmp_path):
    counts = _counts(tmp_path, evm_val=TS_EVM, avm_val=TS_EVM)
    assert counts["snapshot_div"] == 0 and counts["storage_div"] == 0
    assert counts["storage_map_div"] == 0 and counts["storage_noise"] == 0


def _map_case(tmp_path: Path, *, evm_map, avm_map, calls=(),
              time_base=0) -> Path:
    d = tmp_path / "maps"
    d.mkdir()

    def leg(maps):
        return {"results": {}, "snapshots": {}, "block_no": {},
                "storage": {"scalars": {}, "maps": maps, "writes": {}},
                "storage_delta": {}, "time_base": time_base}

    (d / "case.json").write_text(json.dumps(
        {"tag": "t", "name": "N", "address": "0x0", "abi": []}))
    (d / "calls.json").write_text(json.dumps(
        {"meta": {}, "calls": list(calls)}))
    (d / "evm_results.json").write_text(json.dumps(leg(evm_map)))
    (d / "avm_results.json").write_text(json.dumps(leg(avm_map)))
    return d


# LocalNet's clock only moves forward, so a batch of replays creeps away from
# each case's own window until time-gated code fails on the AVM leg alone. bgb
# reported 342 "REAL divergences" that way, at +572 d.
HISTORICAL = 1_600_000_000


def test_a_shifted_replay_with_divergences_is_flagged(tmp_path):
    report = diff_case(_map_case(
        tmp_path, evm_map={"_m": {"«1»": 1}}, avm_map={"_m": {"«1»": 2}},
        calls=[{"i": 0, "ts": HISTORICAL}],
        time_base=HISTORICAL + 600 * 86400,
    ))
    assert report["counts"]["storage_map_div"] == 1
    assert report["findings"]["clock_epoch_shift"][0]["shift_days"] == 600


def test_a_shifted_replay_with_NO_divergences_is_not_flagged(tmp_path):
    # An old window ALWAYS replays shifted (py-evm starts at wall clock), so
    # warning on a clean case would fire nearly every run and teach the reader
    # to ignore it. The caveat only earns its place next to a red result.
    report = diff_case(_map_case(
        tmp_path, evm_map={}, avm_map={},
        calls=[{"i": 0, "ts": HISTORICAL}],
        time_base=HISTORICAL + 600 * 86400,
    ))
    assert "clock_epoch_shift" not in report["findings"]


def test_a_replay_at_its_own_window_is_not_flagged(tmp_path):
    report = diff_case(_map_case(
        tmp_path, evm_map={"_m": {"«1»": 1}}, avm_map={"_m": {"«1»": 2}},
        calls=[{"i": 0, "ts": HISTORICAL}], time_base=HISTORICAL,
    ))
    assert report["counts"]["storage_map_div"] == 1
    assert "clock_epoch_shift" not in report["findings"]


def test_entry_present_on_one_leg_holding_the_default_is_noise(tmp_path):
    # The EVM leg reads slots and cannot enumerate a mapping, so it has no way
    # to tell a default-valued entry from an absent one.
    counts = diff_case(_map_case(
        tmp_path, evm_map={"_m": {}}, avm_map={"_m": {"«D3»": 0}},
    ))["counts"]
    assert counts["storage_map_div"] == 0
    # Deep default-equivalence (_defaults_equal) now folds this at the
    # equality layer: a default-valued entry vs an absent one is the SAME
    # state, silently equal rather than surfaced as classified noise.
    assert counts["storage_noise"] == 0


def test_an_all_default_struct_is_noise_too(tmp_path):
    counts = diff_case(_map_case(
        tmp_path, evm_map={"_m": {}},
        avm_map={"_m": {"«1»": [0, 0, False, "0x00"]}},
    ))["counts"]
    assert counts["storage_map_div"] == 0


def test_a_non_default_entry_on_one_leg_is_still_a_divergence(tmp_path):
    counts = diff_case(_map_case(
        tmp_path, evm_map={"_m": {}}, avm_map={"_m": {"«1»": 7}},
    ))["counts"]
    assert counts["storage_map_div"] == 1


def test_a_mostly_skipped_run_is_flagged_as_near_vacuous(tmp_path, capsys):
    """A ✅ over 3% of the window must not look like a ✅ over all of it."""
    from differ import print_report
    print_report({"tag": "t", "name": "N", "txns_in_window": 300, "replayed": 20,
                  "skips": {"closed-world": 280}, "platform_limits": 0,
                  "findings": {}, "counts": {}})
    assert "20/300" in capsys.readouterr().out


def test_a_full_run_is_not_flagged(tmp_path, capsys):
    from differ import print_report
    print_report({"tag": "t", "name": "N", "txns_in_window": 557, "replayed": 554,
                  "skips": {}, "platform_limits": 0, "findings": {}, "counts": {}})
    assert "vacuous" not in capsys.readouterr().out
