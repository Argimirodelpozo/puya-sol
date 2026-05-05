"""Helpers for deploying --uros-splitter contracts in the
3-contract architecture (main + __storage + orch).

Architecture:
  main      — entry point; full ABI surface; stubs forward to orch
              via inner-app-call. State on main is unused (just gets
              defaults written at AppCreate time but never read).
  __storage — state holder; deployed first with MAIN's bytecode so
              AppCreate runs the user contract's state-var defaults
              on __storage. After AppCreate, harness UpdateApplications
              __storage to the thin admit-update default bytecode.
              From then on, only orch may UpdateApplication.
  orch      — generic dance executor; holds chunk bytecode in boxes,
              swaps chunks onto __storage per call, restores default.

Per-call flow (transparent to caller):
  user → main.foo(args)   (single txn, no group dance)
    main.foo stub → inner orch.dispatch(args=[dispatch_sel, foo_sel,
                                              ...user_args])
      orch.dispatch:
        itxn 1 → UpdateApplication(__storage, chunk_for_foo)
        itxn 2 → __storage.foo(args)  ← chunk runs, mutates state
        itxn 3 → UpdateApplication(__storage, default)
      returns last_log
    main.foo returns last_log to user

Deploy flow:
  1. Deploy orch (compile uros_orchestrator.py once per session)
  2. Compile uros_storage.py for the thin default bytecode
  3. For each split contract:
     a. Recompile its main.teal with TMPL_UROS_ORCH_APP_ID = orch.id
     b. Deploy __storage with main_bytes (AppCreate inits state)
     c. UpdateApplication __storage → thin default (via main's
        __delegate_update)
     d. set_orch on __storage; set_storage on orch
     e. Stream default + chunks into orch boxes
     f. register_chunk_method per selector
     g. Deploy main as a separate app
     h. Call main.__postInit(args) → dance routes to __storage,
        runs constructor body; state init complete
"""

from __future__ import annotations

import base64
import hashlib
import json
import os
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
    wait_for_confirmation,
)
from algosdk.v2client.algod import AlgodClient

REPO_ROOT = Path(__file__).resolve().parents[4]  # → puya-sol/
PUYAPY_BIN = REPO_ROOT / "puya" / ".venv" / "bin" / "puyapy"
ORCH_PY = REPO_ROOT / "src" / "splitter" / "uros_orchestrator.py"
STORAGE_PY = REPO_ROOT / "src" / "splitter" / "uros_storage.py"
ORCH_OUT = REPO_ROOT / "tests" / "uros-splitter" / "out" / "Orchestrator"
STORAGE_OUT = REPO_ROOT / "tests" / "uros-splitter" / "out" / "Storage"

OUT_DIR = Path(__file__).parent.parent / "out"

EMPTY_BOX = (0, b"")


def _arc4_selector(sig: str) -> bytes:
    return hashlib.new("sha512_256", sig.encode()).digest()[:4]


def _app_addr(app_id: int) -> str:
    return encoding.encode_address(
        encoding.checksum(b"appID" + app_id.to_bytes(8, "big")))


def _fund(algod: AlgodClient, sender: SigningAccount, addr: str, amt: int) -> None:
    sp = algod.suggested_params()
    txn = PaymentTxn(sender.address, sp, addr, amt)
    txid = algod.send_transaction(txn.sign(sender.private_key))
    wait_for_confirmation(algod, txid, 4)


def compile_orchestrator() -> None:
    """Compile both orch and __storage default templates if not
    already cached. Targets AVM v10 to match puya-sol's emit (so
    chunks aren't a downgrade)."""
    if not (ORCH_OUT / "UrosOrchestrator.approval.teal").exists():
        ORCH_OUT.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(
            [str(PUYAPY_BIN), str(ORCH_PY), "--out-dir", str(ORCH_OUT),
             "--output-bytecode", "--target-avm-version", "10"],
            check=True, capture_output=True,
        )
    if not (STORAGE_OUT / "UrosStorage.approval.teal").exists():
        STORAGE_OUT.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(
            [str(PUYAPY_BIN), str(STORAGE_PY), "--out-dir", str(STORAGE_OUT),
             "--output-bytecode", "--target-avm-version", "10"],
            check=True, capture_output=True,
        )


