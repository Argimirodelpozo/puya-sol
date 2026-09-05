"""Deploy a compiled ARC56 contract to localnet."""
from __future__ import annotations

import os
import re
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

import algokit_utils as au
from algosdk import encoding
from algosdk.transaction import (
    ApplicationCreateTxn,
    ApplicationUpdateTxn,
    OnComplete,
    StateSchema,
    PaymentTxn,
    wait_for_confirmation,
)

if TYPE_CHECKING:
    from .localnet import LocalNet


class DeployError(Exception):
    pass


@dataclass
class DeployedApp:
    """Handle returned from `deploy()`. Carries enough state for the call API."""
    app_id: int
    app_addr: str
    app_spec: au.Arc56Contract
    client: au.AppClient
    balance_baseline: int
    ctor_fund_wei: int
    child_mbr: int = 1_000_000  # microAlgos per child app deployed by ctor


def _substitute_template_vars(
    teal: str, tmpl_path: Path, overrides: dict[str, int | str] | None = None
) -> str:
    """Replace TMPL_* placeholders in TEAL using puya's deploy.tmpl.json."""
    import json
    try:
        data = json.loads(tmpl_path.read_text())
    except (json.JSONDecodeError, OSError):
        data = {}
    data.update(overrides or {})
    # Puya's deploy.tmpl.json keys are already literal `TMPL_<name>` strings
    # (matching the placeholders in the TEAL bytecblock); replace verbatim.
    # Values are bare hex (no `0x` prefix) — the bytecblock pseudo-op expects
    # the prefix, so add it.
    #
    # Replace longest keys first: a key that is a prefix of another
    # (`TMPL_APPROVAL_C1` ⊂ `TMPL_APPROVAL_C10`) would otherwise corrupt the
    # longer placeholder — the trailing `0` of `C10` survives and lands on
    # the C1 value, producing an odd-length hex constant the assembler
    # rejects ("bytec N is not defined").
    for k, v in sorted(data.items(), key=lambda kv: -len(kv[0])):
        if isinstance(v, int):
            # Integer template variables (notably splitter helper app IDs)
            # appear in `pushint` positions and must stay decimal. Treating
            # 123 as bytecode hex (`0x123`) silently points at app 291.
            teal = teal.replace(k, str(v))
            continue
        s = str(v)
        if all(c in "0123456789abcdefABCDEF" for c in s):
            s = "0x" + s
        teal = teal.replace(k, s)
    return teal


def _scan_ctor_forwarded_value(approval_teal: Path) -> int:
    """Detect `new X{value: N}(...)` constructor patterns and return total N.

    Approximate but conservative: an inner txn creating an app with a
    pre-funding pay group is treated as the constructor forwarding value
    to a child. The amount is read from the pushint preceding the pay
    txn's Amount field.
    """
    # Conservative: we don't try to reconstruct ATC-emitted payment groups
    # in TEAL. The semantic-tests harness historically used 0 for almost
    # every test — return 0 by default. Tests that need a non-zero
    # baseline can override balance_baseline manually.
    return 0


def _load_arc56(arc56_path: Path) -> au.Arc56Contract:
    return au.Arc56Contract.from_json(arc56_path.read_text())


def compile_programs(
    localnet: "LocalNet",
    artifacts: dict,
    template_values: dict[str, int | str] | None = None,
) -> tuple[bytes, bytes]:
    """Compile one artifact pair after applying deploy-time templates."""
    algod = localnet.algod
    approval_src = _substitute_template_vars(
        artifacts["approval_teal"].read_text(),
        artifacts["approval_teal"].parent / "deploy.tmpl.json",
        template_values,
    )
    clear_src = _substitute_template_vars(
        artifacts["clear_teal"].read_text(),
        artifacts["clear_teal"].parent / "deploy.tmpl.json",
        template_values,
    )
    approval = encoding.base64.b64decode(algod.compile(approval_src)["result"])
    clear = encoding.base64.b64decode(algod.compile(clear_src)["result"])
    return approval, clear


