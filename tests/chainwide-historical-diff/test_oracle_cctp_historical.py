from __future__ import annotations

import runpy
from pathlib import Path
from typing import Any


HERE = Path(__file__).parent
CASES = HERE / "cases"
HISTORICAL = runpy.run_path(str(HERE / "oracle_cctp_historical.py"))


def load_cases() -> dict[str, Any]:
    case_data = HISTORICAL["CaseData"]
    return {tag: case_data.load(CASES, tag) for tag in HISTORICAL["CASE_CONFIG"]}


def test_joint_stream_contains_each_root_call_once() -> None:
    cases = load_cases()
    stream = HISTORICAL["historical_stream"](cases)

    root_calls = [item for item in stream if item["kind"] == "call"]
    creations = [item for item in stream if item["kind"] == "create"]
    lifted_calls = [
        call
        for case in cases.values()
        for call in case.calls["calls"]
        if "#" in call["hash"]
    ]

    # Counts derive from the fetched window (originally 429/3/12 at the
    # 200/200/41 windows) so deepening a window doesn't invalidate the test.
    expected_roots = sum(
        1
        for case in cases.values()
        for call in case.calls["calls"]
        if "#" not in call["hash"]
    )
    assert len(root_calls) == expected_roots > 0
    assert len({item["call"]["hash"] for item in root_calls}) == expected_roots
    assert len(creations) == 3
    assert len(lifted_calls) == sum(
        1
        for case in cases.values()
        for call in case.calls["calls"]
        if "#" in call["hash"]
    )
    assert all("#" not in item["call"]["hash"] for item in root_calls)
    assert stream == sorted(
        stream,
        key=lambda item: (
            item["block"],
            item["txindex"] if item["txindex"] is not None else 1 << 30,
            item["case_order"],
            item["case_index"],
            item["serial"],
        ),
    )


def test_verified_receipt_corrections_override_stale_fixture() -> None:
    cases = load_cases()
    by_hash = {
        call["hash"]: call
        for case in cases.values()
        for call in case.calls["calls"]
        if "#" not in call["hash"]
    }

    checked = 0
    for tx_hash, receipt in HISTORICAL["MAINNET_RECEIPT_METADATA"].items():
        # The correction table can cover a wider audited campaign than the
        # small repository fixture bundled with this unit test.
        call = by_hash.get(tx_hash)
        if call is None:
            continue
        checked += 1
        assert call["hist_ok"] is True
        assert HISTORICAL["historical_ok"](call) is receipt["historical_ok"]
    assert checked > 0


def test_historical_contract_addresses_preserve_signed_low_word() -> None:
    for config in [
        *HISTORICAL["CASE_CONFIG"].values(),
        HISTORICAL["STUB_CONFIG"],
    ]:
        encoded = HISTORICAL["address_argument"](config["address"])
        assert encoded == bytes(24) + config["app_id"].to_bytes(8, "big")

    eoa = "0xe69f81b825d7dc31ee9becef4dbeab5cf30e3abb"
    assert HISTORICAL["address_argument"](eoa) == bytes(12) + bytes.fromhex(eoa[2:])


def test_pre08_shims_are_narrow_and_repeatable() -> None:
    transmitter = CASES / "cctp_transmitter" / "out_avm"
    source = (transmitter / "MessageTransmitter.approval.teal").read_text()
    patched, applied = HISTORICAL["pre08_compat_teal"](source)

    assert "TypedMemView.index uint8(32 * 8) wrap restored with unchecked" in applied
    assert "receiveMessage/replaceMessage ensure_budget(45000) OpUp shim" in applied
    assert "receiveMessage forwards exact signed message bytes[116:]" in applied
    assert "replaceMessage caller-app comparison uses address low 64 bits" in applied
    assert patched.count("__historical_ensure_budget:") == 1
    assert "    load 255\n    extract 116 0\n" in patched
    assert "    intc 8 // 256\n    %\n" in patched

    try:
        HISTORICAL["pre08_compat_teal"](patched)
    except ValueError as error:
        assert str(error) == "historical ensure-budget shim already exists"
    else:
        raise AssertionError("applying the compatibility shim twice must fail")