def deploy_orchestrator(algod: AlgodClient, sender: SigningAccount) -> int:
    compile_orchestrator()
    approval = base64.b64decode(algod.compile(
        (ORCH_OUT / "UrosOrchestrator.approval.teal").read_text())["result"])
    clear = base64.b64decode(algod.compile(
        (ORCH_OUT / "UrosOrchestrator.clear.teal").read_text())["result"])

    sp = algod.suggested_params()
    txn = ApplicationCreateTxn(
        sender=sender.address, sp=sp,
        on_complete=OnComplete.NoOpOC,
        approval_program=approval, clear_program=clear,
        global_schema=StateSchema(num_uints=16, num_byte_slices=16),
        local_schema=StateSchema(num_uints=0, num_byte_slices=0),
        app_args=[_arc4_selector("init()void")], extra_pages=3,
    )
    txid = algod.send_transaction(txn.sign(sender.private_key))
    orch_id = int(wait_for_confirmation(algod, txid, 4)["application-index"])
    # Box MBR: 4 codeboxes × ~8 KB + per-method clen/csel entries.
    _fund(algod, sender, _app_addr(orch_id), 200_000_000)
    return orch_id


def _substitute_orch_id(teal: str, orch_id: int) -> str:
    return teal.replace("TMPL_UROS_ORCH_APP_ID", str(orch_id))


def _substitute_main_id(teal: str, main_id: int) -> str:
    """Chunks reference TMPL_UROS_MAIN_APP_ID for cross-app reads of
    main's __og_sender / __og_value globals (Pass 2/3 of the splitter).
    Substituted at deploy time after main has been created."""
    return teal.replace("TMPL_UROS_MAIN_APP_ID", str(main_id))


def _substitute_uros_ids(teal: str, orch_id: int, main_id: int) -> str:
    return _substitute_main_id(_substitute_orch_id(teal, orch_id), main_id)


def _compile_teal(algod: AlgodClient, teal: str) -> bytes:
    return base64.b64decode(algod.compile(teal)["result"])


@dataclass
class SplitDeployment:
    main_id: int
    storage_id: int
    orch_id: int
    app_spec: au.Arc56Contract


def _stream_to_box(algod: AlgodClient, sender: SigningAccount, orch_id: int,
                   write_sel: bytes, box_name: bytes, data: bytes,
                   extra_args_pre: list[bytes] | None = None) -> None:
    chunk_size = 1024
    for offset in range(0, len(data), chunk_size):
        piece = data[offset : offset + chunk_size]
        sp = algod.suggested_params()
        off_b = offset.to_bytes(8, "big")
        data_b = len(piece).to_bytes(2, "big") + piece
        app_args = [write_sel] + (extra_args_pre or []) + [off_b, data_b]
        txn = ApplicationCallTxn(
            sender=sender.address, sp=sp, index=orch_id,
            on_complete=OnComplete.NoOpOC,
            app_args=app_args,
            boxes=[(orch_id, box_name)] + [EMPTY_BOX] * 7,
        )
        wait_for_confirmation(algod,
            algod.send_transaction(txn.sign(sender.private_key)), 4)


