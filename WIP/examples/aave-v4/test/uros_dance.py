"""Helpers for deploying --uros-splitter contracts and invoking
their stubbed methods via the orchestrator's 3-itxn dance.

Pattern, per test session:
  * compile the orchestrator template once (via puyapy)
  * deploy ONE orchestrator app (session-scoped fixture)
  * for each split contract under test, call deploy_split_app() — it
    substitutes TMPL_UROS_ORCH_APP_ID in the .approval.teal at deploy
    time (no puya recompile needed), deploys main, runs the setup
    ceremony, returns a SplitClient wrapping main_id + orch_id.

Pattern, per call to a stubbed method:
  client.call_dance("methodSig(types)retType", [arg1_bytes, ...]) returns
  the algod confirmation result. The caller decodes the return-log if
  needed (raw_logs[0] = ABI return: 4-byte 151f7c75 prefix + value).
"""

from __future__ import annotations

import base64
import hashlib
import json
import subprocess
from dataclasses import dataclass
from pathlib import Path

import algokit_utils as au
from algokit_utils.models.account import SigningAccount
from algosdk import encoding
from algosdk.atomic_transaction_composer import (
    AccountTransactionSigner,
    AtomicTransactionComposer,
    TransactionWithSigner,
)
from algosdk.transaction import (
    ApplicationCallTxn,
    ApplicationCreateTxn,
    OnComplete,
    PaymentTxn,
    StateSchema,
    assign_group_id,
    wait_for_confirmation,
)
from algosdk.v2client.algod import AlgodClient

REPO_ROOT = Path(__file__).resolve().parents[4]  # → puya-sol/
ORCH_PY = REPO_ROOT / "src" / "splitter" / "uros_orchestrator.py"
PUYAPY_BIN = REPO_ROOT / "puya" / ".venv" / "bin" / "puyapy"
ORCH_OUT = REPO_ROOT / "tests" / "uros-splitter" / "out" / "Orchestrator"

OUT_DIR = Path(__file__).parent.parent / "out"

DISPATCH_SELECTOR_HEX = "617da41d"  # sha512_256("dispatch()byte[]")[:4]
EMPTY_BOX = (0, b"")


def _arc4_selector(sig: str) -> bytes:
    return hashlib.new("sha512_256", sig.encode()).digest()[:4]


def _app_addr(app_id: int) -> str:
    return encoding.encode_address(
        encoding.checksum(b"appID" + app_id.to_bytes(8, "big"))
    )


def _fund(algod: AlgodClient, sender: SigningAccount, addr: str, amt: int) -> None:
    sp = algod.suggested_params()
    txn = PaymentTxn(sender.address, sp, addr, amt)
    txid = algod.send_transaction(txn.sign(sender.private_key))
    wait_for_confirmation(algod, txid, 4)


def compile_orchestrator() -> None:
    """Compile uros_orchestrator.py if its outputs aren't already present."""
    if (ORCH_OUT / "UrosOrchestrator.approval.teal").exists():
        return
    ORCH_OUT.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [str(PUYAPY_BIN), str(ORCH_PY), "--out-dir", str(ORCH_OUT), "--output-bytecode"],
        check=True,
        capture_output=True,
    )


def deploy_orchestrator(algod: AlgodClient, sender: SigningAccount) -> int:
    compile_orchestrator()
    approval_teal = (ORCH_OUT / "UrosOrchestrator.approval.teal").read_text()
    clear_teal = (ORCH_OUT / "UrosOrchestrator.clear.teal").read_text()
    approval = base64.b64decode(algod.compile(approval_teal)["result"])
    clear = base64.b64decode(algod.compile(clear_teal)["result"])

    sp = algod.suggested_params()
    init_sel = _arc4_selector("init()void")
    txn = ApplicationCreateTxn(
        sender=sender.address,
        sp=sp,
        on_complete=OnComplete.NoOpOC,
        approval_program=approval,
        clear_program=clear,
        global_schema=StateSchema(num_uints=16, num_byte_slices=16),
        local_schema=StateSchema(num_uints=0, num_byte_slices=0),
        app_args=[init_sel],
        extra_pages=3,
    )
    txid = algod.send_transaction(txn.sign(sender.private_key))
    result = wait_for_confirmation(algod, txid, 4)
    orch_id = int(result["application-index"])
    # Orch holds the bytecode boxes: __codebox_main (~8 KB) + one
    # __codebox_chunk_<i> per chunk (each ~8 KB) + small clen_* /
    # csel_* per registered method. MBR ≈ 400 µA/byte → fund
    # generously so a 10-chunk Hub still fits.
    _fund(algod, sender, _app_addr(orch_id), 200_000_000)
    return orch_id


def _substitute_orch_id(teal: str, orch_id: int) -> str:
    return teal.replace("TMPL_UROS_ORCH_APP_ID", str(orch_id))


