"""End-to-end test for --uros-splitter on a real AAVE V4 contract.

Splits `authority` and `setAuthority` (inherited from AccessManaged) out
of HubConfigurator. Demonstrates that:

1. `main.authority()` called directly returns address(0) — the stub
   bodies inserted by --uros-splitter return zero values.
2. `[main.authority(), orch.dispatch()]` returns the REAL authority
   stored in main's app state — proving the orch dance swapped the
   helper's bytecode in for the duration of the call.
3. Main's program bytes are byte-identical to the original after the
   dance — proving step 3 of the dance (revert-to-main-bytes) ran.

Run after tests/uros-splitter/build_aave.sh.
Requires localnet running.
"""

import base64
import hashlib
from pathlib import Path

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

HERE = Path(__file__).parent
OUT = HERE / "out_aave"


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


def _fund(addr: str, amt: int):
    sp = _algod().suggested_params()
    sender = _account()
    txn = PaymentTxn(sender.address, sp, addr, amt)
    txid = _algod().send_transaction(txn.sign(sender.private_key))
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
    txid = _algod().send_transaction(txn.sign(sender.private_key))
    result = wait_for_confirmation(_algod(), txid, 4)
    return int(result["application-index"])


def _stream_codebox(orch_id: int, sender: SigningAccount, which: int, data: bytes, main_id: int):
    """Write `data` to orch's __codebox_<which> box in chunks. AVM
    charges READ budget against box size for box_replace (validates
    offset+len fits). For a 5+ KB box that exceeds the per-txn
    2048-byte read budget. We pool the budget by grouping each write
    with 3 pad app-calls that also reference the box → 4×2048 = 8192
    read budget per group, plus 4×4096 = 16384 write budget."""
    write_sel = _arc4_selector("write_codebox(uint64,uint64,byte[])void")
    pad_sel = _arc4_selector("pad(uint64)void")
    box_name = f"__codebox_{which}".encode()
    chunk_size = 1024
    counter = 0
    for offset in range(0, len(data), chunk_size):
        chunk = data[offset:offset + chunk_size]
        sp = _algod().suggested_params()
        which_b = which.to_bytes(8, "big")
        off_b = offset.to_bytes(8, "big")
        data_b = len(chunk).to_bytes(2, "big") + chunk
        write_txn = ApplicationCallTxn(
            sender=sender.address,
            sp=sp,
            index=orch_id,
            on_complete=OnComplete.NoOpOC,
            app_args=[write_sel, which_b, off_b, data_b],
            boxes=[(orch_id, box_name)],
        )
        # Pad with `pad` no-op calls that DO execute a 1-byte box
        # extract; this makes the AVM count their box-budget allocation
        # toward the group pool. set_main wouldn't have worked because
        # the AVM seems to only count budget from txns that actually
        # execute box ops.
        # Pool with 7 pads (group max = 16, but per-write only 1 chunk
        # so 8 total = 8×2048 = 16384 read budget; covers 5743-byte box).
        pad_txns = [
            ApplicationCallTxn(
                sender=sender.address, sp=sp, index=orch_id,
                on_complete=OnComplete.NoOpOC,
                app_args=[pad_sel, which_b],
                boxes=[(orch_id, box_name)],
                note=f"pad_{which}_{counter}_{j}".encode(),
            )
            for j in range(7)
        ]
        counter += 1
        group = [write_txn, *pad_txns]
        assign_group_id(group)
        signed = [t.sign(sender.private_key) for t in group]
        txid = _algod().send_transactions(signed)
        wait_for_confirmation(_algod(), txid, 4)


def _decode_address_return(logs: list[str]) -> str:
    """Decode `authority()` ABI return: 4-byte ABI prefix + 32-byte address."""
    assert logs, "no return log"
    raw = base64.b64decode(logs[0])
    assert raw[:4] == bytes.fromhex("151f7c75"), f"bad ABI prefix: {raw[:4].hex()}"
    return encoding.encode_address(raw[4:36])


