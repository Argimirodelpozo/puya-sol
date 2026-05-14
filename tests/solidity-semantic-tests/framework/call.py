"""Call a deployed contract method.

The framework exposes a typed Result:
  - result.abi_return   — the decoded return value, exactly as algosdk
                          presents it given the arc56 return type.
  - result.logs         — list of bytes from log opcodes (Solidity events).
  - result.reverted     — True if the call failed.
  - result.revert_data  — raw revert bytes (when available; populated via simulate).

Calls go through algokit's AppClient + ATC. Reverts come back as
exceptions; we re-simulate to capture revert reasons.
"""
from __future__ import annotations

import os
from copy import deepcopy
from dataclasses import dataclass, field
from typing import Any

import algokit_utils as au
from algosdk import abi as _abi
from algosdk import encoding
from algosdk.atomic_transaction_composer import (
    AtomicTransactionComposer,
    TransactionWithSigner,
    AccountTransactionSigner,
)
from algosdk.transaction import PaymentTxn, ApplicationCallTxn, OnComplete
from algosdk.v2client.models import (
    SimulateRequest,
    SimulateRequestTransactionGroup,
)

from .revert import classify_revert, Reverted, ErrorString, Panic, RawRevert


@dataclass
class Result:
    abi_return: Any = None
    logs: list[bytes] = field(default_factory=list)
    reverted: bool = False
    revert_data: bytes = b""
    revert_reason: Reverted | ErrorString | Panic | RawRevert | None = None
    raw_response: Any = None  # algosdk return for tests that need the raw bytes
    fail_message: str = ""


class CallError(Exception):
    """Raised when a call fails for non-revert reasons (no method, bad sig, etc.)."""


def _resolve_method(app_spec, sig: str):
    """Find an ABI method matching the Solidity-style signature.

    Accepts:
      "f()"
      "f(uint256)"
      "f(uint256,uint256)"
    Returns the matching ABI Method, or None if not found.
    """
    base_name = sig.split("(")[0]
    has_paren = "(" in sig
    target_inner = _extract_inner_args(sig) if has_paren else ""

    target_norm = _normalize_sol_inner(target_inner)
    for m in app_spec.methods:
        if m.name != base_name:
            continue
        abi_m = m.to_abi_method()
        abi_inner = _extract_inner_args(abi_m.get_signature())
        if _normalize_arc4_inner(abi_inner) == target_norm:
            return abi_m
    candidates = [m for m in app_spec.methods if m.name == base_name]
    if len(candidates) == 1:
        return candidates[0].to_abi_method()
    return None


def _extract_inner_args(method_sig: str) -> str:
    """Extract the first balanced-paren group from a method signature.

    ARC4 signatures with tuple returns look like
    `f(uint256[][])(uint256,uint256,uint256)`, so we need balanced-paren
    matching rather than a simple `rindex(')')`.
    """
    i = method_sig.find("(")
    if i < 0:
        return ""
    depth = 0
    for j in range(i, len(method_sig)):
        c = method_sig[j]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return method_sig[i + 1 : j]
    return method_sig[i + 1 :]


_SOL_TO_ARC4_SCALAR = {
    "bool": "bool",
    "address": "address",
    "string": "string",
    "bytes": "byte[]",
}
for _n in range(1, 33):
    _SOL_TO_ARC4_SCALAR[f"bytes{_n}"] = f"byte[{_n}]"
for _n in (8, 16, 32, 64, 128, 256):
    _SOL_TO_ARC4_SCALAR[f"uint{_n}"] = f"uint{_n}"
    _SOL_TO_ARC4_SCALAR[f"int{_n}"] = f"int{_n}"


def _normalize_sol_inner(s: str) -> str:
    return _normalize_inner(s, _sol_token_to_arc4)


def _normalize_arc4_inner(s: str) -> str:
    return _normalize_inner(s, lambda t: t)


def _normalize_inner(s: str, token_map):
    tokens = _split_top_level(s)
    return ",".join(_normalize_one(t, token_map) for t in tokens)


def _normalize_one(t: str, token_map) -> str:
    t = t.strip()
    if not t:
        return t
    if t.startswith("("):
        depth = 0
        for i, c in enumerate(t):
            if c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0:
                    inner = t[1:i]
                    rest = t[i + 1 :]
                    return "(" + _normalize_inner(inner, token_map) + ")" + rest
        return t
    if "[" in t:
        base, _, suffix = t.partition("[")
        return _normalize_one(base, token_map) + "[" + suffix
    return token_map(t)