def _compile_teal(algod: AlgodClient, teal: str) -> bytes:
    return base64.b64decode(algod.compile(teal)["result"])


@dataclass
class SplitDeployment:
    """Returned by deploy_split_app; carries everything needed to
    invoke stubbed methods via the dance."""
    main_id: int
    orch_id: int
    app_spec: au.Arc56Contract
    deploy_tmpl: dict
    selector_to_chunk: dict[bytes, int]  # method selector → chunk_idx

    def call_dance(
        self,
        algod: AlgodClient,
        sender: SigningAccount,
        method_sig: str,
        method_args: list[bytes] | None = None,
    ) -> dict:
        """Send the 2-txn group [main.method, orch.dispatch] and return
        the algod confirmation result.

        Box read budget split:
          txn1 (main): 8 empty refs as pure budget carriers (8×1024 B).
          txn2 (orch): 4 known refs (csel, clen, chunk, main) + 3 empty
                       + 1 foreign_app (main_id) = 8 references max.

        The inner dispatch call runs main's real method body (e.g.
        `getRoleCount` reads `_rolesSet`). Those state boxes aren't
        known statically here — we use populate_app_call_resources to
        auto-discover them by simulating the group, then merge the
        result into txn1's empty box-ref slots.
        """
        sel = _arc4_selector(method_sig)
        chunk_idx = self.selector_to_chunk[sel]
        sp = algod.suggested_params()
        sp.fee = sp.min_fee * 12  # dance does 3 inner txns + buffer
        txn1 = ApplicationCallTxn(
            sender=sender.address, sp=sp, index=self.main_id,
            on_complete=OnComplete.NoOpOC,
            app_args=[sel, *(method_args or [])],
        )
        dispatch_sel = _arc4_selector("dispatch()byte[]")
        csel_box = b"csel_" + sel
        clen_box = b"clen_" + chunk_idx.to_bytes(8, "big")
        chunk_box = b"__codebox_chunk_" + chunk_idx.to_bytes(8, "big")
        main_box = b"__codebox_main"
        txn2 = ApplicationCallTxn(
            sender=sender.address, sp=sp, index=self.orch_id,
            on_complete=OnComplete.NoOpOC,
            app_args=[dispatch_sel],
            foreign_apps=[self.main_id],
            boxes=[
                (self.orch_id, csel_box),
                (self.orch_id, clen_box),
                (self.orch_id, chunk_box),
                (self.orch_id, main_box),
                EMPTY_BOX, EMPTY_BOX, EMPTY_BOX,
            ],
        )
        signer = AccountTransactionSigner(sender.private_key)
        atc = AtomicTransactionComposer()
        atc.add_transaction(TransactionWithSigner(txn1, signer))
        atc.add_transaction(TransactionWithSigner(txn2, signer))
        atc = au.populate_app_call_resources(atc, algod)
        results = atc.execute(algod, 4)
        # AtomicTransactionComposer.execute returns a result with
        # tx_ids and confirmed_round; fetch the dispatch txn's full
        # confirmation to get inner-txns + logs.
        dispatch_txid = results.tx_ids[1]
        return algod.pending_transaction_info(dispatch_txid)


def _stream_to_box(
    algod: AlgodClient, sender: SigningAccount, orch_id: int,
    write_sel: bytes, box_name: bytes, data: bytes,
    extra_app_args: list[bytes] | None = None,
) -> None:
    chunk_size = 1024
    for offset in range(0, len(data), chunk_size):
        chunk = data[offset : offset + chunk_size]
        sp = algod.suggested_params()
        off_b = offset.to_bytes(8, "big")
        data_b = len(chunk).to_bytes(2, "big") + chunk
        app_args = [write_sel] + (extra_app_args or []) + [off_b, data_b]
        txn = ApplicationCallTxn(
            sender=sender.address, sp=sp, index=orch_id,
            on_complete=OnComplete.NoOpOC,
            app_args=app_args,
            boxes=[(orch_id, box_name)] + [EMPTY_BOX] * 7,
        )
        txid = algod.send_transaction(txn.sign(sender.private_key))
        wait_for_confirmation(algod, txid, 4)


def _box_exists_with_size(algod: AlgodClient, app_id: int, name: bytes,
                          expected_size: int) -> bool:
    """Check whether `name` exists on `app_id` with exactly `expected_size`
    bytes. Used to skip redundant setup calls when an orch fixture is
    reused across tests in the same session."""
    try:
        info = algod.application_box_by_name(app_id, name)
    except Exception:
        return False
    value_b64 = info.get("value", "")
    return len(base64.b64decode(value_b64)) == expected_size


