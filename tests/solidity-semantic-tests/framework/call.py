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

# App ids that proved to need populate_app_call_resources (box-backed storage,
# inner app calls). First call on an app is submitted optimistically without
# the populate SIMULATE; if it fails and the populated retry succeeds, the app
# lands here so subsequent calls populate up front. Session-lifetime is fine:
# app ids are unique per localnet session.
_needs_populate: set[int] = set()

# Diagnostics: the resource-populate/simulate machinery has several layered
# retries whose failures used to be silently swallowed — record the last ones
# so a final failure can name the REAL first cause.
_LAST_POPULATE_ERROR: str = ""
_LAST_POOL_EXEC_ERROR: str = ""


@dataclass
class Result:
    abi_return: Any = None
    logs: list[bytes] = field(default_factory=list)
    reverted: bool = False
    revert_data: bytes = b""
    revert_reason: Reverted | ErrorString | Panic | RawRevert | None = None
    raw_response: Any = None  # algosdk return for tests that need the raw bytes
    fail_message: str = ""

    def __post_init__(self):
        # A reverted call has abi_return=None, so a test doing as_int(r.abi_return)
        # fails with "can't coerce NoneType" and hides the revert entirely. Record
        # the reason so as_int can name the real cause (see values.last_revert).
        if self.reverted:
            import framework.values as _v

            # An empty RawRevert carries no information — prefer the AVM message,
            # which names the failing pc/opcodes.
            reason = str(self.revert_reason or "")
            if not self.revert_data and self.fail_message:
                reason = self.fail_message
            _v._LAST_REVERT = reason or "<no reason captured>"


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

    # OVERLOADS: exact type-matching above can fail because puya-sol PROMOTES
    # signed types in the ARC-4 signature (Solidity `int16` -> arc4 `uint64` param
    # / `uint256` return) — a mapping `_sol_token_to_arc4` does not model. The
    # single-candidate fallback rescues non-overloaded methods, but an OVERLOADED
    # signed method resolved to None, and the caller then submitted a SYNTHETIC
    # selector that hit the router's `err` — a resolution failure masquerading as a
    # contract REVERT, i.e. a phantom "compiler bug". (The overnight fuzzer reported
    # exactly this on inheritance/super_overload mutants; the compiler was correct.)
    # Overloads differ by ARITY in practice, so disambiguate on arg count.
    if has_paren:
        want_args = _split_top_level(target_inner) if target_inner.strip() else []
        by_arity = [m for m in candidates if len(m.to_abi_method().args) == len(want_args)]
        if len(by_arity) == 1:
            return by_arity[0].to_abi_method()
        # Still ambiguous (e.g. deposit(address,int240) vs deposit(address,bool)):
        # match each arg by CATEGORY (integer / bool / address / bytes / tuple),
        # which survives the signed->unsigned promotion the exact matcher trips on.
        if len(by_arity) > 1:
            want_cats = [_type_category(t) for t in want_args]
            by_cat = [
                m for m in by_arity
                if [_type_category(str(a.type)) for a in m.to_abi_method().args] == want_cats
            ]
            if len(by_cat) == 1:
                return by_cat[0].to_abi_method()
    return None