# ── zero-log receipt correction inside duplicate-payload groups ─────────────
# Real case: transmitter calls 798/799 carry a BYTE-IDENTICAL attested message
# (sourceDomain 3, nonce 682) three minutes apart, and the corpus marks both
# "ok". CCTP's usedNonces makes that impossible; 798's receipt has ZERO logs
# while 799's has the full Mint/Transfer/MintAndWithdraw/MessageReceived set,
# so the chain rejected 798 and accepted 799. The replay accepts whichever it
# sees first, which is the opposite order — an environmental race, not a
# semantic divergence.
DUP_A = "0xaba28add2492fb5d9c000000000000000000000000000000000000000000000000"
DUP_B = "0xbf95c7138f75d9e38b000000000000000000000000000000000000000000000000"


class _Stub:
    """Minimal stand-in exercising the real Runner methods (both bound below)."""

    zero_log_ok_hashes = HISTORICAL["Runner"].zero_log_ok_hashes

    def __init__(self, tmp_path, zero_log_hashes, rows, calls):
        self.cases_path = tmp_path
        self.results = rows
        self.receipt_corrections = []
        tag = next(iter(HISTORICAL["CASE_CONFIG"]))
        self.tag = tag
        (tmp_path / tag).mkdir(parents=True, exist_ok=True)
        import json as _json
        (tmp_path / tag / "logs.json").write_text(_json.dumps(
            {h: ([] if h in zero_log_hashes else [{"address": "0x0",
                                                  "topics": ["0x0"],
                                                  "data": "0x"}])
             for h in (DUP_A, DUP_B)}))

        class _Case:
            def __init__(self, calls):
                self.calls = {"calls": calls}

        self.cases = {tag: _Case(calls)}


def _rows_and_calls():
    tag = next(iter(HISTORICAL["CASE_CONFIG"]))
    args = [{"__b__": "dead"}, {"__b__": "beef"}]
    calls = [
        {"hash": DUP_A, "sig": "receiveMessage(bytes,bytes)", "args": args},
        {"hash": DUP_B, "sig": "receiveMessage(bytes,bytes)", "args": args},
    ]
    rows = [
        {"tag": tag, "hash": DUP_A, "signature": "receiveMessage(bytes,bytes)",
         "historical_ok": True, "oracle_result": "ACCEPT",
         "matched_status": True},
        {"tag": tag, "hash": DUP_B, "signature": "receiveMessage(bytes,bytes)",
         "historical_ok": True, "oracle_result": "PANIC",
         "matched_status": False},
    ]
    return rows, calls


def test_zero_log_duplicate_is_corrected_and_race_reclassified(tmp_path) -> None:
    rows, calls = _rows_and_calls()
    stub = _Stub(tmp_path, {DUP_A}, rows, calls)
    HISTORICAL["Runner"].reclassify_payload_races(stub)

    assert all(r["matched_status"] for r in rows), rows
    assert len(stub.receipt_corrections) == 1
    correction = stub.receipt_corrections[0]
    assert correction["hash"] == DUP_A
    assert "ZERO logs" in correction["reason"]
    assert "zero logs" in rows[1]["status_note"]


def test_correction_needs_the_zero_log_evidence(tmp_path) -> None:
    """Without a zero-log receipt the group stays a real mismatch."""
    rows, calls = _rows_and_calls()
    stub = _Stub(tmp_path, set(), rows, calls)   # both receipts carry logs
    HISTORICAL["Runner"].reclassify_payload_races(stub)

    assert rows[1]["matched_status"] is False
    assert stub.receipt_corrections == []


def test_correction_never_promotes_a_failure(tmp_path) -> None:
    """A historically-FAILED row is never rewritten, zero logs or not."""
    rows, calls = _rows_and_calls()
    rows[0]["historical_ok"] = False
    rows[0]["oracle_result"] = "PANIC"
    rows[0]["matched_status"] = True
    stub = _Stub(tmp_path, {DUP_A, DUP_B}, rows, calls)
    HISTORICAL["Runner"].reclassify_payload_races(stub)

    assert stub.receipt_corrections == [] or all(
        c["hash"] != DUP_A for c in stub.receipt_corrections)
