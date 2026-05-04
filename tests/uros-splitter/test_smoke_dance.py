"""End-to-end test for --uros-splitter on Smoke.sol.

Verifies the 3-itxn dance: a call to a stubbed method on main (`dec`)
gets routed through the orchestrator, which swaps main's program to
the helper bytes, runs the real `dec` body, and swaps back. State
modifications made by the helper persist in main's app state.

Run after tests/uros-splitter/build.sh has produced the artifacts.
Requires localnet running (`algokit localnet start`).
"""

import base64
from pathlib import Path
import hashlib

import algokit_utils as au
from algokit_utils.models.account import SigningAccount
from algosdk import encoding
from algosdk.transaction import (
    ApplicationCallTxn,
    ApplicationCreateTxn,
    OnComplete,
    PaymentTxn,
    StateSchema,
    assign_group_id,
    wait_for_confirmation,
)
from algosdk.atomic_transaction_composer import (
    AtomicTransactionComposer,
    TransactionWithSigner,
    AccountTransactionSigner,
)
import pytest

HERE = Path(__file__).parent
OUT = HERE / "out"


def _algod():
    config = au.ClientManager.get_default_localnet_config("algod")
    return au.ClientManager.get_algod_client(config)


def _kmd():
    config = au.ClientManager.get_default_localnet_config("kmd")
    return au.ClientManager.get_kmd_client(config)


def _account() -> SigningAccount:
    clients = au.AlgoSdkClients(algod=_algod(), kmd=_kmd())
    return au.AlgorandClient(clients).account.localnet_dispenser()


def _arc4_selector(sig: str) -> bytes:
    return hashlib.new("sha512_256", sig.encode()).digest()[:4]


def _read_bin(path: Path) -> bytes:
    return path.read_bytes()


def _read_teal_compiled(name: str, kind: str) -> bytes:
    """Compile a TEAL file via algod and return its bytes."""
    teal_path = OUT / name / f"{name}.{kind}.teal"
    text = teal_path.read_text()
    result = _algod().compile(text)
    return base64.b64decode(result["result"])


def _fund(addr: str, amt: int):
    sp = _algod().suggested_params()
    sender = _account()
    txn = PaymentTxn(sender.address, sp, addr, amt)
    signed = txn.sign(sender.private_key)
    txid = _algod().send_transaction(signed)
    wait_for_confirmation(_algod(), txid, 4)


def _app_addr(app_id: int) -> str:
    return encoding.encode_address(
        encoding.checksum(b"appID" + app_id.to_bytes(8, "big"))
    )


def _deploy_app(
    name_dir: str,
    bin_prefix: str,
    sender: SigningAccount,
    extra_pages: int = 3,
    app_args: list[bytes] | None = None,
) -> int:
    """Deploy `bin_prefix.approval.bin` from `name_dir/` and return app_id."""
    approval = _read_bin(OUT / name_dir / f"{bin_prefix}.approval.bin")
    clear = _read_bin(OUT / name_dir / f"{bin_prefix}.clear.bin")

    sp = _algod().suggested_params()
    txn = ApplicationCreateTxn(
        sender=sender.address,
        sp=sp,
        on_complete=OnComplete.NoOpOC,
        approval_program=approval,
        clear_program=clear,
        global_schema=StateSchema(num_uints=16, num_byte_slices=16),
        local_schema=StateSchema(num_uints=0, num_byte_slices=0),
        app_args=app_args or [],
        extra_pages=extra_pages,
    )
    signed = txn.sign(sender.private_key)
    txid = _algod().send_transaction(signed)
    result = wait_for_confirmation(_algod(), txid, 4)
    return int(result["application-index"])


def _stream_main(orch_id: int, sender: SigningAccount, data: bytes):
    """Stream `data` into orchestrator's __codebox_main in 1024-B chunks."""
    write_sel = _arc4_selector("write_main(uint64,byte[])void")
    box_name = b"__codebox_main"
    chunk_size = 1024
    for offset in range(0, len(data), chunk_size):
        chunk = data[offset : offset + chunk_size]
        sp = _algod().suggested_params()
        off_b = offset.to_bytes(8, "big")
        data_b = len(chunk).to_bytes(2, "big") + chunk
        EMPTY = (0, b"")
        txn = ApplicationCallTxn(
            sender=sender.address, sp=sp, index=orch_id,
            on_complete=OnComplete.NoOpOC,
            app_args=[write_sel, off_b, data_b],
            boxes=[(orch_id, box_name)] + [EMPTY] * 7,
        )
        txid = _algod().send_transaction(txn.sign(sender.private_key))
        wait_for_confirmation(_algod(), txid, 4)