def _type_category(t: str) -> str:
    """Broad type category, ignoring sign/width — for overload resolution that
    must survive puya-sol's signed->unsigned ARC-4 promotion."""
    t = t.strip()
    if t.startswith("("):
        return "tuple"
    if t.endswith("]"):
        return "array"
    if t == "bool":
        return "bool"
    if t == "address":
        return "address"
    if t == "string" or t.startswith("byte") or t.startswith("bytes"):
        return "bytes"
    if t.startswith(("uint", "int", "ufixed", "fixed")):
        return "integer"
    return t


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
    budget_pool: int = 0,
) -> Result:
    """Call a Solidity ABI method on a deployed app.

    payment_wei: prepend a payment txn of this amount to the app address.
        Mirrors `msg.value` (in microAlgos on AVM).
    expect_revert: if True, the call is simulated (not submitted) so we
        can capture revert details from the failing txn's logs/message.
    extra_fee: additional flat fee (microAlgos) on top of the default
        2× min-fee for the app call.
    budget_pool: prepend N dummy budget-helper app calls to the group,
        each contributing 700 extra opcode budget via fee pooling. Use
        when the call is known to exceed the default 700-op budget by a
        large margin (auto-retry handles smaller overshoots).

    Method lookup:
      1. exact name + ARC4-normalized signature
      2. unique name fallback
      3. else: signal "method missing" — for expect_revert=True we send a
         synthetic 4-byte selector instead so the app's router reverts;
         otherwise CallError.
    """
    global _LAST_POPULATE_ERROR
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

    # If caller pre-allocated a budget pool, use the retry helper directly.
    if budget_pool > 0:
        result = _retry_with_budget_pool(
            algod=algod, localnet=localnet, app=app,
            sender=sender, signer=signer, abi_method=abi_method,
            encoded_args=encoded_args, payment_wei=payment_wei,
            extra_fee=extra_fee, extra_apps=extra_apps,
            extra_accounts=extra_accounts, boxes=boxes,
            pool_size=budget_pool,
        )
        if result is not None:
            return result
        # Fall through to plain call if pool execution failed.

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

    # populate_app_call_resources costs a full SIMULATE round-trip per call, and
    # most calls need no extra resources. Populate only when the caller passed
    # explicit resources or this app previously proved to need auto-discovery
    # (box-backed storage, inner app calls); otherwise submit optimistically and
    # retry once WITH population on failure — a rejected txn commits nothing, so
    # the retry is safe, and success adds the app to the memo so later calls on
    # it skip the failed probe.
    populated = bool(boxes or extra_apps or extra_accounts) or app.app_id in _needs_populate
    if populated:
        try:
            atc = au.populate_app_call_resources(atc, algod)
        except Exception as pe:
            _LAST_POPULATE_ERROR = f"populate: {pe}"

    try:
        resp = atc.execute(algod, wait_rounds=4)
        abi_return = resp.abi_results[-1].return_value if resp.abi_results else None
        logs = _collect_logs(resp)
        return Result(abi_return=abi_return, logs=logs, raw_response=resp)
    except Exception as e:
        if not populated:
            retry_atc = _build_atc(
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
            try:
                retry_atc = au.populate_app_call_resources(retry_atc, algod)
            except Exception as pe:
                _LAST_POPULATE_ERROR = f"retry-populate: {pe}"
            try:
                resp = retry_atc.execute(algod, wait_rounds=4)
                _needs_populate.add(app.app_id)
                abi_return = resp.abi_results[-1].return_value if resp.abi_results else None
                logs = _collect_logs(resp)
                return Result(abi_return=abi_return, logs=logs, raw_response=resp)
            except Exception as e2:
                e = e2  # genuine failure — flow into budget/revert handling below

        # Budget-exhausted? Retry with a pool of dummy helper-app calls in
        # the same group (each contributes 700 opcodes via fee pooling).
        if _is_budget_error(e) or _is_resource_error(e):
            for _amp in (False, True):
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
                    amplify=_amp,
                )
                if retry_result is not None:
                    return retry_result

        # Min-balance shortfall (slot mode: 4-ALGO child grants + created-app
        # params MBR ratchet the creator's account): top up and retry once.
        if _try_minbalance_topup(algod, localnet, e):
            mb_atc = _build_atc(
                algod=algod, app=app, sender=sender, signer=signer,
                abi_method=abi_method, encoded_args=encoded_args,
                payment_wei=payment_wei, extra_fee=extra_fee,
                extra_apps=extra_apps, extra_accounts=extra_accounts,
                boxes=boxes,
            )
            try:
                mb_atc = au.populate_app_call_resources(mb_atc, algod)
            except Exception as pe:
                _LAST_POPULATE_ERROR = f"minbal-populate: {pe}"
            try:
                resp = mb_atc.execute(algod, wait_rounds=4)
                abi_return = resp.abi_results[-1].return_value if resp.abi_results else None
                return Result(abi_return=abi_return, logs=_collect_logs(resp), raw_response=resp)
            except Exception as e2:
                e = e2  # flow into the fee/revert handling below

        # Fee shortfall: bump the outer fee by the reported need (+1 margin)
        # and resubmit once. "(need NmA)" is algod's milliAlgo phrasing.
        if _is_fee_error(e):
            import re as _re
            m = _re.search(r"need (\d+)mA", str(e))
            bump = (int(m.group(1)) + 1) * 1000 if m else 4000
            fee_atc = _build_atc(
                algod=algod, app=app, sender=sender, signer=signer,
                abi_method=abi_method, encoded_args=encoded_args,
                payment_wei=payment_wei, extra_fee=extra_fee + bump,
                extra_apps=extra_apps, extra_accounts=extra_accounts,
                boxes=boxes,
            )
            try:
                fee_atc = au.populate_app_call_resources(fee_atc, algod)
            except Exception as pe:
                _LAST_POPULATE_ERROR = f"fee-retry-populate: {pe}"
            try:
                resp = fee_atc.execute(algod, wait_rounds=4)
                abi_return = resp.abi_results[-1].return_value if resp.abi_results else None
                return Result(abi_return=abi_return, logs=_collect_logs(resp), raw_response=resp)
            except Exception as e2:
                e = e2  # keep the escalated failure for the revert path

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
        # Diagnosability: a test that only asserts on abi_return surfaces this
        # failure as an opaque `as_int(None)` TypeError; the REAL error lives
        # in fail_message. Print it — pytest captures stdout and shows it in
        # the failure report, so -n2 flakes self-diagnose from gate logs.
        print(f"[call] {getattr(abi_method, 'name', '?')} FAILED after retries: "
              f"{str(e)[:300]}")
        if not sim_result.reverted and not _method_is_readonly(app, abi_method):
            # The SIMULATION succeeds (simulate allows unnamed resources and pooled
            # budget the real txn group can't carry) while the actual submission
            # failed. For a READONLY method that's a legitimate read — nothing to
            # commit. For a MUTATOR it's a FALSE NEGATIVE: a "successful" call that
            # committed nothing (seen as `setLengths(255,0) -> ok` with the array
            # silently unchanged). Surface the submit failure for mutators.
            return Result(reverted=True, fail_message=str(e), raw_response=str(e))
        return sim_result