def update_program(
    localnet: "LocalNet",
    app_id: int,
    artifacts: dict,
    template_values: dict[str, int | str] | None = None,
    account=None,
) -> None:
    """Install compiled artifacts on an existing app via UpdateApplication.

    The target app's approval program must expose the split pipeline's
    ``__delegate_update`` UpdateApplication route. Storage and application
    identity remain attached to ``app_id`` across the update.
    """
    import hashlib

    approval, clear = compile_programs(localnet, artifacts, template_values)
    page_size = 2048
    total = len(approval) + len(clear)
    if total > 8 * page_size:
        raise DeployError(
            f"program exceeds AVM 16KB cap: approval={len(approval)}B + "
            f"clear={len(clear)}B"
        )
    # Consensus v42 charges program I/O in both directions: the update reads
    # the currently installed program and writes the replacement. Installing
    # a tiny page over a 15 KiB main therefore still needs read budget. Empty
    # box refs grant 2 KiB to both pools without exposing an actual box.
    try:
        import base64
        params = localnet.algod.application_info(app_id)["params"]
        current_total = sum(len(base64.b64decode(params.get(field) or ""))
                            for field in ("approval-program",
                                          "clear-state-program"))
    except Exception:
        current_total = 8 * page_size
    charged = max(0, max(total, current_total) - 4 * page_size)
    program_budget_refs = (charged + page_size - 1) // page_size
    selector = hashlib.new("sha512_256", b"__delegate_update()void").digest()[:4]
    sp = localnet.algod.suggested_params()
    sp.flat_fee = True
    sp.fee = max(sp.min_fee, 1000) * 8
    signer_account = account or localnet.account
    txn = ApplicationUpdateTxn(
        sender=signer_account.address,
        sp=sp,
        index=app_id,
        approval_program=approval,
        clear_program=clear,
        app_args=[selector],
        boxes=[(0, b"")] * program_budget_refs,
        note=os.urandom(8),
    )
    try:
        txid = localnet.algod.send_transaction(
            txn.sign(signer_account.private_key))
        wait_for_confirmation(localnet.algod, txid, 4)
    except Exception as exc:
        raise DeployError(f"update txn failed: {exc}") from exc


