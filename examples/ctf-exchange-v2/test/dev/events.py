"""ARC-28 event emission helpers for tests.

`emit Event(...)` in Solidity is lowered by puya-sol to a raw `log` opcode
with an ARC-28 payload: `selector(4) ++ arc4_encode(args)`.

The selector is the first 4 bytes of `sha512_256("EventName(arc4_types)")`
— matches puya's `algopy.arc4.Method`-style hashing for events.

The Solidity-side type names that puya-sol uses for the signature mirror
`SolEmitStatement.cpp::arc4SigName` exactly: address → "uint8[32]",
uint256 → "uint256", uint64 → "uint64", bool → "bool", bytes32 → "byte[32]".

Usage:
    sel = event_selector("TradingPaused", ["uint8[32]"])
    logs = call_result.tx_ids ...                          # extract logs
    assert_event_in_logs(logs, sel, expected_payload)
"""
from __future__ import annotations

import hashlib
from typing import Iterable


def event_selector(name: str, arc4_types: Iterable[str]) -> bytes:
    """Compute the 4-byte ARC-28 selector for `name(arc4_types)`."""
    sig = f"{name}({','.join(arc4_types)})"
    h = hashlib.new("sha512_256")
    h.update(sig.encode())
    return h.digest()[:4]


def decode_logs(tx_result) -> list[bytes]:
    """Pull `op.log` payloads out of a confirmed-txn algokit result.

    `tx_result` may be either an algokit `SendAppCallTransactionResult`-like
    object (with `.confirmation` containing `logs`) or a raw confirmation
    dict from algod. Returns a list of raw log byte strings, in emit order.
    """
    confirmation = getattr(tx_result, "confirmation", None) or tx_result
    logs = []
    if isinstance(confirmation, dict):
        for entry in confirmation.get("logs", []) or []:
            if isinstance(entry, str):
                import base64
                logs.append(base64.b64decode(entry))
            elif isinstance(entry, (bytes, bytearray)):
                logs.append(bytes(entry))
    # Some algokit response shapes expose inner-txn logs separately. Walk
    # them too so we capture events emitted during inner calls.
    inner = (confirmation or {}).get("inner-txns", []) if isinstance(confirmation, dict) else []
    for itxn in inner:
        logs.extend(decode_logs(itxn))
    return logs


def find_event(logs: Iterable[bytes], selector: bytes) -> bytes | None:
    """Return the first log payload (after the 4-byte selector) whose
    selector matches; None if not found."""
    for raw in logs:
        if len(raw) >= 4 and raw[:4] == selector:
            return raw[4:]
    return None


def assert_event_emitted(
    tx_result,
    name: str,
    arc4_types: Iterable[str],
    expected_payload: bytes | None = None,
) -> bytes:
    """Assert `name(arc4_types)` was emitted; optionally that its arc4
    payload (after selector) matches `expected_payload` byte-exact.

    Returns the matched log's payload (without the 4-byte selector). The
    caller can decode further as needed. Inner-txn logs are searched too.
    """
    types_list = list(arc4_types)
    sel = event_selector(name, types_list)
    logs = decode_logs(tx_result)
    payload = find_event(logs, sel)
    if payload is None:
        sigs = [_log_signature(raw) for raw in logs]
        raise AssertionError(
            f"event {name}({','.join(types_list)}) not emitted; "
            f"selector={sel.hex()}, logs found: {sigs}"
        )
    if expected_payload is not None and payload != expected_payload:
        raise AssertionError(
            f"event {name} payload mismatch:\n  got     = {payload.hex()}\n"
            f"  wanted  = {expected_payload.hex()}"
        )
    return payload


def _log_signature(raw: bytes) -> str:
    """Pretty-print a raw log entry: 4-byte selector + payload-hex."""
    if len(raw) >= 4:
        return f"{raw[:4].hex()}+{len(raw)-4}B"
    return f"{raw.hex()} (short)"