def _setup_orch_with_chunks(
    algod: AlgodClient, sender: SigningAccount,
    orch_id: int, storage_id: int,
    storage_default_bytes: bytes,
    chunks_with_full_sigs: list[dict],
) -> None:
    """Run the orch ceremony: set_storage, setup boxes, stream
    bytes, register selectors."""
    # set_storage
    sp = algod.suggested_params()
    txn = ApplicationCallTxn(
        sender=sender.address, sp=sp, index=orch_id,
        on_complete=OnComplete.NoOpOC,
        app_args=[_arc4_selector("set_storage(uint64)void"),
                  storage_id.to_bytes(8, "big")],
    )
    wait_for_confirmation(algod,
        algod.send_transaction(txn.sign(sender.private_key)), 4)

    # setup_default_box
    if not _box_exists_with_size(algod, orch_id, b"__codebox_default",
                                 len(storage_default_bytes)):
        sp = algod.suggested_params()
        sp.fee = sp.min_fee * 3
        txn = ApplicationCallTxn(
            sender=sender.address, sp=sp, index=orch_id,
            on_complete=OnComplete.NoOpOC,
            app_args=[_arc4_selector("setup_default_box(uint64)void"),
                      len(storage_default_bytes).to_bytes(8, "big")],
            boxes=[(orch_id, b"__codebox_default")] + [EMPTY_BOX] * 7,
        )
        wait_for_confirmation(algod,
            algod.send_transaction(txn.sign(sender.private_key)), 4)
        _stream_to_box(algod, sender, orch_id,
                       _arc4_selector("write_default(uint64,byte[])void"),
                       b"__codebox_default", storage_default_bytes)

    # setup + stream each chunk
    setup_chunk_sel = _arc4_selector("setup_chunk_box(uint64,uint64)void")
    write_chunk_sel = _arc4_selector("write_chunk(uint64,uint64,byte[])void")
    for ci, chunk in enumerate(chunks_with_full_sigs):
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
            app_args=[setup_chunk_sel,
                      ci.to_bytes(8, "big"),
                      len(chunk_bytes).to_bytes(8, "big")],
            boxes=[(orch_id, chunk_box), (orch_id, clen_box)] + [EMPTY_BOX] * 6,
        )
        wait_for_confirmation(algod,
            algod.send_transaction(txn.sign(sender.private_key)), 4)
        _stream_to_box(algod, sender, orch_id, write_chunk_sel,
                       chunk_box, chunk_bytes,
                       extra_args_pre=[ci.to_bytes(8, "big")])

    # register selectors
    register_sel = _arc4_selector("register_chunk_method(byte[],uint64)void")
    for ci, chunk in enumerate(chunks_with_full_sigs):
        for full_sig in chunk["full_sigs"]:
            sel = _arc4_selector(full_sig)
            csel_box = b"csel_" + sel
            sel_arg = len(sel).to_bytes(2, "big") + sel
            sp = algod.suggested_params()
            txn = ApplicationCallTxn(
                sender=sender.address, sp=sp, index=orch_id,
                on_complete=OnComplete.NoOpOC,
                app_args=[register_sel, sel_arg, ci.to_bytes(8, "big")],
                boxes=[(orch_id, csel_box)] + [EMPTY_BOX] * 7,
            )
            wait_for_confirmation(algod,
                algod.send_transaction(txn.sign(sender.private_key)), 4)


def _box_exists_with_size(algod: AlgodClient, app_id: int, name: bytes,
                          expected_size: int) -> bool:
    try:
        info = algod.application_box_by_name(app_id, name)
    except Exception:
        return False
    return len(base64.b64decode(info.get("value", ""))) == expected_size