def _stream_chunk(orch_id: int, sender: SigningAccount, chunk_idx: int, data: bytes):
    """Stream `data` into orchestrator's __codebox_chunk_<idx> in 1024-B chunks."""
    write_sel = _arc4_selector("write_chunk(uint64,uint64,byte[])void")
    box_name = b"__codebox_chunk_" + chunk_idx.to_bytes(8, "big")
    chunk_size = 1024
    for offset in range(0, len(data), chunk_size):
        chunk = data[offset : offset + chunk_size]
        sp = _algod().suggested_params()
        idx_b = chunk_idx.to_bytes(8, "big")
        off_b = offset.to_bytes(8, "big")
        data_b = len(chunk).to_bytes(2, "big") + chunk
        EMPTY = (0, b"")
        txn = ApplicationCallTxn(
            sender=sender.address, sp=sp, index=orch_id,
            on_complete=OnComplete.NoOpOC,
            app_args=[write_sel, idx_b, off_b, data_b],
            boxes=[(orch_id, box_name)] + [EMPTY] * 7,
        )
        txid = _algod().send_transaction(txn.sign(sender.private_key))
        wait_for_confirmation(_algod(), txid, 4)


def _setup_orch(orch_id: int, sender: SigningAccount, main_id: int,
                main_bytes: bytes, chunks: list[dict]):
    """Run the full orch setup ceremony for the given main + chunks.
    Each entry in `chunks` is a deploy.uros.json chunk dict with:
        {"name", "methods" (list of method names), "approval_hex"}.
    Streams all bytecode into boxes and registers selector→chunk_idx
    mappings."""
    import hashlib

    # 1. set_main
    set_main_sel = _arc4_selector("set_main(uint64)void")
    sp = _algod().suggested_params()
    txn = ApplicationCallTxn(
        sender=sender.address, sp=sp, index=orch_id,
        on_complete=OnComplete.NoOpOC,
        app_args=[set_main_sel, main_id.to_bytes(8, "big")],
    )
    txid = _algod().send_transaction(txn.sign(sender.private_key))
    wait_for_confirmation(_algod(), txid, 4)

    # 2. setup_main_box(main_len)
    setup_main_sel = _arc4_selector("setup_main_box(uint64)void")
    sp = _algod().suggested_params()
    sp.fee = sp.min_fee * 3
    EMPTY = (0, b"")
    txn = ApplicationCallTxn(
        sender=sender.address, sp=sp, index=orch_id,
        on_complete=OnComplete.NoOpOC,
        app_args=[setup_main_sel, len(main_bytes).to_bytes(8, "big")],
        boxes=[(orch_id, b"__codebox_main")] + [EMPTY] * 7,
    )
    txid = _algod().send_transaction(txn.sign(sender.private_key))
    wait_for_confirmation(_algod(), txid, 4)

    # 3. setup_chunk_box(idx, len) for each chunk
    setup_chunk_sel = _arc4_selector("setup_chunk_box(uint64,uint64)void")
    for ci, chunk in enumerate(chunks):
        chunk_bytes = bytes.fromhex(chunk["approval_hex"])
        chunk_box = b"__codebox_chunk_" + ci.to_bytes(8, "big")
        clen_box = b"clen_" + ci.to_bytes(8, "big")
        sp = _algod().suggested_params()
        sp.fee = sp.min_fee * 3
        txn = ApplicationCallTxn(
            sender=sender.address, sp=sp, index=orch_id,
            on_complete=OnComplete.NoOpOC,
            app_args=[
                setup_chunk_sel,
                ci.to_bytes(8, "big"),
                len(chunk_bytes).to_bytes(8, "big"),
            ],
            boxes=[(orch_id, chunk_box), (orch_id, clen_box)] + [EMPTY] * 6,
        )
        txid = _algod().send_transaction(txn.sign(sender.private_key))
        wait_for_confirmation(_algod(), txid, 4)

    # 4. Stream main + each chunk
    _stream_main(orch_id, sender, main_bytes)
    for ci, chunk in enumerate(chunks):
        _stream_chunk(orch_id, sender, ci, bytes.fromhex(chunk["approval_hex"]))

    # 5. register_chunk_method(selector, idx) per split method.
    # The method's full ABI signature has to match what the contract
    # exposes — for simplicity the test passes <name>(... )... explicitly.
    # In a real harness you'd read the arc56 spec.
    register_sel = _arc4_selector("register_chunk_method(byte[],uint64)void")
    for ci, chunk in enumerate(chunks):
        for full_sig in chunk["methods"]:  # caller passes full signatures
            sel = hashlib.new("sha512_256", full_sig.encode()).digest()[:4]
            csel_box = b"csel_" + sel
            sel_arg = len(sel).to_bytes(2, "big") + sel
            sp = _algod().suggested_params()
            txn = ApplicationCallTxn(
                sender=sender.address, sp=sp, index=orch_id,
                on_complete=OnComplete.NoOpOC,
                app_args=[register_sel, sel_arg, ci.to_bytes(8, "big")],
                boxes=[(orch_id, csel_box)] + [EMPTY] * 7,
            )
            txid = _algod().send_transaction(txn.sign(sender.private_key))
            wait_for_confirmation(_algod(), txid, 4)


