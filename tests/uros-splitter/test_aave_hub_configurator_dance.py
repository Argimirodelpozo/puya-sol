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
    """Write `data` to orch's __codebox_<which> box in chunks. Each
    box ref in a txn's `boxes` list contributes 1024 read + 1024 write
    budget; max 8 refs/txn. We fill write_txn's box list with the
    real box plus 7 empty refs to get 8×1024 = 8192 read/write budget
    on a single txn — enough for a 5+ KB box modify."""
    write_sel = _arc4_selector("write_codebox(uint64,uint64,byte[])void")
    box_name = f"__codebox_{which}".encode()
    chunk_size = 1024
    EMPTY = (0, b"")
    box_refs = [(orch_id, box_name)] + [EMPTY] * 7
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
            boxes=box_refs,
        )
        txid = _algod().send_transaction(write_txn.sign(sender.private_key))
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
    # AVM box IO budget: 1024 read + 1024 write PER BOX REFERENCE in a
    # txn's `boxes` list (max 8 refs/txn). To pool enough budget for two
    # 5+ KB boxes, fill setup_txn AND every pad with 8 box refs each.
    # Empty references (app_id=0, name=b"") contribute budget without
    # naming a real box, exactly the "budget carrier" pattern.
    BOX0 = (orch_id, b"__codebox_0")
    BOX1 = (orch_id, b"__codebox_1")
    EMPTY = (0, b"")
    setup_txn = ApplicationCallTxn(
        sender=sender.address, sp=sp, index=orch_id,
        on_complete=OnComplete.NoOpOC,
        app_args=[
            setup_sel,
            len(main_bytes).to_bytes(8, "big"),
            len(helper_bytes).to_bytes(8, "big"),
        ],
        boxes=[BOX0, BOX1, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY],
    )
    pad_sel = _arc4_selector("pad(uint64)void")
    pad_txns = [
        ApplicationCallTxn(
            sender=sender.address, sp=sp, index=orch_id,
            on_complete=OnComplete.NoOpOC,
            app_args=[pad_sel, (i % 2).to_bytes(8, "big")],
            boxes=[BOX0, BOX1, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY],
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
    # MaxAppTotalTxnReferences = 8 across {accounts, foreign_apps,
    # foreign_assets, boxes}. With foreign_apps=[main_id] we have 7
    # slots left for boxes. Use 2 real + 5 empty = 7 box refs, giving
    # 7×1024 = 7168 read budget on dispatch's own txn. dispatch needs
    # to read ~5743 B from each box; 7168 covers one. Pair with a pad
    # txn to pool the other box's read budget.
    EMPTY = (0, b"")
    BOX0 = (orch_id, b"__codebox_0")
    BOX1 = (orch_id, b"__codebox_1")
    txn2 = ApplicationCallTxn(
        sender=sender.address, sp=sp, index=orch_id,
        on_complete=OnComplete.NoOpOC,
        app_args=[dispatch_sel],
        foreign_apps=[main_id],
        boxes=[BOX0, BOX1, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY],
    )
    assign_group_id([txn1, txn2])
    s1 = txn1.sign(sender.private_key)
    s2 = txn2.sign(sender.private_key)
    txid = _algod().send_transactions([s1, s2])
    wait_for_confirmation(_algod(), txid, 4)
    # The result we actually want is txn2's (the orch dispatch). Fetch
    # by txn2's id.
    txn2_id = txn2.get_txid()
    result = _algod().pending_transaction_info(txn2_id)

    # 8. dispatch() returns the inner call's last_log: the ABI-encoded
    #    address from helper's authority(). orch's own ABI return wraps
    #    its byte[] return in an outer 151f7c75 prefix + length-prefixed
    #    bytes, so we have to peel two layers.
    # Sanity-check the inner-txn structure: 3 inner txns with helper's
    # authority() log on the middle one (itxn 1: install helper; itxn 2:
    # call helper.authority(); itxn 3: restore main).
    inner_txns = result.get("inner-txns", [])
    assert len(inner_txns) == 3, f"expected 3 inner txns, got {len(inner_txns)}"
    inner_2_logs = inner_txns[1].get("logs", [])
    assert len(inner_2_logs) == 1, f"itxn 2 should have 1 log, got {len(inner_2_logs)}"

    orch_logs = result.get("logs", [])
    assert orch_logs, "no return log from orch.dispatch()"
    raw = base64.b64decode(orch_logs[0])
    assert raw[:4] == bytes.fromhex("151f7c75"), f"bad outer ABI prefix: {raw[:4].hex()}"
    # ARC4 dynamic byte[]: 2-byte length + bytes (itxn 2's last_log)
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