def deploy(
    localnet: "LocalNet",
    artifacts: dict,
    *,
    ctor_args: list | None = None,
    fund_wei: int = 0,
    extra_funding_microalgos: int = 0,
    postinit_args: list | None = None,
    postinit_budget_pool: int = 0,
    postinit_inner_txns: int = 0,
    skip_postinit: bool = False,
    reserve_program_pages: int = 0,
    template_values: dict[str, int | str] | None = None,
    exact_schema: bool = False,
) -> DeployedApp:
    """Deploy the given compiled-contract artifacts. Raises DeployError on failure.

    artifacts: dict with keys 'arc56', 'approval_teal', 'clear_teal' (Path each).
    ctor_args: list of Python values matched against the constructor's
        ARC4 param types (one Python value per param). Passed as
        ApplicationArgs[0..N-1] to the create txn.
    fund_wei: extra microAlgos added on top of the default min-balance funding.
        Maps to Solidity `constructor() payable; new X{value: ...}` semantics.
    postinit_args: list of Python values for __postInit if your constructor
        body needs to run after the create txn (boxes, etc.). When None,
        __postInit is called with no args if present.
    skip_postinit: leave a deferred constructor unexecuted. This is used by
        the proxy-runtime replay model: implementation constructors bake
        runtime/immutable values but their storage writes occur in the
        implementation account, never in proxy storage.
    exact_schema: use precisely the ARC-56 state schema, without the historical
        spare global cells. Useful for testing the compiler's schema contract.
    """
    app_spec = _load_arc56(artifacts["arc56"])
    algod = localnet.algod
    schema = app_spec.state.schema
    global_schema = StateSchema(
        num_uints=schema.global_state.ints if exact_schema else 16,
        num_byte_slices=schema.global_state.bytes if exact_schema else 16,
    )
    local_schema = StateSchema(
        num_uints=schema.local_state.ints if exact_schema else 0,
        num_byte_slices=schema.local_state.bytes if exact_schema else 0,
    )

    approval_bin, clear_bin = compile_programs(
        localnet, artifacts, template_values)

    # algod caps the SUM of approval + clear at (1 + extra_pages) * 2048,
    # not the max of the two individually. snark.sol hits this: approval=6142
    # + clear=4 = 6146, which needs extra_pages=3 (budget 8192), not 2 (6144).
    page_size = 2048
    total_program_bytes = len(approval_bin) + len(clear_bin)
    extra_pages = max(
        int(reserve_program_pages),
        max(0, (total_program_bytes - 1) // page_size),
    )

    # Consensus v42 raises the absolute limit from 3 to 7 extra pages: 16 KiB
    # total. Bytes above the old four-page allowance are charged a small fee
    # and count against the create transaction's box-I/O write budget. The fee
    # below already has ample headroom; empty box refs contribute 2048 bytes of
    # I/O budget apiece and do not grant access to any actual box.
    max_extra_pages = 7
    free_program_bytes = 4 * page_size
    charged_program_bytes = max(0, total_program_bytes - free_program_bytes)
    write_budget_refs = (charged_program_bytes + page_size - 1) // page_size
    if extra_pages > max_extra_pages:
        raise DeployError(
            f"program exceeds AVM 16KB cap: approval={len(approval_bin)}B + "
            f"clear={len(clear_bin)}B needs extra_pages={extra_pages} "
            f"(max {max_extra_pages}); "
            "not a compiler bug — use helper extraction or state-preserving "
            "code paging")

    # Encode constructor args (if any) into ApplicationArgs.
    app_args = None
    if ctor_args:
        app_args = _encode_ctor_args(ctor_args, app_spec, artifacts)

    sp = algod.suggested_params()
    sp.flat_fee = True
    sp.fee = max(sp.min_fee, 1000) * 8

    create_txn = ApplicationCreateTxn(
        sender=localnet.account.address,
        sp=sp,
        on_complete=OnComplete.NoOpOC,
        approval_program=approval_bin,
        clear_program=clear_bin,
        global_schema=global_schema,
        local_schema=local_schema,
        extra_pages=extra_pages,
        boxes=[(0, b"")] * write_budget_refs,
        app_args=app_args,
        # Unique note: two xdist workers deploying the SAME fixture bytecode
        # in the same round would otherwise build IDENTICAL create txns —
        # same txid, second submit rejected "already in ledger".
        note=os.urandom(8),
    )
    signed = create_txn.sign(localnet.account.private_key)
    try:
        txid = algod.send_transaction(signed)
        result = wait_for_confirmation(algod, txid, 4)
    except Exception as e:
        _m = str(e)
        raise DeployError("create txn failed: "
                          + (_m[:120] + " … " + _m[-260:] if len(_m) > 400 else _m)) from e
    app_id = result["application-index"]

    app_addr = encoding.encode_address(
        encoding.checksum(b"appID" + app_id.to_bytes(8, "big"))
    )

    # Fund: base MBR + state schema + headroom for inner txns + ctor value forward
    min_balance = (100_000 + 28_500 * global_schema.num_uints
                   + 50_000 * global_schema.num_byte_slices + 10_000_000)
    # Slot mode: `new C()` grants children 4 ALGO (box MBR headroom) and the
    # inner create raises the creator's own min balance — give every deploy
    # enough slack that a couple of child creations never overspend.
    if "--evm-storage-layout" in os.environ.get("PUYA_SOL_EXTRA_ARGS", ""):
        min_balance += 10_000_000
    total_fund = min_balance + fund_wei + extra_funding_microalgos
    sp2 = algod.suggested_params()
    pay = PaymentTxn(
        localnet.account.address, sp2, app_addr, total_fund, note=os.urandom(8)
    )
    wait_for_confirmation(
        algod, algod.send_transaction(pay.sign(localnet.account.private_key)), 4
    )

    # --child-programs-via-box: stream child approval programs into their
    # "__cp_<Child>" boxes before __postInit runs the `new C()` creates.
    _provision_child_program_boxes(
        algod, localnet, app_id, app_spec, app_addr, artifacts)

    app_client = au.AppClient(
        au.AppClientParams(
            algorand=localnet.client,
            app_spec=app_spec,
            app_id=app_id,
            default_sender=localnet.account.address,
        )
    )

    # Call __postInit if present (writes ctor-allocated state to boxes etc.)
    postinit_spec = next(
        (m for m in app_spec.methods if m.name == "__postInit"), None
    )
    if postinit_spec and not skip_postinit:
        # Opcode budget is a RESOURCE, not a semantic property: a ctor that
        # initialises aggregates in slot mode costs far more than a scalar one,
        # and the right pool size is not knowable per test. Escalate on the
        # budget error only (any other failure propagates unchanged).
        _pool = postinit_budget_pool
        _fee_bump = 0
        while True:
            try:
                _call_postinit(
                    algod=algod,
                    localnet=localnet,
                    app_id=app_id,
                    app_spec=app_spec,
                    postinit_spec=postinit_spec,
                    ctor_args=ctor_args,
                    postinit_args=postinit_args,
                    budget_pool=_pool,
                    inner_txns=postinit_inner_txns,
                    fee_bump=_fee_bump,
                )
                break
            except Exception as e:
                # the group holds the call + inner txns + helpers, and AVM
                # caps a group at 16 — never escalate past what fits
                _cap = 14 - int(postinit_inner_txns or 0)
                msg = str(e)
                # inner-txn fee shortfall ("group fee 0.0A too small (need
                # NmA)"): the ctor spawned more inner txns than
                # postinit_inner_txns declared (slot mode adds a __postInit
                # per `new C()`). Pool helpers are FEE-NEUTRAL (each adds
                # 1000 and consumes 1000), so the shortfall goes straight
                # onto the call's flat fee.
                import re as _re
                m = _re.search(r"need (\d+)mA", msg)
                fee_short = "fee" in msg and "too small" in msg
                if fee_short and _fee_bump < 32_000:
                    _fee_bump += ((int(m.group(1)) if m else 3) + 1) * 1000
                    continue
                if "budget exceeded" not in msg or _pool >= _cap:
                    raise
                _pool = min(max(4, _pool * 2), _cap)

    # Read balance after postInit so child-app deployments and box MBR are
    # already subtracted from the baseline.
    try:
        post_bal = algod.account_info(app_addr)["amount"]
    except Exception:
        post_bal = min_balance
    forwarded = _scan_ctor_forwarded_value(artifacts["approval_teal"])
    baseline = post_bal - fund_wei + forwarded

    return DeployedApp(
        app_id=app_id,
        app_addr=app_addr,
        app_spec=app_spec,
        client=app_client,
        balance_baseline=baseline,
        ctor_fund_wei=fund_wei,
    )


def _encode_ctor_args(values: list, app_spec, artifacts) -> list[bytes]:
    """Encode ctor args.

    The new framework expects callers to pass Python values that match the
    constructor's ARC4 param types directly. If the test wants the legacy
    EVM-style flat grouping (each param is a tuple/struct), the test should
    pass already-grouped values.
    """
    from algosdk import abi as _abi

    # Try to discover param types from __postInit (canonical) or constructor
    method = next((m for m in app_spec.methods if m.name == "__postInit"), None)
    if not method:
        method = next((m for m in app_spec.methods if m.name == "constructor"), None)
    if not method:
        # No spec — fall back to per-value default encoding
        return [_default_encode(v) for v in values]
    if len(values) != len(method.args):
        raise DeployError(
            f"constructor argument count mismatch: expected {len(method.args)}, "
            f"got {len(values)}"
        )

    encoded: list[bytes] = []
    for spec, val in zip(method.args, values):
        try:
            t = _abi.ABIType.from_string(str(spec.type))
            encoded.append(t.encode(val))
        except Exception:
            encoded.append(_default_encode(val))
    return encoded


def _default_encode(v) -> bytes:
    if isinstance(v, bytes):
        return v
    if isinstance(v, str):
        # Algorand address (58-char base32)? Decode to the 32-byte payload.
        # Otherwise treat as raw string (ASCII bytes).
        if len(v) == 58:
            try:
                from algosdk import encoding as _enc
                return _enc.decode_address(v)
            except Exception:
                pass
        return v.encode()
    if isinstance(v, bool):
        # Constructor reads bool via `extract_uint64` (8-byte field) so encode
        # as itob(1)/itob(0) rather than ARC4 1-byte `\x80`/`\x00`.
        return (1 if v else 0).to_bytes(8, "big")
    if isinstance(v, int):
        # 32-byte big-endian (Solidity uint256 default)
        return v.to_bytes(32, "big", signed=v < 0)
    if isinstance(v, (list, tuple)):
        # Static array fallback: concat each element's default-encoding.
        return b"".join(_default_encode(x) for x in v)
    raise TypeError(f"can't default-encode {type(v).__name__}")


_MIN_BALANCE_RE = re.compile(
    r"account (\S+) balance (\d+) below min (\d+)")


def _topup_min_balance(algod, localnet, error: Exception) -> bool:
    """Cover a min-balance shortfall the node reported, if that's the failure.

    Box MBR is 2500 + 400*(name+size) per box, so it scales with whatever the
    contract's storage layout actually allocates. Deploy funding is a flat
    constant, which silently became too small the moment an array needed a
    second 32 KB page. Reading the shortfall off the node's own message keeps
    the harness honest about layout changes instead of re-tuning a magic
    number per contract. Returns False when this is some other error.
    """
    match = _MIN_BALANCE_RE.search(str(error))
    if not match:
        return False
    address, balance, required = match.group(1), int(match.group(2)), int(match.group(3))
    shortfall = required - balance
    if shortfall <= 0:
        return False
    sp = algod.suggested_params()
    pay = PaymentTxn(
        localnet.account.address, sp, address,
        shortfall + 1_000_000, note=os.urandom(8),
    )
    wait_for_confirmation(
        algod, algod.send_transaction(pay.sign(localnet.account.private_key)), 4
    )
    return True


def _call_postinit(
    *,
    algod,
    localnet,
    app_id: int,
    app_spec,
    postinit_spec,
    ctor_args: list | None,
    postinit_args: list | None,
    budget_pool: int = 0,
    fee_bump: int = 0,
    inner_txns: int = 0,
    topped_up: bool = False,
) -> None:
    """Call the __postInit method, populating box refs via simulate.

    budget_pool: when > 0, prepend that many no-op app calls to the shared
        budget helper. Each helper call contributes 700 extra opcode
        budget via fee pooling.
    inner_txns: number of inner txns the constructor body issues. Each
        contributes 1000 microalgos to the postinit's flat fee so the
        outer group covers their fees.
    """
    import os
    from algosdk.atomic_transaction_composer import (
        AtomicTransactionComposer,
        TransactionWithSigner,
    )
    from algosdk.transaction import ApplicationCallTxn, OnComplete

    sender = localnet.account.address
    signer = localnet.client.account.get_signer(sender)
    sp = algod.suggested_params()
    sp.flat_fee = True
    base_extra = max(4, inner_txns + 1)
    sp.fee = (1000 * (budget_pool + base_extra) if (budget_pool or inner_txns) else 4000) + fee_bump

    abi_method = postinit_spec.to_abi_method()
    # Padding and ABI coercion below mutate the list. Work on a copy so a
    # caller can safely reuse its ctor_args/postinit_args after deployment.
    args = list(postinit_args if postinit_args is not None else (ctor_args or []))
    if len(args) > len(abi_method.args):
        raise DeployError(
            f"__postInit argument count mismatch: expected at most "
            f"{len(abi_method.args)}, got {len(args)}"
        )
    # Pad missing trailing args with zero values
    while len(args) < len(abi_method.args):
        args.append(_zero_for_type(str(abi_method.args[len(args)].type)))
    # Coerce bare ints aimed at byte[N] params (fn-ptr carriers: the EVM
    # fixture spells a fn ptr as one packed word) to N big-endian bytes, and
    # ints aimed at address params to the canonical 32-byte form (algosdk's
    # address codec takes base32/bytes32, not ints).
    for i, spec in enumerate(abi_method.args[: len(args)]):
        t = str(spec.type)
        if not (isinstance(args[i], int) and not isinstance(args[i], bool)):
            continue
        if t.startswith("byte[") and t != "byte[]":
            n = int(t[5:-1])
            args[i] = list((args[i] % (1 << (8 * n))).to_bytes(n, "big"))
        elif t == "address":
            args[i] = (args[i] % (1 << 256)).to_bytes(32, "big")

    atc = AtomicTransactionComposer()
    if budget_pool > 0:
        helper_id = localnet.budget_helper_id
        sp_dummy = algod.suggested_params()
        sp_dummy.flat_fee = True
        sp_dummy.fee = 0
        for i in range(budget_pool):
            dummy = ApplicationCallTxn(
                sender=sender,
                sp=sp_dummy,
                index=helper_id,
                on_complete=OnComplete.NoOpOC,
                note=os.urandom(8),
            )
            atc.add_transaction(TransactionWithSigner(dummy, signer))
    atc.add_method_call(
        app_id=app_id,
        method=abi_method,
        sender=sender,
        sp=sp,
        signer=signer,
        method_args=args[: len(abi_method.args)],
        note=os.urandom(8),
    )
    try:
        atc = au.populate_app_call_resources(atc, algod)
        atc.execute(algod, wait_rounds=4)
    except Exception as e:
        # Box MBR outgrew the flat deploy funding — top up by the reported
        # shortfall and retry once.
        if not topped_up and _topup_min_balance(algod, localnet, e):
            return _call_postinit(
                algod=algod, localnet=localnet, app_id=app_id, app_spec=app_spec,
                postinit_spec=postinit_spec, ctor_args=ctor_args,
                postinit_args=postinit_args, budget_pool=budget_pool,
                fee_bump=fee_bump, inner_txns=inner_txns, topped_up=True)
        # Reference-array overflow (>8 foreign refs on the single postinit txn):
        # retry with dummy helper txns — each group txn carries 8 more refs.
        msg = str(e).lower()
        if budget_pool == 0 and ("box reference" in msg or "unavailable" in msg
                                 or "budget" in msg or "opcode" in msg):
            return _call_postinit(
                algod=algod, localnet=localnet, app_id=app_id, app_spec=app_spec,
                postinit_spec=postinit_spec, ctor_args=ctor_args,
                postinit_args=postinit_args, budget_pool=8, inner_txns=inner_txns,
                topped_up=topped_up)
        raise DeployError(f"__postInit failed: {str(e)[:300]}") from e


def _provision_child_program_boxes(
    algod, localnet, app_id: int, app_spec, app_addr: str, artifacts: dict
) -> None:
    """--child-programs-via-box: stream each child approval program into its
    "__cp_<Child>" box via the synthesized __provisionChildProg method.

    The method's presence in the arc56 IS the flag signal (it is only
    synthesized for contracts whose `new C()` loads box pages); the child
    binaries still land in deploy.tmpl.json as TMPL_APPROVAL_<child>_P0/_P1
    pages — here they feed the box instead of a TEAL substitution."""
    import json as _json
    import os
    from algosdk.atomic_transaction_composer import AtomicTransactionComposer
    from algosdk.transaction import PaymentTxn

    prov_spec = next(
        (m for m in app_spec.methods if m.name == "__provisionChildProg"), None)
    if prov_spec is None:
        return
    tmpl_path = artifacts["approval_teal"].parent / "deploy.tmpl.json"
    if not tmpl_path.exists():
        return
    tmpl = _json.loads(tmpl_path.read_text())
    programs: dict[str, bytes] = {}
    for k in tmpl:
        if k.startswith("TMPL_APPROVAL_") and k.endswith("_P0"):
            child = k[len("TMPL_APPROVAL_"):-len("_P0")]
            programs[child] = bytes.fromhex(
                tmpl.get(f"TMPL_APPROVAL_{child}_P0", "")
                + tmpl.get(f"TMPL_APPROVAL_{child}_P1", ""))

    sender = localnet.account.address
    signer = localnet.client.account.get_signer(sender)
    method = prov_spec.to_abi_method()
    for child, program in sorted(programs.items()):
        box_name = b"__cp_" + child.encode()
        # Box MBR lands on the app account at box_create time — fund it first.
        mbr = 2500 + 400 * (len(box_name) + len(program)) + 1000
        sp_pay = algod.suggested_params()
        pay = PaymentTxn(sender, sp_pay, app_addr, mbr, note=os.urandom(8))
        wait_for_confirmation(
            algod,
            algod.send_transaction(pay.sign(localnet.account.private_key)), 4)
        # ≤1900-byte chunks keep each call's app-args well under the 2048 cap.
        chunk = 1900
        for off in range(0, len(program), chunk):
            atc = AtomicTransactionComposer()
            sp = algod.suggested_params()
            sp.flat_fee = True
            sp.fee = 2000
            atc.add_method_call(
                app_id=app_id, method=method, sender=sender, sp=sp,
                signer=signer,
                method_args=[box_name, len(program), off,
                             program[off:off + chunk]],
                note=os.urandom(8))
            try:
                atc = au.populate_app_call_resources(atc, algod)
                atc.execute(algod, wait_rounds=4)
            except Exception as e:
                raise DeployError(
                    f"__provisionChildProg({child}, off={off}) failed: "
                    f"{str(e)[:300]}") from e


def _zero_for_type(t: str):
    if t == "bool":
        return False
    if t == "string":
        return ""
    if t.endswith("[]"):
        return []
    if t.startswith("byte["):
        try:
            n = int(t[5:-1])
        except ValueError:
            n = 0
        return [0] * n
    if t.startswith(("uint", "int")):
        return 0
    return 0