def test_dance_decrements_counter():
    sender = _account()

    # Build artifacts must already exist.
    assert (OUT / "Smoke" / "Smoke.approval.bin").exists(), \
        "run tests/uros-splitter/build.sh first"

    # 1. Deploy orchestrator FIRST so we know its app id. Main's stubs
    #    have a guard that asserts `gtxn[next].ApplicationID ==
    #    TMPL_UROS_ORCH_APP_ID`; we need to recompile main with the
    #    real orch id baked in before deploying main.
    init_sel = _arc4_selector("init()void")
    orch_id = _deploy_app(
        "Orchestrator", "UrosOrchestrator", sender,
        app_args=[init_sel],
    )
    _fund(_app_addr(orch_id), 5_000_000)

    # 2. Recompile main with the real orch app id substituted in.
    import subprocess
    subprocess.run(
        [str(HERE.parent.parent / "tests" / "uros-splitter" / "build.sh")],
        env={**__import__("os").environ, "UROS_ORCH_APP_ID": str(orch_id)},
        check=True,
        capture_output=True,
    )
    # Sanity: the new main bytes should differ at the pushint8 carrying
    # the orch id (won't equal the placeholder build).

    # 3. Deploy main with the substituted bytecode.
    main_id = _deploy_app("Smoke", "Smoke", sender)
    _fund(_app_addr(main_id), 1_000_000)

    # 4-5. Run the orch setup ceremony for one chunk.
    import json as _json
    deploy_tmpl = _json.loads((OUT / "Smoke" / "deploy.uros.json").read_text())
    main_bytes = _read_bin(OUT / "Smoke" / "Smoke.approval.bin")
    # Map chunk's bare method names → full ABI signatures so the
    # selector-registration step computes the right selectors.
    # For Smoke.sol the only split method is `dec(uint256)void`.
    chunks_with_full_sigs = []
    for chunk in deploy_tmpl["chunks"]:
        full_sigs = []
        for m in chunk["methods"]:
            if m == "dec":
                full_sigs.append("dec(uint256)void")
            elif m == "inc":
                full_sigs.append("inc(uint256)void")
            else:
                full_sigs.append(m)  # already a full signature
        chunks_with_full_sigs.append({**chunk, "methods": full_sigs})
    _setup_orch(orch_id, sender, main_id, main_bytes, chunks_with_full_sigs)

    # 6. Prime main: call inc(100) so counter has a value to decrement.
    inc_sel = _arc4_selector("inc(uint256)void")
    sp = _algod().suggested_params()
    val = (100).to_bytes(32, "big")
    txn = ApplicationCallTxn(
        sender=sender.address, sp=sp, index=main_id,
        on_complete=OnComplete.NoOpOC,
        app_args=[inc_sel, val],
    )
    txid = _algod().send_transaction(txn.sign(sender.private_key))
    wait_for_confirmation(_algod(), txid, 4)

    # 7. The dance: group [stub_call, dispatch].
    dec_sel = _arc4_selector("dec(uint256)void")
    dec_amt = (10).to_bytes(32, "big")
    dispatch_sel = _arc4_selector("dispatch()byte[]")

    sp = _algod().suggested_params()
    sp.fee = sp.min_fee * 12  # 3 inner txns + buffer

    txn1 = ApplicationCallTxn(
        sender=sender.address, sp=sp, index=main_id,
        on_complete=OnComplete.NoOpOC,
        app_args=[dec_sel, dec_amt],
    )
    # dispatch needs to read: the selector → chunk_idx mapping (csel_<sel>),
    # the chunk's length (clen_<idx>), main bytes, chunk bytes. With max
    # 8 box refs/txn (and 1 slot taken by foreign_apps), we have 7 box
    # ref slots — enough for the relevant boxes plus empty pad.
    EMPTY = (0, b"")
    csel_box = b"csel_" + dec_sel
    clen_box = b"clen_" + (0).to_bytes(8, "big")
    chunk_box = b"__codebox_chunk_" + (0).to_bytes(8, "big")
    txn2 = ApplicationCallTxn(
        sender=sender.address, sp=sp, index=orch_id,
        on_complete=OnComplete.NoOpOC,
        app_args=[dispatch_sel],
        foreign_apps=[main_id],
        boxes=[
            (orch_id, csel_box),
            (orch_id, clen_box),
            (orch_id, chunk_box),
            (orch_id, b"__codebox_main"),
            EMPTY, EMPTY, EMPTY,
        ],
    )
    assign_group_id([txn1, txn2])
    s1 = txn1.sign(sender.private_key)
    s2 = txn2.sign(sender.private_key)
    txid = _algod().send_transactions([s1, s2])
    wait_for_confirmation(_algod(), txid, 4)

    # 8. Read main's counter — should be 100 - 10 = 90. Tests that the
    # dance's revert step actually restored main's bytecode (otherwise
    # main would still be running helper's program here, where get() is
    # also stubbed to return 0).
    get_sel = _arc4_selector("get()uint256")
    sp = _algod().suggested_params()
    txn = ApplicationCallTxn(
        sender=sender.address, sp=sp, index=main_id,
        on_complete=OnComplete.NoOpOC,
        app_args=[get_sel],
    )
    txid = _algod().send_transaction(txn.sign(sender.private_key))
    result = wait_for_confirmation(_algod(), txid, 4)
    logs = result.get("logs", [])
    # First log is ABI return: 4-byte prefix (151f7c75) + 32-byte uint256
    assert logs, "no return log from get()"
    raw = base64.b64decode(logs[0])
    assert raw[:4] == bytes.fromhex("151f7c75"), f"bad ABI prefix: {raw[:4].hex()}"
    counter = int.from_bytes(raw[4:36], "big")
    assert counter == 90, f"expected 90, got {counter}"

    # 9. Sanity check: main's program bytes equal the original. Read
    # the on-chain program via algod and compare.
    info = _algod().application_info(main_id)
    on_chain = base64.b64decode(info["params"]["approval-program"])
    expected = _read_bin(OUT / "Smoke" / "Smoke.approval.bin")
    assert on_chain == expected, (
        f"main approval program drift: on-chain {len(on_chain)} B vs "
        f"expected {len(expected)} B; first-diff at byte "
        f"{next((i for i, (a, b) in enumerate(zip(on_chain, expected)) if a != b), 'n/a')}"
    )

    # 10. NEGATIVE: a direct call to main.dec(N) without the orch-dispatch
    #     follow-up must revert (the stub's guard rejects when the next
    #     group txn isn't orch.dispatch()). Confirms the guard actually
    #     enforces the dance.
    sp = _algod().suggested_params()
    bad_txn = ApplicationCallTxn(
        sender=sender.address, sp=sp, index=main_id,
        on_complete=OnComplete.NoOpOC,
        app_args=[dec_sel, dec_amt],
    )
    try:
        bad_id = _algod().send_transaction(bad_txn.sign(sender.private_key))
        wait_for_confirmation(_algod(), bad_id, 4)
        assert False, "direct main.dec() should have reverted (guard didn't fire)"
    except Exception as e:
        msg = str(e)
        assert "assert failed" in msg or "logic eval error" in msg, \
            f"unexpected error from guard: {msg}"


if __name__ == "__main__":
    test_dance_decrements_counter()
    print("OK")