def _sol_token_to_arc4(t: str) -> str:
    return _SOL_TO_ARC4_SCALAR.get(t, t)


def _split_top_level(s: str) -> list[str]:
    out, depth, buf = [], 0, []
    for ch in s:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append("".join(buf).strip())
            buf = []
        else:
            buf.append(ch)
    if buf:
        out.append("".join(buf).strip())
    return out


def call(
    localnet,
    app,
    sig: str,
    args: tuple = (),
    *,
    payment_wei: int = 0,
    expect_revert: bool = False,
    extra_fee: int = 0,
    extra_apps: list[int] | None = None,
    extra_accounts: list[str] | None = None,
    boxes: list[tuple[int, bytes]] | None = None,
) -> Result:
    """Call a Solidity ABI method on a deployed app.

    payment_wei: prepend a payment txn of this amount to the app address.
        Mirrors `msg.value` (in microAlgos on AVM).
    expect_revert: if True, the call is simulated (not submitted) so we
        can capture revert details from the failing txn's logs/message.
    extra_fee: additional flat fee (microAlgos) on top of the default
        2× min-fee for the app call.

    Method lookup:
      1. exact name + ARC4-normalized signature
      2. unique name fallback
      3. else: signal "method missing" — for expect_revert=True we send a
         synthetic 4-byte selector instead so the app's router reverts;
         otherwise CallError.
    """
    algod = localnet.algod
    sender = localnet.account.address
    signer = AccountTransactionSigner(localnet.account.private_key)

    abi_method = _resolve_method(app.app_spec, sig)
    if abi_method is None:
        if not expect_revert:
            raise CallError(f"method not found in app_spec: {sig}")
        # The method doesn't exist on the deployed app — submit a raw call
        # with a synthetic selector so the router runs and reverts.
        import hashlib
        selector = hashlib.sha3_256(sig.encode()).digest()[:4]
        return _raw_call_inner(
            algod, app, sender, signer, selector,
            extra_args=(), payment_wei=payment_wei,
            extra_fee=extra_fee, expect_revert=True,
        )

    encoded_args = _encode_args(abi_method, args)
    atc = _build_atc(
        algod=algod,
        app=app,
        sender=sender,
        signer=signer,
        abi_method=abi_method,
        encoded_args=encoded_args,
        payment_wei=payment_wei,
        extra_fee=extra_fee,
        extra_apps=extra_apps,
        extra_accounts=extra_accounts,
        boxes=boxes,
    )

    if expect_revert:
        return _simulate_for_revert(algod, atc)

    try:
        atc = au.populate_app_call_resources(atc, algod)
    except Exception:
        pass

    try:
        resp = atc.execute(algod, wait_rounds=4)
        abi_return = resp.abi_results[-1].return_value if resp.abi_results else None
        logs = _collect_logs(resp)
        return Result(abi_return=abi_return, logs=logs, raw_response=resp)
    except Exception as e:
        # Budget-exhausted? Retry with a pool of dummy helper-app calls in
        # the same group (each contributes 700 opcodes via fee pooling).
        if _is_budget_error(e):
            retry_result = _retry_with_budget_pool(
                algod=algod,
                localnet=localnet,
                app=app,
                sender=sender,
                signer=signer,
                abi_method=abi_method,
                encoded_args=encoded_args,
                payment_wei=payment_wei,
                extra_fee=extra_fee,
                extra_apps=extra_apps,
                extra_accounts=extra_accounts,
                boxes=boxes,
            )
            if retry_result is not None:
                return retry_result

        # Re-simulate to extract revert info, using a fresh ATC.
        sim_atc = _build_atc(
            algod=algod,
            app=app,
            sender=sender,
            signer=signer,
            abi_method=abi_method,
            encoded_args=encoded_args,
            payment_wei=payment_wei,
            extra_fee=extra_fee,
            extra_apps=extra_apps,
            extra_accounts=extra_accounts,
            boxes=boxes,
        )
        sim_result = _simulate_for_revert(algod, sim_atc)
        sim_result.raw_response = str(e)
        return sim_result


def _is_budget_error(exc: Exception) -> bool:
    msg = str(exc).lower()
    return "budget" in msg or "opcode" in msg or "dynamic cost" in msg


