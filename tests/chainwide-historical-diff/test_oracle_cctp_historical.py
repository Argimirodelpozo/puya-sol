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

    assert len(root_calls) == 429
    assert len({item["call"]["hash"] for item in root_calls}) == 429
    assert len(creations) == 3
    assert len(lifted_calls) == 12
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