def _setup_main_and_chunks(
    algod: AlgodClient,
    sender: SigningAccount,
    orch_id: int,
    main_id: int,
    main_bytes: bytes,
    chunks: list[dict],
) -> dict[bytes, int]:
    """Run set_main + setup_*_box + write_* + register_chunk_method
    for all chunks. Idempotent on box setup — if a chunk's box is
    already present with the same length, skip the setup + write.
    Returns the selector → chunk_idx map."""
    # set_main is always called: each new main gets a fresh app id, so
    # the orch's main_app_id global must be re-pointed.
    set_main_sel = _arc4_selector("set_main(uint64)void")
    sp = algod.suggested_params()
    txn = ApplicationCallTxn(
        sender=sender.address, sp=sp, index=orch_id,
        on_complete=OnComplete.NoOpOC,
        app_args=[set_main_sel, main_id.to_bytes(8, "big")],
    )
    txid = algod.send_transaction(txn.sign(sender.private_key))
    wait_for_confirmation(algod, txid, 4)

    main_box_name = b"__codebox_main"
    if not _box_exists_with_size(algod, orch_id, main_box_name, len(main_bytes)):
        setup_main_sel = _arc4_selector("setup_main_box(uint64)void")
        sp = algod.suggested_params()
        sp.fee = sp.min_fee * 3  # box-create inner txn
        txn = ApplicationCallTxn(
            sender=sender.address, sp=sp, index=orch_id,
            on_complete=OnComplete.NoOpOC,
            app_args=[setup_main_sel, len(main_bytes).to_bytes(8, "big")],
            boxes=[(orch_id, main_box_name)] + [EMPTY_BOX] * 7,
        )
        txid = algod.send_transaction(txn.sign(sender.private_key))
        wait_for_confirmation(algod, txid, 4)
        write_main_sel = _arc4_selector("write_main(uint64,byte[])void")
        _stream_to_box(algod, sender, orch_id, write_main_sel,
                       main_box_name, main_bytes)

    setup_chunk_sel = _arc4_selector("setup_chunk_box(uint64,uint64)void")
    write_chunk_sel = _arc4_selector("write_chunk(uint64,uint64,byte[])void")
    for ci, chunk in enumerate(chunks):
        chunk_bytes = bytes.fromhex(chunk["approval_hex"])
        chunk_box = b"__codebox_chunk_" + ci.to_bytes(8, "big")
        clen_box = b"clen_" + ci.to_bytes(8, "big")
        if _box_exists_with_size(algod, orch_id, chunk_box, len(chunk_bytes)):
            continue
        sp = algod.suggested_params()
        sp.fee = sp.min_fee * 3
        txn = ApplicationCallTxn(
            sender=sender.address, sp=sp, index=orch_id,
            on_complete=OnComplete.NoOpOC,
            app_args=[
                setup_chunk_sel,
                ci.to_bytes(8, "big"),
                len(chunk_bytes).to_bytes(8, "big"),
            ],
            boxes=[(orch_id, chunk_box), (orch_id, clen_box)] + [EMPTY_BOX] * 6,
        )
        txid = algod.send_transaction(txn.sign(sender.private_key))
        wait_for_confirmation(algod, txid, 4)
        _stream_to_box(
            algod, sender, orch_id, write_chunk_sel,
            chunk_box, chunk_bytes,
            extra_app_args=[ci.to_bytes(8, "big")],
        )

    # Register each split method's selector → chunk_idx.
    register_sel = _arc4_selector("register_chunk_method(byte[],uint64)void")
    selector_to_chunk: dict[bytes, int] = {}
    for ci, chunk in enumerate(chunks):
        for full_sig in chunk["full_sigs"]:
            sel = _arc4_selector(full_sig)
            selector_to_chunk[sel] = ci
            csel_box = b"csel_" + sel
            sel_arg = len(sel).to_bytes(2, "big") + sel
            sp = algod.suggested_params()
            txn = ApplicationCallTxn(
                sender=sender.address, sp=sp, index=orch_id,
                on_complete=OnComplete.NoOpOC,
                app_args=[register_sel, sel_arg, ci.to_bytes(8, "big")],
                boxes=[(orch_id, csel_box)] + [EMPTY_BOX] * 7,
            )
            txid = algod.send_transaction(txn.sign(sender.private_key))
            wait_for_confirmation(algod, txid, 4)
    return selector_to_chunk