def _method_is_readonly(app, abi_method) -> bool:
    """arc56 `readonly` flag (view/pure) for the resolved method, by signature."""
    try:
        sig = abi_method.get_signature()
        for m in app.app_spec.methods:
            if m.to_abi_method().get_signature() == sig:
                return bool(getattr(m, "readonly", False))
    except Exception:
        pass
    return False


def _is_budget_error(exc: Exception) -> bool:
    msg = str(exc).lower()
    return "budget" in msg or "opcode" in msg or "dynamic cost" in msg


def _is_fee_error(exc: Exception) -> bool:
    """Inner-txn fee shortfall: `new C()` emits create (+ forced __postInit in
    slot mode) inner txns at fee 0; pooled outer fee must cover them. Tests
    don't model fees, so escalate instead of failing (EVM has no analogue)."""
    msg = str(exc).lower()
    return "fee" in msg and "too small" in msg


def _is_resource_error(exc: Exception) -> bool:
    """Reference-array overflow: >8 foreign refs per txn. The budget-pool retry
    group (16 txns) also multiplies reference capacity (8 per txn), so the same
    retry fixes it — e.g. box-per-slot storage touching 10+ slots in one call."""
    msg = str(exc).lower()
    return ("invalid box reference" in msg or "unavailable box" in msg
            or "invalid app reference" in msg or "unavailable resource" in msg
            or "unavailable account" in msg)