def deploy_split_app(
    algod: AlgodClient,
    sender: SigningAccount,
    name: str,
    orch_id: int,
    app_args: list[bytes] | None = None,
    fund_amount: int = 5_000_000,
) -> SplitDeployment:
    """Deploy a split contract under the new 3-contract architecture.

    Steps (per call):
      1. Substitute TMPL_UROS_ORCH_APP_ID into main + chunk TEAL,
         compile via algod.
      2. Deploy __storage with **main's bytecode** initially. AppCreate
         runs the user contract's state-var inits on __storage.
      3. UpdateApplication __storage → thin uros_storage default
         bytes. Sender = deployer; main's __delegate_update admits
         the update (no orch involved yet).
      4. set_orch on __storage; set_storage on orch.
      5. Set up orch's chunk boxes; stream bytes; register selectors.
      6. Deploy main as a separate app (for user-direct calls).
      7. Call main.__postInit(args) → dance routes to __storage's
         __postInit chunk → state init complete.
    """
    contract_dir = OUT_DIR / name
    deploy_tmpl = json.loads((contract_dir / "deploy.uros.json").read_text())
    arc56_path = contract_dir / f"{name}.arc56.json"
    # Lightweight method-sig parser: pull each method's name + arg types
    # + return type from raw arc56 JSON. Sidesteps algokit's ABI parser
    # which rejects int256 (puya emits int256 for some signed-int returns
    # like Hub's eliminateDeficit, which AAVE V4's source has).
    arc56_raw = json.loads(arc56_path.read_text())
    method_signatures: dict[str, dict] = {}
    for m in arc56_raw.get("methods", []):
        method_signatures[m["name"]] = m
    # Best-effort algokit Arc56Contract — needed for the optional
    # __postInit dance below. Skip for contracts whose spec doesn't
    # parse (e.g. Hub: int256). The harness still works without it
    # since __postInit-via-dance is optional.
    try:
        app_spec = au.Arc56Contract.from_json(arc56_path.read_text())
    except Exception:
        app_spec = None

    # 1. Substitute orch_id into main TEAL and compile. Main itself
    # doesn't reference TMPL_UROS_MAIN_APP_ID — only chunks do — so we
    # can compile main's bytecode now without knowing main_id.
    main_teal = (contract_dir / f"{name}.approval.teal").read_text()
    main_clear_teal = (contract_dir / f"{name}.clear.teal").read_text()
    main_approval = _compile_teal(algod, _substitute_orch_id(main_teal, orch_id))
    main_clear = _compile_teal(algod, main_clear_teal)

    # Read storage default bytes (already compiled by compile_orchestrator).
    storage_default = (STORAGE_OUT / "UrosStorage.approval.bin").read_bytes()
    storage_clear = (STORAGE_OUT / "UrosStorage.clear.bin").read_bytes()

    # 2. Deploy __storage WITH MAIN'S BYTECODE so AppCreate runs the
    # user contract's state-var inits.
    sp = algod.suggested_params()
    txn = ApplicationCreateTxn(
        sender=sender.address, sp=sp,
        on_complete=OnComplete.NoOpOC,
        approval_program=main_approval, clear_program=main_clear,
        global_schema=StateSchema(num_uints=16, num_byte_slices=16),
        local_schema=StateSchema(num_uints=0, num_byte_slices=0),
        app_args=app_args or [], extra_pages=3,
    )
    txid = algod.send_transaction(txn.sign(sender.private_key))
    storage_id = int(wait_for_confirmation(algod, txid, 4)["application-index"])
    _fund(algod, sender, _app_addr(storage_id), fund_amount)

    # 3. UpdateApplication __storage → thin admit-update default.
    # Sender = deployer; main's __delegate_update admits.
    sp = algod.suggested_params()
    txn = ApplicationCallTxn(
        sender=sender.address, sp=sp, index=storage_id,
        on_complete=OnComplete.UpdateApplicationOC,
        approval_program=storage_default, clear_program=storage_clear,
        app_args=[_arc4_selector("__delegate_update()void")],
    )
    wait_for_confirmation(algod,
        algod.send_transaction(txn.sign(sender.private_key)), 4)

    # 4. __storage.set_orch(orch_id).
    sp = algod.suggested_params()
    txn = ApplicationCallTxn(
        sender=sender.address, sp=sp, index=storage_id,
        on_complete=OnComplete.NoOpOC,
        app_args=[_arc4_selector("set_orch(uint64)void"),
                  orch_id.to_bytes(8, "big")],
    )
    wait_for_confirmation(algod,
        algod.send_transaction(txn.sign(sender.private_key)), 4)

    # 5. Deploy main BEFORE compiling chunks. Chunks reference main's
    # app id via TMPL_UROS_MAIN_APP_ID for cross-app reads of __og_sender
    # / __og_value, so we need main_id known before chunk TEAL compiles.
    sp = algod.suggested_params()
    txn = ApplicationCreateTxn(
        sender=sender.address, sp=sp,
        on_complete=OnComplete.NoOpOC,
        approval_program=main_approval, clear_program=main_clear,
        global_schema=StateSchema(num_uints=16, num_byte_slices=16),
        local_schema=StateSchema(num_uints=0, num_byte_slices=0),
        app_args=app_args or [], extra_pages=3,
    )
    txid = algod.send_transaction(txn.sign(sender.private_key))
    main_id = int(wait_for_confirmation(algod, txid, 4)["application-index"])
    _fund(algod, sender, _app_addr(main_id), 1_000_000)

    # 6. Build chunks list. Now substitute BOTH orch_id AND main_id —
    # the chunk's app_global_get_ex(MAIN, "__og_sender") read needs
    # main's real app id baked in.
    chunks_with_full_sigs = []
    for ci, c in enumerate(deploy_tmpl["chunks"]):
        chunk_teal = (contract_dir / "__uros_split" / f"chunk_{ci}"
                      / f"{c['name']}.approval.teal").read_text()
        chunk_bytes = _compile_teal(
            algod, _substitute_uros_ids(chunk_teal, orch_id, main_id))
        full_sigs = []
        for mname in c["methods"]:
            m = method_signatures.get(mname)
            if m is None:
                raise AssertionError(
                    f"split method '{mname}' not in arc56 for {name}")
            arg_types = ",".join(a.get("type", "") for a in m.get("args", []))
            ret = (m.get("returns") or {}).get("type") or "void"
            full_sigs.append(f"{mname}({arg_types}){ret}")
        chunks_with_full_sigs.append({
            **c, "approval_hex": chunk_bytes.hex(),
            "full_sigs": full_sigs,
        })

    # 7. orch setup (set_storage + boxes + chunks + selectors).
    _setup_orch_with_chunks(algod, sender, orch_id, storage_id,
                            storage_default, chunks_with_full_sigs)

    # 8. main.__postInit dance, routes through orch to __storage.
    if app_args and app_spec is not None:
        _call_postinit_via_dance(algod, sender, main_id, orch_id, storage_id,
                                 app_spec, list(app_args))

    return SplitDeployment(
        main_id=main_id, storage_id=storage_id, orch_id=orch_id,
        app_spec=app_spec,
    )


