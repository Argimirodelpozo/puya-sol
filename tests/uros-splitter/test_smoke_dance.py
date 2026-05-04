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


def _stream_codebox(orch_id: int, sender: SigningAccount, which: int, data: bytes):
    """Stream `data` into orchestrator's __codebox_<which> in 1024-B chunks.
    AVM single-call ApplicationArgs total cap is 2048 B; 1024 leaves room
    for selector + which + offset args."""
    write_sel = _arc4_selector("write_codebox(uint64,uint64,byte[])void")
    box_name = f"__codebox_{which}".encode()
    chunk_size = 1024
    for offset in range(0, len(data), chunk_size):
        chunk = data[offset : offset + chunk_size]
        sp = _algod().suggested_params()
        # ABI-encode args: which (uint64), offset (uint64), data (dynamic bytes)
        which_b = which.to_bytes(8, "big")
        off_b = offset.to_bytes(8, "big")
        # dynamic bytes encoding: 2-byte length prefix + data
        data_b = len(chunk).to_bytes(2, "big") + chunk
        txn = ApplicationCallTxn(
            sender=sender.address,
            sp=sp,
            index=orch_id,
            on_complete=OnComplete.NoOpOC,
            app_args=[write_sel, which_b, off_b, data_b],
            boxes=[(orch_id, box_name)],
        )
        signed = txn.sign(sender.private_key)
        txid = _algod().send_transaction(signed)
        wait_for_confirmation(_algod(), txid, 4)


def test_dance_decrements_counter():
    sender = _account()

    # Build artifacts must already exist.
    assert (OUT / "Smoke" / "Smoke.approval.bin").exists(), \
        "run tests/uros-splitter/build.sh first"

    # 1. Deploy main contract (with stubbed `dec`).
    main_id = _deploy_app("Smoke", "Smoke", sender)
    _fund(_app_addr(main_id), 1_000_000)

    # 2. Deploy orchestrator (calls __init__ via NoOp create).
    init_sel = _arc4_selector("init()void")
    orch_id = _deploy_app(
        "Orchestrator", "UrosOrchestrator", sender,
        app_args=[init_sel],
    )
    _fund(_app_addr(orch_id), 5_000_000)  # extra MBR for boxes

    # 3. set_main(main_id)
    set_main_sel = _arc4_selector("set_main(uint64)void")
    sp = _algod().suggested_params()
    txn = ApplicationCallTxn(
        sender=sender.address, sp=sp, index=orch_id,
        on_complete=OnComplete.NoOpOC,
        app_args=[set_main_sel, main_id.to_bytes(8, "big")],
    )
    txid = _algod().send_transaction(txn.sign(sender.private_key))
    wait_for_confirmation(_algod(), txid, 4)

    # 4. setup_boxes(main_len, helper_len)
    main_bytes = _read_bin(OUT / "Smoke" / "Smoke.approval.bin")
    helper_bytes = _read_bin(OUT / "Smoke" / "__uros_split" / "Smoke__split.approval.bin")
    setup_sel = _arc4_selector("setup_boxes(uint64,uint64)void")
    sp = _algod().suggested_params()
    sp.fee = sp.min_fee * 3  # cover inner box ops
    txn = ApplicationCallTxn(
        sender=sender.address, sp=sp, index=orch_id,
        on_complete=OnComplete.NoOpOC,
        app_args=[
            setup_sel,
            len(main_bytes).to_bytes(8, "big"),
            len(helper_bytes).to_bytes(8, "big"),
        ],
        boxes=[(orch_id, b"__codebox_0"), (orch_id, b"__codebox_1")],
    )
    txid = _algod().send_transaction(txn.sign(sender.private_key))
    wait_for_confirmation(_algod(), txid, 4)

    # 5. Stream both bytecodes.
    _stream_codebox(orch_id, sender, 0, main_bytes)
    _stream_codebox(orch_id, sender, 1, helper_bytes)

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
    txn2 = ApplicationCallTxn(
        sender=sender.address, sp=sp, index=orch_id,
        on_complete=OnComplete.NoOpOC,
        app_args=[dispatch_sel],
        foreign_apps=[main_id],
        boxes=[
            (orch_id, b"__codebox_0"),
            (orch_id, b"__codebox_1"),
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


if __name__ == "__main__":
    test_dance_decrements_counter()
    print("OK")