def _try_minbalance_topup(algod, localnet, exc: Exception) -> bool:
    """Top up an app account whose min balance outgrew its funds and say so.

    Slot mode compounds this: every `new C()` grants the child 4 ALGO AND
    permanently raises the creator's own min balance (created-app params MBR),
    so an app creating several children drains below min mid-test. Tests don't
    model balances, and EVM has no MBR analogue — auto-fund instead of failing.
    Parses `account <addr> balance <B> below min <M>` / `tried to spend <N>`
    from algod's error text; pays shortfall + slack to <addr>. Returns True if
    a payment was sent (caller should retry once).
    """
    import re as _re
    from algosdk.transaction import PaymentTxn, wait_for_confirmation
    msg = str(exc)
    m = _re.search(r"account ([A-Z2-7]{58}).{0,120}?balance (\d+) below min (\d+)", msg)
    amount = None
    addr = None
    if m:
        addr = m.group(1)
        amount = int(m.group(3)) - int(m.group(2)) + 8_000_000
    else:
        m = _re.search(r"overspend \(account ([A-Z2-7]{58})", msg)
        if m:
            addr = m.group(1)
            amount = 12_000_000
    if not addr or not amount or amount <= 0:
        return False
    try:
        sp = algod.suggested_params()
        pay = PaymentTxn(localnet.account.address, sp, addr, amount, note=os.urandom(8))
        wait_for_confirmation(
            algod, algod.send_transaction(pay.sign(localnet.account.private_key)), 4)
        return True
    except Exception:
        return False