def _call_postinit_via_dance(
    algod: AlgodClient, sender: SigningAccount,
    main_id: int, orch_id: int, storage_id: int,
    app_spec: au.Arc56Contract, app_args: list[bytes],
) -> None:
    """Call main.__postInit which forwards via inner-call to orch.
    The orch dances to __storage to run the actual __postInit chunk
    (which writes state init)."""
    postinit = None
    for m in (app_spec.methods or []):
        if m.name == "__postInit":
            postinit = m
            break
    if postinit is None or len(app_args) < len(postinit.args):
        return
    arg_types = ",".join(getattr(a, "type", "") for a in postinit.args)
    sig = f"__postInit({arg_types})void"
    sel = _arc4_selector(sig)
    call_args = [sel] + list(app_args)[: len(postinit.args)]
    sp = algod.suggested_params()
    sp.fee = sp.min_fee * 16  # main → orch → 3-itxn dance + buffer
    txn = ApplicationCallTxn(
        sender=sender.address, sp=sp, index=main_id,
        on_complete=OnComplete.NoOpOC, app_args=call_args,
        foreign_apps=[orch_id, storage_id],
    )
    signer = AccountTransactionSigner(sender.private_key)
    atc = AtomicTransactionComposer()
    atc.add_transaction(TransactionWithSigner(txn, signer))
    try:
        atc = au.populate_app_call_resources(atc, algod)
    except Exception:
        pass
    try:
        atc.execute(algod, 4)
    except Exception:
        # Mirror the legacy harness: swallow __postInit errors so
        # tests that don't actually rely on full init can still
        # exercise direct (non-init-dependent) methods.
        pass
