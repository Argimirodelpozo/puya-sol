"""The two-hop campaign must prove nesting and rollback, not just matching statuses."""
import hashlib

import pytest
from eth_abi import encode

from chd_common import dump_json
from sp1_three_contracts import avm_tree, calldata, compare, has_two_hops, proof_rows


def proof_case():
    return {"txns": [{
        "hash": "local-source-proof", "hist_ok": True,
        "input": "0x" + calldata("verifyProof(bytes32,bytes,bytes)",
            ["bytes32", "bytes", "bytes"],
            [bytes(32), b"public values", bytes.fromhex("4388a21c") + bytes(352)]).hex(),
    }]}


def test_controls_are_explicit_and_keep_a_valid_recovery_call():
    key, rows = proof_rows(proof_case())
    assert key == bytes(32)
    assert [row["expected_ok"] for row in rows] == [True, False, False, False, True]
    assert rows[0]["public_values"] != rows[1]["public_values"]
    assert rows[2]["proof"].startswith("ffffffff")
    assert len(rows[3]["proof"]) == len(rows[0]["proof"]) - 2
    assert rows[-1]["proof"] == rows[0]["proof"]
    assert rows[-1]["public_values"] == rows[0]["public_values"]


def test_proof_window_requires_one_program_key():
    case = proof_case()
    txn = case["txns"][0]
    case["txns"].append({**txn, "input": txn["input"][:10]
        + encode(["bytes32", "bytes", "bytes"],
                 [b"1" * 32, b"public values", bytes.fromhex("4388a21c") + bytes(352)]).hex()})
    with pytest.raises(ValueError, match="exactly one program key"):
        proof_rows(case)


def test_two_hops_require_a_nested_verifier_not_a_sibling():
    gateway = {"u64": {"ApplicationID": 9003}}
    verifier = {"u64": {"ApplicationID": 9002}}
    assert not has_two_hops(avm_tree([gateway, verifier]))
    assert has_two_hops(avm_tree([{**gateway, "inner_txns": [verifier]}]))
    assert not has_two_hops(avm_tree([{"u64": {"ApplicationID": 8002}}]))


@pytest.mark.parametrize("broken", ["rollback", "nesting", "status", "platform_limit", "none"])
def test_comparison_checks_independent_expectations(tmp_path, broken):
    values = b"public values"
    digest = hashlib.sha256(values).hexdigest()
    rows = [{"kind": "valid", "expected_ok": True, "public_values": values.hex()},
            {"kind": "invalid", "expected_ok": False, "public_values": values.hex()}]
    results = [
        {"ok": True, "return": digest, "state": [1, digest], "route": ["verifier", False], "platform_limit": False,
         "inner_tree": [{"contract": "gateway", "children": [
             {"contract": "verifier", "children": []}]}]},
        {"ok": False, "return": "", "state": [1, digest], "route": ["verifier", False], "platform_limit": False,
         "inner_tree": []},
    ]
    if broken == "rollback":
        results[-1]["state"] = [2, digest]
    elif broken == "nesting":
        results[0]["inner_tree"] = []
    elif broken == "status":
        results[-1]["ok"] = True
    elif broken == "platform_limit":
        results[-1]["platform_limit"] = True
    dump_json(tmp_path / "campaign.json", {"scope": "unit fixture", "rows": rows})
    # Even identical errors on BOTH legs must fail the independent expectations.
    for leg in ("evm", "avm"):
        dump_json(tmp_path / f"{leg}.json", {"results": results})
    assert bool(compare(tmp_path)["findings"]) is (broken != "none")


def test_incomplete_replay_cannot_pass(tmp_path):
    dump_json(tmp_path / "campaign.json", {"rows": [{}]})
    for leg in ("evm", "avm"):
        dump_json(tmp_path / f"{leg}.json", {"results": []})
    with pytest.raises(ValueError, match="incomplete"):
        compare(tmp_path)
