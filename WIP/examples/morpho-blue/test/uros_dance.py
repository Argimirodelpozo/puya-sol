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
# Local cache for orch + storage default bytecode. Per-example
# cache dir so a parallel aave-v4 run targeting v10 doesn't clobber
# this example's v12 cached compiles (they share the
# tests/uros-splitter/out dir otherwise).
ORCH_OUT = Path(__file__).parent.parent / "out" / "_uros_cache" / "Orchestrator"
STORAGE_OUT = Path(__file__).parent.parent / "out" / "_uros_cache" / "Storage"

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
             "--output-bytecode", "--target-avm-version", "12"],
            check=True, capture_output=True,
        )
    if not (STORAGE_OUT / "UrosStorage.approval.teal").exists():
        STORAGE_OUT.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(
            [str(PUYAPY_BIN), str(STORAGE_PY), "--out-dir", str(STORAGE_OUT),
             "--output-bytecode", "--target-avm-version", "12"],
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


def _substitute_pure_helper_ids(
    teal: str, helper_app_ids: dict[str, int],
) -> str:
    """Each entry is `PURE_HELPER_<name>_<n>_APP_ID -> app_id`. We
    substitute the TMPL_-prefixed form into the TEAL (the prefix puya
    bakes in at compile time)."""
    for tv, app_id in helper_app_ids.items():
        teal = teal.replace("TMPL_" + tv, str(app_id))
    return teal


def _deploy_pure_helpers(
    algod: AlgodClient, sender: SigningAccount,
    contract_dir: Path,
) -> dict[str, int]:
    """Deploy each PureHelper sidecar app listed in pure_helpers.json,
    return {template_var → app_id}. Helpers are tiny single-method
    contracts (no state, no creation args), so AppCreate is plain.
    Idempotency: caller is expected to deploy fresh each module run
    (the splitter dance fixture is module-scoped already).

    Ordering: pure helpers can call other pure helpers (via inner-
    txn ApplicationCall). The callee's helper id has to be known at
    the caller's compile time — algod's TEAL compiler rejects
    `intcblock TMPL_X` operands as non-integer. So we walk the
    dep graph by scanning each helper's TEAL for unresolved TMPL_
    references and deploy in topological order.
    """
    pure_helpers_path = contract_dir / "pure_helpers.json"
    if not pure_helpers_path.exists():
        return {}
    spec = json.loads(pure_helpers_path.read_text())
    helpers = spec.get("helpers", [])
    # Index by template_var for the topo walk.
    by_tv: dict[str, dict] = {h["template_var"]: h for h in helpers}
    teals: dict[str, str] = {}
    deps: dict[str, set[str]] = {}
    for h in helpers:
        tv = h["template_var"]
        name = h["contract_name"]
        teal = (contract_dir / f"{name}.approval.teal").read_text()
        teals[tv] = teal
        # Scan the TEAL for TMPL_PURE_HELPER_*_APP_ID references —
        # everything BUT this helper's own var is a dependency.
        d: set[str] = set()
        for other in by_tv:
            if other == tv:
                continue
            if "TMPL_" + other in teal:
                d.add(other)
        deps[tv] = d

    # Topological sort. Pure functions can be mutually recursive in
    # principle, but Solidity disallows this for `pure` functions
    # (no recursion through state-mutability). Cycle would deadlock
    # the deploy — assert no cycles.
    deployed: dict[str, int] = {}
    pending = set(by_tv.keys())
    while pending:
        ready = {tv for tv in pending if deps[tv].issubset(deployed.keys())}
        if not ready:
            raise RuntimeError(
                f"_deploy_pure_helpers: dependency cycle / unresolvable "
                f"refs among: {pending!r}")
        for tv in sorted(ready):
            h = by_tv[tv]
            name = h["contract_name"]
            # Substitute any caller helper's TMPL refs to known ids
            # before compiling.
            teal = _substitute_pure_helper_ids(teals[tv], deployed)
            clear_teal = (contract_dir / f"{name}.clear.teal").read_text()
            approval = _compile_teal(algod, teal)
            clear = _compile_teal(algod, clear_teal)
            sp = algod.suggested_params()
            txn = ApplicationCreateTxn(
                sender=sender.address, sp=sp,
                on_complete=OnComplete.NoOpOC,
                approval_program=approval, clear_program=clear,
                global_schema=StateSchema(num_uints=0, num_byte_slices=0),
                local_schema=StateSchema(num_uints=0, num_byte_slices=0),
                extra_pages=0,
            )
            txid = algod.send_transaction(txn.sign(sender.private_key))
            app_id = int(wait_for_confirmation(algod, txid, 4)["application-index"])
            deployed[tv] = app_id
            pending.remove(tv)
    return deployed


def _substitute_orch_id(teal: str, orch_id: int) -> str:
    return teal.replace("TMPL_UROS_ORCH_APP_ID", str(orch_id))


def _substitute_main_id(teal: str, main_id: int) -> str:
    """Chunks reference TMPL_UROS_MAIN_APP_ID for cross-app reads of
    main's __og_sender / __og_value globals (Pass 2/3 of the splitter).
    Substituted at deploy time after main has been created."""
    return teal.replace("TMPL_UROS_MAIN_APP_ID", str(main_id))


def _substitute_storage_id(teal: str, storage_id: int) -> str:
    """Main's stub references TMPL_UROS_STORAGE_APP_ID for the
    pay-forward shim — when the user's call carries a paired pay txn,
    main forwards the value to __storage's address (computed at
    runtime via app_params_get(STORAGE_APP_ID, AppAddress)).
    Substituted at deploy time after __storage is created."""
    return teal.replace("TMPL_UROS_STORAGE_APP_ID", str(storage_id))


def _substitute_main_uros_ids(teal: str, orch_id: int, storage_id: int) -> str:
    """Apply the substitutions main's TEAL needs (orch_id + storage_id).
    Main doesn't reference TMPL_UROS_MAIN_APP_ID — only chunks do."""
    return _substitute_storage_id(
        _substitute_orch_id(teal, orch_id), storage_id)


def _substitute_chunk_uros_ids(
    teal: str, orch_id: int, main_id: int, storage_id: int
) -> str:
    """Chunks substitute all three: orch (for orc-guards if any),
    main (for og_sender/og_value reads), storage (in case any chunk
    method's emitted code references it — currently none do, but the
    template var is declared in chunk options.json for safety)."""
    return _substitute_storage_id(
        _substitute_main_id(_substitute_orch_id(teal, orch_id), main_id),
        storage_id,
    )


def _app_addr_bytes32(app_id: int) -> bytes:
    """Compute the 32-byte raw account address for an app id (the
    sha512_256("appID" + uint64_be) digest that AVM derives at runtime
    via `app_params_get AppAddress`)."""
    return hashlib.new("sha512_256", b"appID" + app_id.to_bytes(8, "big")).digest()


def _patch_inner_txn_senders(teal: str, sender_app_id: int) -> str:
    """Patch TEAL so every `itxn_submit` emits with Sender =
    sender_app_id's app address (unless user code explicitly set
    Sender earlier in the same itxn group via `itxn_field Sender`).

    Used by both main and chunk TEAL post-processing under the
    bidirectional uros rekey (main.AuthAddr=storage,
    storage.AuthAddr=main):

      * Main TEAL (sender_app_id = storage_id): main can't sign for
        its own address (main.auth=storage), so default-Sender inner
        txns from main are rejected. Setting Sender = storage_addr is
        admissible because main signs for storage via storage.auth=main.
      * Chunk TEAL (sender_app_id = main_id): chunks run inside
        storage's account; default-Sender = storage_addr leaks the
        wrong identity to external callees. Setting Sender = main_addr
        is admissible because storage signs for main via main.auth=storage.

    The AWST-level patch in `UrosSplitter::patchInnerTxnSenderExpr`
    only runs on ABI-entry methods (not internal helpers, whose bodies
    are shared with main via shallowCloneContract). This TEAL pass
    covers every itxn_submit regardless of which method it lives in.

    Insertion sequence (4 opcodes, ~6 B per itxn_submit):
        pushint <sender_app_id>
        app_params_get AppAddress
        assert
        itxn_field Sender
        itxn_submit              ← original line
    """
    lines = teal.split("\n")
    out: list[str] = []
    for line in lines:
        if line.strip().startswith("itxn_submit"):
            # Walk backward inside the current itxn's field list. Stop
            # at the previous itxn_begin / itxn_next / itxn_submit so we
            # only inspect THIS itxn's fields, not an earlier one in a
            # multi-txn group. We need to know two things:
            #   1. Does it already have `itxn_field Sender`? (skip)
            #   2. Does it have `itxn_field ApplicationID`? (only patch
            #      APPL-type inner txns — Pay/Asset txns with
            #      default Sender usually do what user code intended,
            #      and the deploy-time rekey Pay txns specifically need
            #      Sender=main_addr default to rekey the right account.)
            saw_sender = False
            saw_app_id = False
            for prev in reversed(out):
                s = prev.strip()
                if s.startswith("itxn_begin") or s.startswith("itxn_next") \
                        or s.startswith("itxn_submit"):
                    break
                if s.startswith("itxn_field Sender"):
                    saw_sender = True
                if s.startswith("itxn_field ApplicationID"):
                    saw_app_id = True
            if saw_app_id and not saw_sender:
                # Push the literal 32-byte address rather than
                # `app_params_get AppAddress` so the deployed contract
                # doesn't depend on the sender-app being in foreign-apps
                # at every call site (algokit's populate_app_call_resources
                # auto-detects box refs but not app_params_get probes).
                addr_hex = _app_addr_bytes32(sender_app_id).hex()
                out.append(f"    pushbytes 0x{addr_hex}")
                out.append("    itxn_field Sender")
        out.append(line)
    return "\n".join(out)


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

    # AVM hard cap on installed program size: 4 pages × 2048 B = 8192 B
    # (extra_pages max is 3). Chunks must be bin-packed under that cap.
    # If you hit this assertion, rebalance compile_all.sh's SPLIT_GROUPS
    # for the contract — moving methods between groups, splitting big
    # methods further, or pinning costly methods to main.
    MAX_CHUNK_SIZE = 8192

    # setup + stream each chunk
    setup_chunk_sel = _arc4_selector("setup_chunk_box(uint64,uint64)void")
    write_chunk_sel = _arc4_selector("write_chunk(uint64,uint64,byte[])void")
    for ci, chunk in enumerate(chunks_with_full_sigs):
        chunk_bytes = bytes.fromhex(chunk["approval_hex"])
        assert len(chunk_bytes) <= MAX_CHUNK_SIZE, (
            f"chunk {ci} is {len(chunk_bytes)} B > {MAX_CHUNK_SIZE} B "
            f"AVM page cap. Methods: {chunk['full_sigs']!r}. "
            f"Rebalance SPLIT_GROUPS in compile_all.sh.")
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


def _register_chain_groups(
    algod: AlgodClient, sender: SigningAccount,
    orch_id: int, contract_dir: Path,
    chunks_with_full_sigs: list[dict],
    main_method_signatures: dict,
) -> None:
    """Register cross-chunk piece chains with orch.

    For each entry in chain_groups.json:
      - Look up the primary method's signature in main's arc56 (the
        primary stays on main as a stub).
      - For each piece, find its hosting chunk_idx by name and pull its
        signature from THAT chunk's arc56 (pieces are only on chunks).
      - Pack entries: 8 B chunk_idx (BE) + 4 B piece selector each.
      - Call orch.register_chunk_method_chain(primary_sel, entries).

    Idempotent: re-running on an already-registered chain just rewrites
    the chain_for_selector box value (orch doesn't reject repeats).
    """
    chain_groups_path = contract_dir / "chain_groups.json"
    if not chain_groups_path.exists():
        return
    chain_groups = json.loads(chain_groups_path.read_text())
    groups = chain_groups.get("groups", [])
    if not groups:
        return

    # piece_method_name → containing chunk_idx (linear scan; chunks list
    # is small so a dict isn't worth the boilerplate).
    piece_to_chunk: dict[str, int] = {}
    for ci, c in enumerate(chunks_with_full_sigs):
        for mname in c["methods"]:
            piece_to_chunk[mname] = ci

    register_sel = _arc4_selector(
        "register_chunk_method_chain(byte[],byte[])void")

    for g in groups:
        primary_name = g["primary_method"]
        primary_sig_meta = main_method_signatures.get(primary_name)
        if primary_sig_meta is None:
            raise AssertionError(
                f"chain group primary '{primary_name}' not in main arc56 "
                f"(was the original method removed by --uros-splitter?)")
        a_types = ",".join(a.get("type", "") for a in primary_sig_meta.get("args", []))
        ret = (primary_sig_meta.get("returns") or {}).get("type") or "void"
        primary_sig = f"{primary_name}({a_types}){ret}"
        primary_sel = _arc4_selector(primary_sig)

        # Build packed entries: 12 B per piece.
        entries = b""
        for piece_name in g["piece_methods"]:
            ci = piece_to_chunk.get(piece_name)
            if ci is None:
                raise AssertionError(
                    f"chain piece '{piece_name}' not on any chunk; check "
                    f"that --uros-splitter group lists include the piece "
                    f"name (FunctionSplitter doesn't auto-distribute).")
            chunk_meta = chunks_with_full_sigs[ci]
            chunk_arc56 = json.loads(
                (contract_dir / "__uros_split" / f"chunk_{ci}"
                 / f"{chunk_meta['name']}.arc56.json").read_text())
            chunk_method_sigs = {m["name"]: m
                                 for m in chunk_arc56.get("methods", [])}
            pm = chunk_method_sigs.get(piece_name)
            if pm is None:
                raise AssertionError(
                    f"chain piece '{piece_name}' not in chunk {ci} arc56")
            p_types = ",".join(a.get("type", "") for a in pm.get("args", []))
            p_ret = (pm.get("returns") or {}).get("type") or "void"
            piece_sig = f"{piece_name}({p_types}){p_ret}"
            piece_sel = _arc4_selector(piece_sig)
            entries += ci.to_bytes(8, "big") + piece_sel

        # ABI byte[] arg encoding: uint16 BE length prefix + raw bytes.
        primary_arg = len(primary_sel).to_bytes(2, "big") + primary_sel
        entries_arg = len(entries).to_bytes(2, "big") + entries
        cchain_box = b"cchain_" + primary_sel
        sp = algod.suggested_params()
        txn = ApplicationCallTxn(
            sender=sender.address, sp=sp, index=orch_id,
            on_complete=OnComplete.NoOpOC,
            app_args=[register_sel, primary_arg, entries_arg],
            boxes=[(orch_id, cchain_box)] + [EMPTY_BOX] * 7,
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

    # 0. Deploy any --deploy-pure-helpers sidecar apps first so we can
    # bake their app ids into main + chunk TEAL. Each helper is a
    # one-method standalone Contract with its own approval/clear; the
    # rewritten call sites in main + chunks reference each helper by
    # TMPL_PURE_HELPER_<name>_<n>_APP_ID. Empty if no --deploy-pure-helpers.
    pure_helper_app_ids = _deploy_pure_helpers(algod, sender, contract_dir)

    # 1. Read main TEAL. Substitution is split across steps because of
    # a chicken-and-egg: main references TMPL_UROS_STORAGE_APP_ID for
    # the pay-forward shim, but __storage is deployed by AppCreate'ing
    # with main's bytecode itself. So we compile main twice:
    #   v1: storage_id placeholder (0) — used for __storage's AppCreate.
    #       The shim never runs during AppCreate so the placeholder
    #       value doesn't matter. State schema is identical to v2.
    #   v2: real storage_id — used for main's own AppCreate (step 5).
    main_teal = (contract_dir / f"{name}.approval.teal").read_text()
    main_clear_teal = (contract_dir / f"{name}.clear.teal").read_text()
    main_clear = _compile_teal(algod, main_clear_teal)
    main_approval_v1 = _compile_teal(
        algod, _substitute_pure_helper_ids(
            _substitute_main_uros_ids(main_teal, orch_id, 0),
            pure_helper_app_ids))

    # Read storage default bytes (already compiled by compile_orchestrator).
    storage_default = (STORAGE_OUT / "UrosStorage.approval.bin").read_bytes()
    storage_clear = (STORAGE_OUT / "UrosStorage.clear.bin").read_bytes()

    # 2. Deploy __storage WITH MAIN'S BYTECODE (v1) so AppCreate runs the
    # user contract's state-var inits.
    sp = algod.suggested_params()
    txn = ApplicationCreateTxn(
        sender=sender.address, sp=sp,
        on_complete=OnComplete.NoOpOC,
        approval_program=main_approval_v1, clear_program=main_clear,
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

    # 5. Compile main_v2 (with real storage_id substituted) and deploy
    # main BEFORE compiling chunks. Chunks reference main's app id via
    # TMPL_UROS_MAIN_APP_ID for cross-app reads of __og_sender /
    # __og_value, so we need main_id known before chunk TEAL compiles.
    print(f"[uros_dance] compiling main_v2 with orch_id={orch_id} storage_id={storage_id}")
    # Patch main TEAL: every inner txn that doesn't already set Sender
    # gets Sender=storage_addr injected. main.AuthAddr=storage means main
    # can't sign for its own address, but it CAN sign for storage (via
    # storage.AuthAddr=main). So all inner txns must claim Sender=storage.
    main_teal_patched = _patch_inner_txn_senders(main_teal, storage_id)
    main_approval_v2 = _compile_teal(
        algod, _substitute_pure_helper_ids(
            _substitute_main_uros_ids(main_teal_patched, orch_id, storage_id),
            pure_helper_app_ids))
    sp = algod.suggested_params()
    txn = ApplicationCreateTxn(
        sender=sender.address, sp=sp,
        on_complete=OnComplete.NoOpOC,
        approval_program=main_approval_v2, clear_program=main_clear,
        global_schema=StateSchema(num_uints=16, num_byte_slices=16),
        local_schema=StateSchema(num_uints=0, num_byte_slices=0),
        app_args=app_args or [], extra_pages=3,
    )
    txid = algod.send_transaction(txn.sign(sender.private_key))
    main_id = int(wait_for_confirmation(algod, txid, 4)["application-index"])
    _fund(algod, sender, _app_addr(main_id), 1_000_000)

    # 5b. Bidirectional mirror rekey: main <-> __storage.
    # Each app rekeys its own account to the other, so:
    #   * __storage->main: main can sign for __storage's account, used by
    #     main's stub when issuing the dispatch itxn (Sender=storage_addr
    #     override since main can't sign for itself post-rekey-to-storage).
    #   * main->__storage: __storage can sign for main's account, used by
    #     chunk-emitted itxns whose Sender is overridden to main_addr
    #     (Pass 5 in UrosSplitter.cpp). External contracts called from
    #     chunks therefore observe Sender=main_addr — main is both the
    #     in-address and the out-address from any caller's POV.
    #
    # IMPORTANT ordering: rekey __storage -> main FIRST. After main rekeys
    # to __storage, main loses authority over its own account; if we then
    # try to issue a fresh top-level call to main.__rekey_to_storage(...),
    # AVM still admits because the user (sponsor) signs that outer txn,
    # but main's own outbound itxns (the rekey pay submitted by
    # __rekey_to_storage's body) need authForSender(main_addr) which will
    # only succeed if __storage is already main's auth — circular.
    # Reverse the order: __storage->main first (uses default Sender,
    # __storage hasn't been rekeyed so this works), then main->__storage
    # (also uses default Sender, main hasn't been rekeyed yet).
    main_pubkey = encoding.decode_address(_app_addr(main_id))
    storage_pubkey = encoding.decode_address(_app_addr(storage_id))
    sp = algod.suggested_params()
    sp.fee = sp.min_fee * 2
    txn = ApplicationCallTxn(
        sender=sender.address, sp=sp, index=storage_id,
        on_complete=OnComplete.NoOpOC,
        app_args=[
            _arc4_selector("__rekey_to_main(address)void"),
            main_pubkey,
        ],
    )
    wait_for_confirmation(algod,
        algod.send_transaction(txn.sign(sender.private_key)), 4)
    sp = algod.suggested_params()
    sp.fee = sp.min_fee * 2
    txn = ApplicationCallTxn(
        sender=sender.address, sp=sp, index=main_id,
        on_complete=OnComplete.NoOpOC,
        app_args=[
            _arc4_selector("__rekey_to_storage(address)void"),
            storage_pubkey,
        ],
    )
    wait_for_confirmation(algod,
        algod.send_transaction(txn.sign(sender.private_key)), 4)
    # Verify both auth_addrs are pinned correctly.
    main_info = algod.account_info(_app_addr(main_id))
    storage_info = algod.account_info(_app_addr(storage_id))
    assert main_info.get("auth-addr") == _app_addr(storage_id), (
        f"main.auth_addr={main_info.get('auth-addr')} != storage_addr={_app_addr(storage_id)}")
    assert storage_info.get("auth-addr") == _app_addr(main_id), (
        f"storage.auth_addr={storage_info.get('auth-addr')} != main_addr={_app_addr(main_id)}")
    print(f"[uros_dance] mirror rekey complete: "
          f"main.auth=storage, storage.auth=main")

    # 6. Build chunks list. Substitute orch_id, main_id, AND storage_id
    # — the chunk's app_global_get_ex(MAIN, ...) needs main, and any
    # pay-forward / storage-aware emit in chunks would need storage too.
    # Method signatures are looked up first in main's arc56 (covers
    # ABI-public methods that have stubs on main), then falling back to
    # the chunk's own arc56 (covers FunctionSplitter pieces, which only
    # exist on their containing chunk).
    chunks_with_full_sigs = []
    for ci, c in enumerate(deploy_tmpl["chunks"]):
        chunk_dir = contract_dir / "__uros_split" / f"chunk_{ci}"
        chunk_teal = (chunk_dir / f"{c['name']}.approval.teal").read_text()
        # Patch chunk TEAL: every inner txn that doesn't already set
        # Sender gets Sender=main_addr injected. Chunks run inside
        # storage's account; storage.AuthAddr=main means storage can't
        # sign for itself, but CAN sign for main (via main.AuthAddr=storage).
        # External callees observe Sender=main consistently.
        chunk_teal = _patch_inner_txn_senders(chunk_teal, main_id)
        chunk_bytes = _compile_teal(
            algod, _substitute_pure_helper_ids(
                _substitute_chunk_uros_ids(
                    chunk_teal, orch_id, main_id, storage_id),
                pure_helper_app_ids))
        chunk_arc56 = json.loads(
            (chunk_dir / f"{c['name']}.arc56.json").read_text())
        chunk_method_sigs = {m["name"]: m for m in chunk_arc56.get("methods", [])}
        full_sigs = []
        for mname in c["methods"]:
            m = method_signatures.get(mname) or chunk_method_sigs.get(mname)
            if m is None:
                raise AssertionError(
                    f"split method '{mname}' not in main or chunk arc56 "
                    f"for {name} (chunk {ci})")
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

    # 7b. Register cross-chunk piece chains with orch (if any). Reads
    # chain_groups.json (emitted by main.cpp when --fn-split :cross is
    # used) and packs (chunk_idx, piece_selector) entries per primary
    # method, calling orch.register_chunk_method_chain for each.
    _register_chain_groups(algod, sender, orch_id, contract_dir,
                            chunks_with_full_sigs, method_signatures)

    # 8. main.__postInit dance, routes through orch to __storage.
    # Don't require app_spec — fall back to the raw method_signatures
    # dict from the arc56 JSON (algokit's Arc56Contract parser rejects
    # types like int200 that puya-sol emits).
    if app_args and "__postInit" in method_signatures:
        # Pre-discover static box names from main's TEAL — __postInit
        # may box_create state-init boxes (e.g. SpokeInstance creates
        # _reserves / _userPositions / __gap). Constructor-body
        # inner-calls (like AAVE's old `IAaveOracle.DECIMALS()` check)
        # would need extra_foreign_apps slots, but the AVM 8-ref cap
        # leaves no room past `[storage_id] + 7 boxes`. Source-side
        # workaround for AAVE's Spoke ctor (see Spoke.sol). Other
        # callers needing ctor-side foreign apps must pass them
        # explicitly via this function's `extra_foreign_apps` arg
        # AND drop boxes to fit.
        box_names = _discover_box_names_from_teal(main_teal)
        _call_postinit_via_dance(algod, sender, main_id, orch_id, storage_id,
                                 method_signatures["__postInit"],
                                 list(app_args), box_names=box_names)

    return SplitDeployment(
        main_id=main_id, storage_id=storage_id, orch_id=orch_id,
        app_spec=app_spec,
    )


def _discover_box_names_from_teal(teal: str) -> list[bytes]:
    """Scan main's TEAL for `box_create` calls preceded by a string
    constant — surfaces the static box names __postInit creates so the
    dance call can pre-admit them as box references. Misses boxes
    created with dynamic keys (mappings keyed at runtime), but covers
    array/struct state vars whose box name is the field name."""
    import re
    names: list[bytes] = []
    seen = set()
    # Pattern: pushbytes 0x... // "name"\n... box_create
    # Easier: walk lines, track last quoted string, when we hit
    # box_create the most recent quoted string is the box name.
    last_str: str | None = None
    for line in teal.split("\n"):
        line = line.strip()
        if line.startswith("bytec") or line.startswith("pushbytes"):
            m = re.search(r'"([^"]+)"', line)
            if m:
                last_str = m.group(1)
        elif line.startswith("box_create"):
            if last_str and last_str not in seen:
                seen.add(last_str)
                names.append(last_str.encode())
            last_str = None
    return names


def _discover_state_boxes_for_storage(
    storage_id: int, _postinit: dict,
) -> list[tuple[int, bytes]]:
    """Box references for the dance's __postInit call. Boxes live on
    __storage (the contract that runs __postInit's body), so all refs
    target storage_id."""
    # Find the contract's main TEAL by walking up to the deploy_tmpl
    # path — _call_postinit_via_dance is invoked from inside
    # deploy_split_app where contract_dir is in scope, but to keep the
    # signature simple we re-derive from the current OUT_DIR layout.
    # The caller (deploy_split_app) knows `name`; we can be passed it
    # but for now scan all out/*/*.approval.teal files for the matching
    # selector. Cheaper: hard-coded fallback to common AAVE box names.
    return []  # filled below by the caller after scanning


def _call_postinit_via_dance(
    algod: AlgodClient, sender: SigningAccount,
    main_id: int, orch_id: int, storage_id: int,
    postinit: dict, app_args: list[bytes],
    box_names: list[bytes] | None = None,
    extra_foreign_apps: list[int] | None = None,
) -> None:
    """Call main.__postInit which forwards via inner-call to orch.
    The orch dances to __storage to run the actual __postInit chunk
    (which writes state init).

    `postinit` is the raw arc56 method dict (with `args` list of
    `{name, type}` entries). Used directly so callers don't need an
    Arc56Contract parser instance — see deploy_split_app for context."""
    args_list = postinit.get("args", [])
    if len(app_args) < len(args_list):
        return
    arg_types = ",".join(a.get("type", "") for a in args_list)
    sig = f"__postInit({arg_types})void"
    sel = _arc4_selector(sig)
    call_args = [sel] + list(app_args)[: len(args_list)]
    sp = algod.suggested_params()
    sp.fee = sp.min_fee * 16  # main → orch → 3-itxn dance + buffer
    # __postInit may box_create state-init boxes (e.g. SpokeInstance
    # creates _reserves / _userPositions / __gap / etc. on __storage).
    # populate_app_call_resources can't auto-discover the box names from
    # a failing simulate — pre-admit ALL plausible mapping/array boxes
    # by walking the arc56 raw spec for AppGlobal entries that look
    # box-shaped. The over-admit is harmless; missing-box at runtime is
    # fatal.
    # AVM caps total tx references at 8 (foreign_apps + boxes + accts
    # + assets). __postInit's body lives on main (it isn't in any
    # chunk group), so the box_creates target main_id — pass index 0
    # (current app) for the box refs. We need storage_id in
    # foreign_apps because every main method calls __uros_forward_value
    # which does `app_params_get AppAddress` on TMPL_UROS_STORAGE_APP_ID.
    # 1 foreign_app + 7 boxes = 8 references.
    # __postInit, when chunked into the dance, runs on __storage —
    # box_creates land on storage's app boxes. So the outer txn's
    # box refs target storage. Plus orch_id in foreign_apps so main's
    # forwarding stub can issue the inner-itxn to orch.dispatch.
    fa = [storage_id, orch_id, *(extra_foreign_apps or [])]
    box_refs: list[tuple[int, bytes]] = [
        (storage_id, n) for n in (box_names or [])[:7]
    ]
    txn = ApplicationCallTxn(
        sender=sender.address, sp=sp, index=main_id,
        on_complete=OnComplete.NoOpOC, app_args=call_args,
        foreign_apps=fa,
        boxes=box_refs[: max(0, 8 - len(fa))],
    )
    signer = AccountTransactionSigner(sender.private_key)
    atc = AtomicTransactionComposer()
    # Pre-pad the group with three no-op app calls (re-set orch's
    # storage to its current value — `set_storage(uint64)` is a
    # globalput, idempotent). populate_app_call_resources distributes
    # box / foreign-app refs across the group, so each pad txn buys
    # us 8 more ref slots. Pay txns can't carry box refs, so we use
    # app calls.
    import os
    pad_sel = _arc4_selector("set_storage(uint64)void")
    pad_sp = algod.suggested_params()
    pad_sp.fee = pad_sp.min_fee
    pad_sp.flat_fee = True
    for i in range(3):
        pad_txn = ApplicationCallTxn(
            sender=sender.address, sp=pad_sp, index=orch_id,
            on_complete=OnComplete.NoOpOC,
            app_args=[pad_sel, storage_id.to_bytes(8, "big")],
            note=os.urandom(16) + bytes([i]),
        )
        atc.add_transaction(TransactionWithSigner(pad_txn, signer))
    atc.add_transaction(TransactionWithSigner(txn, signer))
    # Now that __postInit runs via the dance, the orch needs its
    # csel_<sel> + chunk-bytes boxes admitted. populate_app_call_resources
    # walks simulate to find them. Hand-built box refs (state-init
    # boxes on storage) stay; populate ADDS to the set.
    try:
        atc = au.populate_app_call_resources(atc, algod)
    except Exception as e:
        print(f"[uros_dance] populate_app_call_resources failed: {e!r}")
    try:
        atc.execute(algod, 4)
    except Exception as e:
        # Mirror the legacy harness: swallow __postInit errors so
        # tests that don't actually rely on full init can still
        # exercise direct (non-init-dependent) methods. Surface the
        # error message so silently-broken init is at least visible
        # in stdout.
        print(f"[uros_dance] __postInit dance failed: {e!r}")