def _retry_with_budget_pool(
    *,
    algod, localnet, app, sender, signer, abi_method, encoded_args,
    payment_wei, extra_fee, extra_apps, extra_accounts, boxes,
    pool_size: int = 15,
):
    """Retry the call with `pool_size` dummy budget-helper calls in the same group.

    Each helper call at min-fee contributes 700 extra opcode budget. With
    pool_size=15 we get 15 × 700 ≈ 10,500 extra opcodes — enough for most
    EVM-translated arithmetic-heavy contracts.

    Returns a Result on success, None if retry also failed (caller falls
    back to the simulate path so the failure detail surfaces).
    """
    try:
        helper_id = localnet.budget_helper_id
    except Exception:
        return None

    sp_call = algod.suggested_params()
    sp_call.flat_fee = True
    sp_call.fee = 1000 * (pool_size + 2) + extra_fee  # +2: header + helper-call fee accounting

    atc = AtomicTransactionComposer()
    if payment_wei > 0:
        sp_pay = algod.suggested_params()
        sp_pay.flat_fee = True
        sp_pay.fee = 1000
        atc.add_transaction(
            TransactionWithSigner(
                PaymentTxn(sender, sp_pay, app.app_addr, payment_wei), signer
            )
        )

    atc.add_method_call(
        app_id=app.app_id,
        method=abi_method,
        sender=sender,
        sp=sp_call,
        signer=signer,
        method_args=encoded_args,
        note=os.urandom(8),
        foreign_apps=extra_apps,
        accounts=extra_accounts,
        boxes=boxes,
    )

    for _ in range(pool_size):
        sp_dummy = algod.suggested_params()
        sp_dummy.flat_fee = True
        sp_dummy.fee = 0  # paid from sp_call.fee pool
        atc.add_transaction(
            TransactionWithSigner(
                ApplicationCallTxn(
                    sender=sender, sp=sp_dummy, index=helper_id,
                    on_complete=OnComplete.NoOpOC, note=os.urandom(8),
                ),
                signer,
            )
        )

    try:
        atc = au.populate_app_call_resources(atc, algod)
    except Exception:
        pass

    try:
        resp = atc.execute(algod, wait_rounds=4)
    except Exception:
        return None

    abi_return = resp.abi_results[-1].return_value if resp.abi_results else None
    logs = _collect_logs(resp)
    return Result(abi_return=abi_return, logs=logs, raw_response=resp)


def call_raw(
    localnet,
    app,
    selector: bytes | None,
    *,
    extra_args: tuple = (),
    payment_wei: int = 0,
    expect_revert: bool = False,
    extra_fee: int = 0,
) -> Result:
    """Call an app with a raw 4-byte selector (bypasses ABI method dispatch).

    Use for: fallback dispatch tests, allowNonExistingFunctions cases,
    or hand-crafted calldata where you control the byte layout.

    Pass selector=None to make a bare call (NumAppArgs == 0). The
    AVM-side router treats this as the receive()/fallback() entry path.
    """
    algod = localnet.algod
    sender = localnet.account.address
    signer = AccountTransactionSigner(localnet.account.private_key)
    return _raw_call_inner(
        algod, app, sender, signer, selector,
        extra_args=extra_args, payment_wei=payment_wei,
        extra_fee=extra_fee, expect_revert=expect_revert,
    )


def _raw_call_inner(
    algod, app, sender, signer, selector: bytes | None, *,
    extra_args: tuple, payment_wei: int,
    extra_fee: int, expect_revert: bool,
) -> Result:
    sp = algod.suggested_params()
    sp.flat_fee = True
    sp.fee = 2000 + extra_fee

    # selector=None means a bare call with no app_args (NumAppArgs == 0)
    # — used to invoke Solidity `receive()`/`fallback()` dispatch.
    if selector is None:
        app_args = list(extra_args) or None
    else:
        app_args = [selector] + list(extra_args)
    txns = []
    if payment_wei > 0:
        sp_pay = algod.suggested_params()
        sp_pay.flat_fee = True
        sp_pay.fee = 1000
        txns.append(
            TransactionWithSigner(
                PaymentTxn(sender, sp_pay, app.app_addr, payment_wei), signer
            )
        )
    app_txn = ApplicationCallTxn(
        sender=sender,
        sp=sp,
        index=app.app_id,
        on_complete=OnComplete.NoOpOC,
        app_args=app_args,
        note=os.urandom(8),
    )
    txns.append(TransactionWithSigner(app_txn, signer))

    def _new_atc():
        a = AtomicTransactionComposer()
        for t in txns:
            a.add_transaction(t)
        return a

    if expect_revert:
        return _simulate_for_revert(algod, _new_atc())
    try:
        atc = _new_atc()
        # Populate box refs / foreign apps via simulate, matching the
        # ABI-call path so fallback contracts with boxes work.
        try:
            atc = au.populate_app_call_resources(atc, algod)
        except Exception:
            pass
        atc.execute(algod, wait_rounds=4)
        return Result()
    except Exception as e:
        sim_result = _simulate_for_revert(algod, _new_atc())
        sim_result.raw_response = str(e)
        return sim_result