def _retry_with_budget_pool(
    *,
    algod, localnet, app, sender, signer, abi_method, encoded_args,
    payment_wei, extra_fee, extra_apps, extra_accounts, boxes,
    pool_size: int = 15,
    amplify: bool = False,
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
        target_id = localnet.budget_target_id
    except Exception:
        return None

    # The group holds the call, the optional payment, and the helpers, and
    # MAX_GROUP_SIZE is 16. At the default 15 the no-payment case sits exactly
    # on the ceiling, so a `msg.value` payment made it 17 and the composer
    # rejected the whole group — surfacing as "cannot exceed MAX_GROUP_SIZE"
    # and, because the txn then ran on the EVM leg only, as phantom storage
    # divergences downstream. Costs 700 opcodes of headroom on payable calls.
    _MAX_GROUP = 16
    pool_size = min(pool_size,
                    _MAX_GROUP - 1 - (1 if payment_wei > 0 else 0))

    # Each helper call amplifies via _OPUP_DEPTH inner self-calls (+700 budget
    # each), so the group ceiling is pool_size*(1+_OPUP_DEPTH)*700 ≈ 94k ops.
    # The outer fee must cover every inner txn too.
    # Two configs (caller tries plain first, amplified second):
    #  - plain:     bare dummies — every ref slot free (15×8=120) for populate
    #               to spread NAMED box refs + duplicates (I/O-heavy calls);
    #               budget ceiling 15×700 ≈ 11k ops.
    #  - amplified: each dummy issues _OPUP_DEPTH inner calls to the target
    #               (+700 each, ceiling ≈ 95k ops) and carries 2 EMPTY box
    #               refs (creation quota for boxes of apps created IN-group —
    #               `new C()`'s __postInit box_create; no named ref can exist
    #               pre-submit). Costs ref slots — hence not the first try.
    _OPUP_DEPTH = 8 if amplify else 0
    sp_call = algod.suggested_params()
    sp_call.flat_fee = True
    # + 16k headroom: the app under test may emit inner txns of its own
    # (`new C()` = create + __postInit in slot mode) — without this the group
    # is exactly their fees short ("group fee 0.0A too small (need 1mA)").
    sp_call.fee = (1000 * (pool_size + 2) + extra_fee
                   + 1000 * pool_size * _OPUP_DEPTH + 16_000)

    atc = AtomicTransactionComposer()
    # Dummies FIRST: pooled budget is sequential — an amplifier's OpUp inner
    # calls only add budget once its txn has EXECUTED, so a main call at index
    # 0 sees just 700×group ≈ 11k no matter the OpUp depth. Helpers up front
    # accrue ≈ pool×depth×700 before the main program runs. The payment stays
    # IMMEDIATELY before the app call (payable check reads gtxns[GroupIndex-1]).
    for _ in range(pool_size):
        sp_dummy = algod.suggested_params()
        sp_dummy.flat_fee = True
        sp_dummy.fee = 0  # paid from sp_call.fee pool
        atc.add_transaction(
            TransactionWithSigner(
                ApplicationCallTxn(
                    sender=sender, sp=sp_dummy, index=helper_id,
                    on_complete=OnComplete.NoOpOC, note=os.urandom(8),
                    app_args=[_OPUP_DEPTH.to_bytes(8, "big")] if amplify else None,
                    foreign_apps=[target_id] if amplify else None,
                    boxes=[(0, b"")] * 2 if amplify else None,
                ),
                signer,
            )
        )

    if payment_wei > 0:
        sp_pay = algod.suggested_params()
        sp_pay.flat_fee = True
        sp_pay.fee = 1000
        atc.add_transaction(
            TransactionWithSigner(
                PaymentTxn(sender, sp_pay, app.app_addr, payment_wei, note=os.urandom(8)), signer
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

    try:
        atc = au.populate_app_call_resources(atc, algod)
    except Exception as pe:
        global _LAST_POPULATE_ERROR
        _LAST_POPULATE_ERROR = f"pool-populate: {pe}"

    try:
        resp = atc.execute(algod, wait_rounds=4)
    except Exception as ee:
        global _LAST_POOL_EXEC_ERROR
        _LAST_POOL_EXEC_ERROR = f"pool-exec: {ee}"
        return None

    abi_return = resp.abi_results[-1].return_value if resp.abi_results else None
    logs = _collect_logs(resp)
    return Result(abi_return=abi_return, logs=logs, raw_response=resp)


def _raw_pool_execute(
    algod, localnet, app, sender, signer, *,
    app_args, payment_wei, extra_fee, amplify, pool_size=15,
):
    """Raw-call twin of _retry_with_budget_pool (same dummy construction —
    plain round spreads refs, amplified round adds budget + creation quota).
    Returns a Result on success, None when this round also failed."""
    try:
        helper_id = localnet.budget_helper_id
        target_id = localnet.budget_target_id
    except Exception:
        return None
    _MAX_GROUP = 16
    pool_size = min(pool_size, _MAX_GROUP - 1 - (1 if payment_wei > 0 else 0))
    _OPUP_DEPTH = 8 if amplify else 0
    sp_call = algod.suggested_params()
    sp_call.flat_fee = True
    sp_call.fee = (1000 * (pool_size + 2) + extra_fee
                   + 1000 * pool_size * _OPUP_DEPTH + 16_000)
    atc = AtomicTransactionComposer()
    # Dummies FIRST (see _retry_with_budget_pool): OpUp budget must accrue
    # before the main call executes; payment stays adjacent to the app call.
    for _ in range(pool_size):
        sp_dummy = algod.suggested_params()
        sp_dummy.flat_fee = True
        sp_dummy.fee = 0
        atc.add_transaction(TransactionWithSigner(
            ApplicationCallTxn(
                sender=sender, sp=sp_dummy, index=helper_id,
                on_complete=OnComplete.NoOpOC, note=os.urandom(8),
                app_args=[_OPUP_DEPTH.to_bytes(8, "big")] if amplify else None,
                foreign_apps=[target_id] if amplify else None,
                boxes=[(0, b"")] * 2 if amplify else None),
            signer))
    if payment_wei > 0:
        sp_pay = algod.suggested_params()
        sp_pay.flat_fee = True
        sp_pay.fee = 1000
        atc.add_transaction(TransactionWithSigner(
            PaymentTxn(sender, sp_pay, app.app_addr, payment_wei,
                       note=os.urandom(8)), signer))
    atc.add_transaction(TransactionWithSigner(
        ApplicationCallTxn(
            sender=sender, sp=sp_call, index=app.app_id,
            on_complete=OnComplete.NoOpOC, app_args=app_args,
            note=os.urandom(8)),
        signer))
    global _LAST_POPULATE_ERROR, _LAST_POOL_EXEC_ERROR
    try:
        atc = au.populate_app_call_resources(atc, algod)
    except Exception as pe:
        _LAST_POPULATE_ERROR = f"raw-pool-populate: {pe}"
    try:
        atc.execute(algod, wait_rounds=4)
        return Result()
    except Exception as ee:
        _LAST_POOL_EXEC_ERROR = f"raw-pool-exec: {ee}"
        return None


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
        localnet=localnet,
    )


def _raw_call_inner(
    algod, app, sender, signer, selector: bytes | None, *,
    extra_args: tuple, payment_wei: int,
    extra_fee: int, expect_revert: bool,
    localnet=None,
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
                PaymentTxn(sender, sp_pay, app.app_addr, payment_wei, note=os.urandom(8)), signer
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
    # Same optimistic-populate strategy as the ABI path: skip the per-call
    # populate SIMULATE unless this app is known to need resource discovery;
    # on failure retry once populated (fallback contracts with boxes).
    global _LAST_POPULATE_ERROR
    populated = app.app_id in _needs_populate
    try:
        atc = _new_atc()
        if populated:
            try:
                atc = au.populate_app_call_resources(atc, algod)
            except Exception as pe:
                _LAST_POPULATE_ERROR = f"raw-populate: {pe}"
        atc.execute(algod, wait_rounds=4)
        return Result()
    except Exception as e:
        if not populated:
            try:
                retry_atc = au.populate_app_call_resources(_new_atc(), algod)
                retry_atc.execute(algod, wait_rounds=4)
                _needs_populate.add(app.app_id)
                return Result()
            except Exception as e2:
                e = e2
        # Budget/resource shortfall: same two-round dummy pool as the ABI
        # path (plain = max ref spread, amplified = opcode budget + creation
        # quota). Raw fallback calls WRITE boxes too — without this a
        # slot-mode `fallback() { savedData = msg.data; }` lost its write.
        if localnet is not None and (_is_budget_error(e) or _is_resource_error(e)):
            for _amp in (False, True):
                pooled = _raw_pool_execute(
                    algod, localnet, app, sender, signer,
                    app_args=app_args, payment_wei=payment_wei,
                    extra_fee=extra_fee, amplify=_amp)
                if pooled is not None:
                    _needs_populate.add(app.app_id)
                    return pooled
        sim_result = _simulate_for_revert(algod, _new_atc())
        sim_result.raw_response = str(e)
        if not sim_result.reverted:
            # The SIMULATE succeeds (unnamed resources + pooled budget the real
            # group didn't carry) while the submit failed. Raw calls exist to
            # exercise fallback/receive dispatch, which almost always WRITES —
            # reporting success here silently drops the write (the ABI path
            # gained this honesty check long ago; this twin never did).
            return Result(reverted=True, fail_message=str(e), raw_response=str(e))
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
                PaymentTxn(sender, sp_pay, app.app_addr, payment_wei, note=os.urandom(8)), signer
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


# ── EVM ABI decoder for the isoltest raw-words calling convention ──────────
# Upstream isoltest fixtures spell a call as the raw EVM calldata tail — N
# 32-byte words — e.g. `f(uint256,bytes,uint256): 7, 0x60, 8, 2, 0`. The EVM
# receives those bytes and ABI-DECODES them into typed params; our AVM contract
# likewise receives the decoded params (which puya re-encodes canonically). So
# to call the AVM faithfully we must EVM-decode the words into Python values and
# hand THOSE to algosdk (which ARC4-encodes them for the app). algosdk's own
# decoder is ARC4, not EVM ABI (dynamic layouts differ), so we decode here.

def _parse_abi_type(s: str):
    """Parse an ABI type string into a small tree. Tuples: (uint256,bytes).
    algosdk normalizes Solidity `bytes`→`byte[]` and `bytesN`→`byte[N]`."""
    s = s.strip()
    if s == "byte[]":
        return ("bytes",)
    if s in ("byte", "byte[1]"):
        return ("bytesN", 1)
    if s.startswith("byte[") and s.endswith("]") and s[5:-1].isdigit():
        return ("bytesN", int(s[5:-1]))
    if s.endswith("]"):
        i = s.rindex("[")
        inner, dim = s[:i], s[i + 1 : -1]
        elem = _parse_abi_type(inner)
        return ("sarray", elem, int(dim)) if dim else ("darray", elem)
    if s.startswith("(") and s.endswith(")"):
        return ("tuple", _split_tuple(s[1:-1]))
    if s in ("bytes", "byte[]"):
        return ("bytes",)
    if s == "string":
        return ("string",)
    if s == "bool":
        return ("bool",)
    if s == "address":
        return ("address",)
    if s.startswith("uint") or s.startswith("int"):
        return ("uint" if s.startswith("uint") else "sint", int(s[s.index("t") + 1 :] or 256))
    if s.startswith("bytes"):
        return ("bytesN", int(s[5:]))
    raise ValueError(f"unhandled ABI type {s!r}")


def _split_tuple(s: str) -> list:
    """Split a tuple body on top-level commas (respecting nested parens)."""
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch == "," and depth == 0:
            out.append(_parse_abi_type(cur)); cur = ""; continue
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        cur += ch
    if cur.strip():
        out.append(_parse_abi_type(cur))
    return out


def _evm_is_dynamic(t) -> bool:
    k = t[0]
    if k in ("bytes", "string", "darray"):
        return True
    if k == "sarray":
        return _evm_is_dynamic(t[1])
    if k == "tuple":
        return any(_evm_is_dynamic(c) for c in t[1])
    return False


def _evm_head_words(t) -> int:
    """32-byte words this type occupies in the head (1 pointer if dynamic)."""
    if _evm_is_dynamic(t):
        return 1
    if t[0] == "sarray":
        return t[2] * _evm_head_words(t[1])
    if t[0] == "tuple":
        return sum(_evm_head_words(c) for c in t[1])
    return 1


def _evm_decode(t, blob: bytes, base: int):
    """Decode a value of type t whose head starts at blob[base:]."""
    k = t[0]
    if k in ("uint", "sint"):
        v = int.from_bytes(blob[base : base + 32], "big")
        if k == "sint" and v >= 1 << (t[1] - 1):
            v -= 1 << t[1]
        return v
    if k == "bool":
        return bool(int.from_bytes(blob[base : base + 32], "big"))
    if k == "address":
        # AVM account = 32-byte public key; raw-word fixtures spell addresses as
        # small ints (EVM word = uint160 right-aligned). Hand algosdk the 32-byte
        # BE form (its address codec rejects bare ints) — matches the contract-side
        # address(uint160(x)) convention used across the suite.
        return blob[base : base + 32]
    if k == "bytesN":
        return list(blob[base : base + t[1]])
    if k == "tuple":
        vals, cur = [], base
        for c in t[1]:
            if _evm_is_dynamic(c):
                off = int.from_bytes(blob[cur : cur + 32], "big")
                vals.append(_evm_decode(c, blob[base:], off))
            else:
                vals.append(_evm_decode(c, blob, cur))
            cur += 32 * _evm_head_words(c)
        return vals
    if k == "sarray":
        elem, n = t[1], t[2]
        vals, cur = [], base
        for _ in range(n):
            if _evm_is_dynamic(elem):
                off = int.from_bytes(blob[cur : cur + 32], "big")
                vals.append(_evm_decode(elem, blob[base:], off))
            else:
                vals.append(_evm_decode(elem, blob, cur))
            cur += 32 * _evm_head_words(elem)
        return vals
    # dynamic: length word then payload, all relative to `base`
    ln = int.from_bytes(blob[base : base + 32], "big")
    body = base + 32
    if k in ("bytes", "string"):
        raw = blob[body : body + ln]
        return raw.decode() if k == "string" else list(raw)
    if k == "darray":
        elem = t[1]
        vals, cur = [], body
        for _ in range(ln):
            if _evm_is_dynamic(elem):
                off = int.from_bytes(blob[cur : cur + 32], "big")
                vals.append(_evm_decode(elem, blob[body:], off))
            else:
                vals.append(_evm_decode(elem, blob, cur))
            cur += 32 * _evm_head_words(elem)
        return vals
    raise ValueError(f"unhandled decode {t}")


def _looks_like_raw_words(abi_method, args) -> bool:
    """True when args are the isoltest raw EVM calldata words rather than typed
    values: all bare ints, and the signature isn't a plain list of single-word
    scalars matched 1:1 (which is an ordinary typed call)."""
    if not args or not all(isinstance(a, int) and not isinstance(a, bool) for a in args):
        return False
    types = [_parse_abi_type(str(a.type)) for a in abi_method.args]
    all_scalar = all(not _evm_is_dynamic(t) and _evm_head_words(t) == 1 for t in types)
    return not (all_scalar and len(args) == len(types))


def _decode_raw_words(abi_method, args) -> list:
    """EVM-decode raw calldata words into per-param Python values for algosdk."""
    blob = b"".join(int(a).to_bytes(32, "big") for a in args)
    tup = ("tuple", [_parse_abi_type(str(a.type)) for a in abi_method.args])
    return _evm_decode(tup, blob, 0)


def _encode_args(abi_method, args: tuple) -> list:
    """Forward args verbatim. One algosdk quirk: byte arrays must come in
    as `list[int]`, not `bytes`. Convert that one shape; everything else
    is the test's responsibility — use the helpers in `framework.values`
    (or pass the right Python type for the ABI signature).

    isoltest words convention: upstream fixtures call `f(bytes)` with the
    raw EVM calldata tail spelled as N 32-byte words (`f(bytes): 0x20,
    0xA0, ...`). When the method takes exactly one `byte[]` param and the
    test passed multiple bare ints, pack each into a 32-byte big-endian
    word, then strip the EVM head ([offset][length]) the same way the
    EVM decoder would — the function receives only the payload bytes.
    Falls back to the raw concatenation when the head doesn't parse.

    The general case (multi-param, structs, arrays) goes through the full
    EVM ABI decoder above (_decode_raw_words).
    """
    if (
        len(abi_method.args) == 1
        and str(abi_method.args[0].type) == "byte[]"
        and len(args) > 1
        and all(isinstance(a, int) and not isinstance(a, bool) for a in args)
    ):
        blob = b"".join(int(a).to_bytes(32, "big") for a in args)
        if len(blob) >= 64:
            off = int.from_bytes(blob[:32], "big")
            if off % 32 == 0 and off + 32 <= len(blob):
                ln = int.from_bytes(blob[off : off + 32], "big")
                if off + 32 + ln <= len(blob):
                    return [list(blob[off + 32 : off + 32 + ln])]
        return [list(blob)]

    # General isoltest raw-words convention: multi-param, structs, arrays.
    if _looks_like_raw_words(abi_method, args):
        try:
            return _decode_raw_words(abi_method, args)
        except Exception:
            pass  # not raw words after all — fall through to verbatim forward

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
        import os as _os
        if _os.environ.get("CHD_TAPE_DEBUG"):
            # dump EVERY log (outer + inner) of the failing group — the tape
            # stub's served answer (151f7c75-prefixed) is only visible here
            import base64 as _b64
            def _walk(tr, depth=0):
                for t in tr or []:
                    txn = (t.get("txn-result") or t).get("txn-results") or None
                    res = t.get("txn-result") or t
                    for lg in (res.get("logs") or []):
                        try:
                            print(f"[sim-log d{depth}] "
                                  + _b64.b64decode(lg).hex()[:100])
                        except Exception:
                            pass
                    _walk(res.get("inner-txns") or [], depth + 1)
            for g in groups:
                _walk(g.get("txn-results") or [])
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