def _call_postinit(
    algod: AlgodClient, sender: SigningAccount, app_id: int,
    app_spec: au.Arc56Contract, app_args: list[bytes],
) -> None:
    """If the contract has __postInit, invoke it with the same args
    that were passed at AppCreate (puya-sol's auto-split ctor pattern)."""
    postinit_method = None
    for m in getattr(app_spec, "methods", []) or []:
        if getattr(m, "name", None) == "__postInit":
            postinit_method = m
            break
    if postinit_method is None or len(app_args) < len(postinit_method.args):
        return
    arg_types = ",".join(getattr(a, "type", "") for a in postinit_method.args)
    sig = f"__postInit({arg_types})void"
    sel = _arc4_selector(sig)
    sp = algod.suggested_params()
    call_args = [sel] + list(app_args)[: len(postinit_method.args)]
    call_txn = ApplicationCallTxn(
        sender=sender.address, sp=sp, index=app_id,
        on_complete=OnComplete.NoOpOC, app_args=call_args,
    )
    signer = AccountTransactionSigner(sender.private_key)
    atc = AtomicTransactionComposer()
    atc.add_transaction(TransactionWithSigner(call_txn, signer))
    try:
        atc = au.populate_app_call_resources(atc, algod)
    except Exception:
        pass
    try:
        atc.execute(algod, 4)
    except Exception:
        # Same fail-open semantics as conftest.deploy_contract.
        pass


def deploy_split_app(
    algod: AlgodClient,
    sender: SigningAccount,
    name: str,
    orch_id: int,
    app_args: list[bytes] | None = None,
    fund_amount: int = 1_000_000,
) -> SplitDeployment:
    """Deploy a split contract: substitute orch_id into TEAL, deploy
    main, run the orch setup ceremony, optionally __postInit. The
    chunks' bytecode is the build-time output (orch_id is template
    var #7 in the intcblock — we substitute via .replace on TEAL
    text and recompile via algod.compile)."""
    contract_dir = OUT_DIR / name
    deploy_tmpl = json.loads((contract_dir / "deploy.uros.json").read_text())

    # 1. Substitute TMPL_UROS_ORCH_APP_ID and compile main.
    main_teal_path = contract_dir / f"{name}.approval.teal"
    main_clear_path = contract_dir / f"{name}.clear.teal"
    main_teal = _substitute_orch_id(main_teal_path.read_text(), orch_id)
    main_approval = _compile_teal(algod, main_teal)
    main_clear = _compile_teal(algod, main_clear_path.read_text())

    # 2. Substitute and compile each chunk's TEAL — the orch ceremony
    #    streams the substituted bytes into __codebox_chunk_<i>.
    chunks = []
    arc56_path = contract_dir / f"{name}.arc56.json"
    app_spec = au.Arc56Contract.from_json(arc56_path.read_text())
    method_signatures = {m.name: m for m in (app_spec.methods or [])}
    for ci, c in enumerate(deploy_tmpl["chunks"]):
        chunk_teal = (
            contract_dir / "__uros_split" / f"chunk_{ci}" / f"{c['name']}.approval.teal"
        ).read_text()
        chunk_teal = _substitute_orch_id(chunk_teal, orch_id)
        chunk_bytes = _compile_teal(algod, chunk_teal)
        # Look up each split method's full ABI signature from the arc56 spec.
        full_sigs = []
        for mname in c["methods"]:
            m = method_signatures.get(mname)
            if m is None:
                # Fallback: use bare name (won't compute a useful selector
                # but the harness still streams + registers). Surface the
                # mismatch via assertion so the test fails loudly.
                raise AssertionError(
                    f"split method '{mname}' not in arc56 for {name}"
                )
            arg_types = ",".join(getattr(a, "type", "") for a in m.args)
            ret = getattr(getattr(m, "returns", None), "type", "void") or "void"
            full_sigs.append(f"{mname}({arg_types}){ret}")
        chunks.append({
            **c,
            "approval_hex": chunk_bytes.hex(),
            "full_sigs": full_sigs,
        })

    # 3. Deploy main.
    sp = algod.suggested_params()
    txn = ApplicationCreateTxn(
        sender=sender.address,
        sp=sp,
        on_complete=OnComplete.NoOpOC,
        approval_program=main_approval,
        clear_program=main_clear,
        global_schema=StateSchema(num_uints=16, num_byte_slices=16),
        local_schema=StateSchema(num_uints=0, num_byte_slices=0),
        app_args=app_args or [],
        extra_pages=3,
    )
    txid = algod.send_transaction(txn.sign(sender.private_key))
    result = wait_for_confirmation(algod, txid, 4)
    main_id = int(result["application-index"])
    if fund_amount > 0:
        _fund(algod, sender, _app_addr(main_id), fund_amount)

    # 4. Run __postInit (if present and arg-count matches).
    if app_args:
        _call_postinit(algod, sender, main_id, app_spec, list(app_args))

    # 5. Run orch setup ceremony.
    selector_to_chunk = _setup_main_and_chunks(
        algod, sender, orch_id, main_id, main_approval, chunks,
    )

    return SplitDeployment(
        main_id=main_id,
        orch_id=orch_id,
        app_spec=app_spec,
        deploy_tmpl=deploy_tmpl,
        selector_to_chunk=selector_to_chunk,
    )