def _build_atc(
    *,
    algod,
    app,
    sender,
    signer,
    abi_method,
    encoded_args,
    payment_wei,
    extra_fee,
    extra_apps,
    extra_accounts,
    boxes,
):
    atc = AtomicTransactionComposer()
    if payment_wei > 0:
        sp_pay = algod.suggested_params()
        sp_pay.flat_fee = True
        sp_pay.fee = 1000
        atc.add_transaction(
            TransactionWithSigner(
                PaymentTxn(sender, sp_pay, app.app_addr, payment_wei), signer
            )
        )
    sp_call = algod.suggested_params()
    sp_call.flat_fee = True
    # Default fee = 5× min — enough headroom for puya-emitted opup itxns and
    # most inner-contract creation/call patterns. Tests can override via
    # `extra_fee=`.
    sp_call.fee = 5000 + extra_fee
    atc.add_method_call(
        app_id=app.app_id,
        method=abi_method,
        sender=sender,
        sp=sp_call,
        signer=signer,
        method_args=encoded_args,
        note=os.urandom(8),
        foreign_apps=extra_apps,
        accounts=extra_accounts,
        boxes=boxes,
    )
    return atc


def _encode_args(abi_method, args: tuple) -> list:
    """Forward args verbatim. One algosdk quirk: byte arrays must come in
    as `list[int]`, not `bytes`. Convert that one shape; everything else
    is the test's responsibility — use the helpers in `framework.values`
    (or pass the right Python type for the ABI signature).
    """
    out: list = []
    for spec, val in zip(abi_method.args, args):
        t = str(spec.type)
        if (t == "byte[]" or t.startswith("byte[")) and isinstance(val, (bytes, bytearray)):
            out.append(list(val))
        else:
            out.append(val)
    return out


def _collect_logs(resp) -> list[bytes]:
    logs: list[bytes] = []
    for tx_info in getattr(resp, "tx_info", None) or []:
        for log in tx_info.get("logs", []):
            logs.append(encoding.base64.b64decode(log))
    return logs


def _simulate_for_revert(algod, atc) -> Result:
    """Run the ATC via simulate and return a Result reflecting revert details.

    The Result is .reverted=True if any txn failed. revert_data is taken
    from the failing txn's last log entry (puya-sol's revert path emits
    revert bytes via `log` before raising); revert_reason is classified.
    """
    try:
        sim_req = SimulateRequest(
            txn_groups=[],  # ATC.simulate overwrites this with its own group
            allow_more_logs=True,
            allow_empty_signatures=True,
            allow_unnamed_resources=True,
            extra_opcode_budget=170_000,
        )
        sim_resp = atc.simulate(algod, sim_req)
    except Exception as e:
        return Result(reverted=True, fail_message=str(e)[:300])

    raw = getattr(sim_resp, "simulate_response", None) or sim_resp
    if isinstance(raw, dict):
        groups = raw.get("txn-groups", [])
    else:
        groups = []

    revert_data = b""
    fail_msg = ""
    abi_return = None
    logs: list[bytes] = []
    reverted = False
    for g in groups:
        # Group-level failure is where simulate surfaces the reverting txn.
        gmsg = g.get("failure-message", "")
        if gmsg:
            reverted = True
            fail_msg = gmsg
        for tres in g.get("txn-results", []):
            # Per-txn failure-message exists in some response shapes too.
            tmsg = tres.get("failure-message", "")
            if tmsg:
                reverted = True
                fail_msg = tmsg
            txn = tres.get("txn-result", {})
            tlogs = txn.get("logs", []) or []
            decoded_logs = [encoding.base64.b64decode(L) for L in tlogs]
            if decoded_logs:
                logs.extend(decoded_logs)
        # After all txns, the last log of the failing txn is the revert data.
        if reverted and logs and not revert_data:
            revert_data = logs[-1]
    # Get ABI return from method_results if not reverted
    if not reverted:
        try:
            mres = sim_resp.abi_results
            if mres:
                abi_return = mres[-1].return_value
        except Exception:
            pass

    reason = classify_revert(revert_data) if reverted else None
    return Result(
        abi_return=abi_return,
        logs=logs,
        reverted=reverted,
        revert_data=revert_data,
        revert_reason=reason,
        fail_message=fail_msg,
    )
