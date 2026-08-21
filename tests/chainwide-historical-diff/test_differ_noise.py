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
