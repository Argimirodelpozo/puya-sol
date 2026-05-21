"""Deploy a compiled ARC56 contract to localnet."""
from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

import algokit_utils as au
from algosdk import encoding
from algosdk.transaction import (
    ApplicationCreateTxn,
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


def _substitute_template_vars(teal: str, tmpl_path: Path) -> str:
    """Replace TMPL_* placeholders in TEAL using puya's deploy.tmpl.json."""
    if not tmpl_path.exists():
        return teal
    import json
    try:
        data = json.loads(tmpl_path.read_text())
    except (json.JSONDecodeError, OSError):
        return teal
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
    """
    app_spec = _load_arc56(artifacts["arc56"])
    algod = localnet.algod

    approval_src = _substitute_template_vars(
        artifacts["approval_teal"].read_text(),
        artifacts["approval_teal"].parent / "deploy.tmpl.json",
    )
    clear_src = _substitute_template_vars(
        artifacts["clear_teal"].read_text(),
        artifacts["clear_teal"].parent / "deploy.tmpl.json",
    )
    approval_bin = encoding.base64.b64decode(algod.compile(approval_src)["result"])
    clear_bin = encoding.base64.b64decode(algod.compile(clear_src)["result"])

    # algod caps the SUM of approval + clear at (1 + extra_pages) * 2048,
    # not the max of the two individually. snark.sol hits this: approval=6142
    # + clear=4 = 6146, which needs extra_pages=3 (budget 8192), not 2 (6144).
    extra_pages = max(0, (len(approval_bin) + len(clear_bin) - 1) // 2048)

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
        global_schema=StateSchema(num_uints=16, num_byte_slices=16),
        local_schema=StateSchema(num_uints=0, num_byte_slices=0),
        extra_pages=extra_pages,
        app_args=app_args,
    )
    signed = create_txn.sign(localnet.account.private_key)
    try:
        txid = algod.send_transaction(signed)
        result = wait_for_confirmation(algod, txid, 4)
    except Exception as e:
        raise DeployError(f"create txn failed: {str(e)[:300]}") from e
    app_id = result["application-index"]

    app_addr = encoding.encode_address(
        encoding.checksum(b"appID" + app_id.to_bytes(8, "big"))
    )

    # Fund: base MBR + state schema + headroom for inner txns + ctor value forward
    min_balance = 100_000 + 28_500 * 16 + 50_000 * 16 + 10_000_000
    total_fund = min_balance + fund_wei + extra_funding_microalgos
    sp2 = algod.suggested_params()
    pay = PaymentTxn(
        localnet.account.address, sp2, app_addr, total_fund
    )
    wait_for_confirmation(
        algod, algod.send_transaction(pay.sign(localnet.account.private_key)), 4
    )

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
    if postinit_spec:
        _call_postinit(
            algod=algod,
            localnet=localnet,
            app_id=app_id,
            app_spec=app_spec,
            postinit_spec=postinit_spec,
            ctor_args=ctor_args,
            postinit_args=postinit_args,
            budget_pool=postinit_budget_pool,
            inner_txns=postinit_inner_txns,
        )

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
    inner_txns: int = 0,
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
    sp.fee = 1000 * (budget_pool + base_extra) if (budget_pool or inner_txns) else 4000

    abi_method = postinit_spec.to_abi_method()
    args = postinit_args if postinit_args is not None else (ctor_args or [])
    # Pad missing trailing args with zero values
    while len(args) < len(abi_method.args):
        args.append(_zero_for_type(str(abi_method.args[len(args)].type)))

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
    atc = au.populate_app_call_resources(atc, algod)
    try:
        atc.execute(algod, wait_rounds=4)
    except Exception as e:
        raise DeployError(f"__postInit failed: {str(e)[:300]}") from e


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