def test_aave_hub_configurator_dance():
    sender = _account()

    assert (OUT / "HubConfigurator" / "HubConfigurator.approval.bin").exists(), \
        "run tests/uros-splitter/build_aave.sh first"

    # 1. Deploy HubConfigurator. Constructor takes (address authority);
    #    we use sender.address so we know what to expect back from authority().
    authority_bytes = encoding.decode_address(sender.address)
    main_id = _deploy_app(
        "HubConfigurator", "HubConfigurator", sender,
        app_args=[authority_bytes],
    )
    _fund(_app_addr(main_id), 5_000_000)

    # 2. Deploy orchestrator.
    init_sel = _arc4_selector("init()void")
    orch_id = _deploy_app(
        "Orchestrator", "UrosOrchestrator", sender,
        app_args=[init_sel],
    )
    _fund(_app_addr(orch_id), 100_000_000)  # extra MBR for two big boxes

    # 3. Pin main's app id on the orchestrator.
    set_main_sel = _arc4_selector("set_main(uint64)void")
    sp = _algod().suggested_params()
    txn = ApplicationCallTxn(
        sender=sender.address, sp=sp, index=orch_id,
        on_complete=OnComplete.NoOpOC,
        app_args=[set_main_sel, main_id.to_bytes(8, "big")],
    )
    txid = _algod().send_transaction(txn.sign(sender.private_key))
    wait_for_confirmation(_algod(), txid, 4)

    # 4. Allocate the codebox boxes. box_create consumes write budget at
    #    1 unit/byte; default 4096 per txn isn't enough for a 5+ KB box,
    #    so we group the setup call with no-op padding txns to pool the
    #    budget across the group. 4 txns × 4096 = 16384 covers two ~6 KB
    #    boxes plus headroom.
    main_bytes = _read_bin(OUT / "HubConfigurator" / "HubConfigurator.approval.bin")
    helper_bytes = _read_bin(
        OUT / "HubConfigurator" / "__uros_split" / "HubConfigurator__split.approval.bin"
    )
    setup_sel = _arc4_selector("setup_boxes(uint64,uint64)void")
    sp = _algod().suggested_params()
    sp.fee = sp.min_fee * 4
    setup_txn = ApplicationCallTxn(
        sender=sender.address, sp=sp, index=orch_id,
        on_complete=OnComplete.NoOpOC,
        app_args=[
            setup_sel,
            len(main_bytes).to_bytes(8, "big"),
            len(helper_bytes).to_bytes(8, "big"),
        ],
        boxes=[(orch_id, b"__codebox_0"), (orch_id, b"__codebox_1")],
    )
    # Box write budget pools across ApplicationCall txns in the same
    # group that reference the SAME app AND execute a box op. set_main
    # doesn't touch boxes, so it doesn't contribute. setup_boxes runs
    # FIRST in the group (creates the boxes), so we pad with a fresh
    # box_create no-op... but those would conflict. Easier: just bundle
    # 3 setup_boxes calls (idempotent: AVM rejects duplicate creates,
    # so 1st creates and the 2nd-4th will fail). Better: pre-create
    # smaller boxes, then resize. Or: use try/catch in the contract.
    # Pragmatic: just use the box_extract no-op `pad` after setup,
    # and trust that setup_boxes' write budget alone is enough since
    # box_create is the heaviest op here (it consumes 1 unit/byte).
    # Empirically the budget spec is per-call, so pad with 3 set_main
    # calls just to consume slots in the group; if setup_boxes alone
    # has 4096 budget and we need 5743+5743 = 11486, we DO need pooling.
    # Use the new `pad` method that touches both boxes; its 1-byte
    # extract contributes to write budget too.
    pad_sel = _arc4_selector("pad(uint64)void")
    pad_txns = [
        ApplicationCallTxn(
            sender=sender.address, sp=sp, index=orch_id,
            on_complete=OnComplete.NoOpOC,
            app_args=[pad_sel, (i % 2).to_bytes(8, "big")],
            boxes=[(orch_id, b"__codebox_0"), (orch_id, b"__codebox_1")],
            note=f"setup_pad_{i}".encode(),
        )
        for i in range(7)
    ]
    group = [setup_txn, *pad_txns]
    assign_group_id(group)
    signed = [t.sign(sender.private_key) for t in group]
    txid = _algod().send_transactions(signed)
    wait_for_confirmation(_algod(), txid, 4)

    # 5. Stream both bytecodes.
    _stream_codebox(orch_id, sender, 0, main_bytes, main_id)
    _stream_codebox(orch_id, sender, 1, helper_bytes, main_id)

    # 6. Direct call to main.authority() — should return zero address (stub).
    auth_sel = _arc4_selector("authority()address")
    sp = _algod().suggested_params()
    txn = ApplicationCallTxn(
        sender=sender.address, sp=sp, index=main_id,
        on_complete=OnComplete.NoOpOC,
        app_args=[auth_sel],
    )
    txid = _algod().send_transaction(txn.sign(sender.private_key))
    result = wait_for_confirmation(_algod(), txid, 4)
    direct_auth = _decode_address_return(result.get("logs", []))
    print(f"direct main.authority() = {direct_auth}")
    zero_addr = encoding.encode_address(b"\x00" * 32)
    assert direct_auth == zero_addr, f"expected stubbed zero address, got {direct_auth}"

    # 7. The dance: group [main.authority(), orch.dispatch()].
    dispatch_sel = _arc4_selector("dispatch()byte[]")
    sp = _algod().suggested_params()
    sp.fee = sp.min_fee * 12  # 3 inner txns + buffer
    txn1 = ApplicationCallTxn(
        sender=sender.address, sp=sp, index=main_id,
        on_complete=OnComplete.NoOpOC,
        app_args=[auth_sel],
    )
    txn2 = ApplicationCallTxn(
        sender=sender.address, sp=sp, index=orch_id,
        on_complete=OnComplete.NoOpOC,
        app_args=[dispatch_sel],
        foreign_apps=[main_id],
        boxes=[(orch_id, b"__codebox_0"), (orch_id, b"__codebox_1")],
    )
    assign_group_id([txn1, txn2])
    s1 = txn1.sign(sender.private_key)
    s2 = txn2.sign(sender.private_key)
    txid = _algod().send_transactions([s1, s2])
    result = wait_for_confirmation(_algod(), txid, 4)

    # 8. dispatch() returns the inner call's last_log: the ABI-encoded
    #    address from helper's authority(). orch's own ABI return wraps
    #    its byte[] return in an outer 151f7c75 prefix + length-prefixed
    #    bytes, so we have to peel two layers.
    orch_logs = result.get("logs", [])
    assert orch_logs, "no return log from orch.dispatch()"
    raw = base64.b64decode(orch_logs[0])
    assert raw[:4] == bytes.fromhex("151f7c75"), f"bad outer ABI prefix: {raw[:4].hex()}"
    # ARC4 dynamic byte[]: 2-byte length + bytes
    inner_len = int.from_bytes(raw[4:6], "big")
    inner_log = raw[6:6 + inner_len]
    # inner_log is helper.authority()'s log, also ABI-prefixed
    assert inner_log[:4] == bytes.fromhex("151f7c75"), \
        f"bad inner ABI prefix: {inner_log[:4].hex()}"
    danced_auth = encoding.encode_address(inner_log[4:36])
    print(f"danced main.authority() = {danced_auth}")
    assert danced_auth == sender.address, \
        f"expected real authority {sender.address}, got {danced_auth}"

    # 9. Verify main is restored: bytes byte-identical to the original.
    info = _algod().application_info(main_id)
    on_chain = base64.b64decode(info["params"]["approval-program"])
    assert on_chain == main_bytes, (
        f"main approval drift: on-chain {len(on_chain)} B vs expected "
        f"{len(main_bytes)} B"
    )
    print("OK — dance routes split methods through helper, restores main")


if __name__ == "__main__":
    test_aave_hub_configurator_dance()
