"""Event-emission tests for CTFExchange.

Mirrors the `vm.expectEmit(...)` blocks in v2's CTFExchange.t.sol /
Pausable / Auth / Fees test bodies. We can't use Foundry's expectEmit on
AVM, but every Solidity `emit` statement lowers (via puya-sol's
SolEmitStatement) to a raw `op.log` opcode whose payload is
`selector(4) ++ arc4_encode(args)` — same shape ARC-28 specifies.

Verifying the event means: send the txn, pull `logs` from the
confirmation, find the entry whose first 4 bytes match the event's
selector, and check the rest matches the ARC-4 encoding of the arguments.
"""
import algokit_utils as au
import pytest
from algosdk.transaction import PaymentTxn, wait_for_confirmation

from conftest import addr
from dev.events import event_selector, decode_logs, assert_event_emitted


# ── fixtures (mirrored from test_ctfexchange.py — see notes there) ─────


@pytest.fixture
def exchange(split_exchange):
    _, _, orch = split_exchange
    return orch


@pytest.fixture
def henry(localnet, admin):
    acct = localnet.account.random()
    sp = localnet.client.algod.suggested_params()
    pay = PaymentTxn(admin.address, sp, acct.address, 1_000_000)
    wait_for_confirmation(localnet.client.algod,
        localnet.client.algod.send_transaction(pay.sign(admin.private_key)), 4)
    return acct


# ── helpers ─────────────────────────────────────────────────────────────


def _send(client, method, args=None, sender=None, extra_fee=30_000):
    """Send a method call and return the full algokit result so the caller
    can read logs from `result.confirmation`."""
    return client.send.call(au.AppClientMethodCallParams(
        method=method, args=args or [],
        sender=sender.address if sender else None,
        extra_fee=au.AlgoAmount(micro_algo=extra_fee),
    ), send_params=au.SendParams(populate_app_call_resources=True))


# ── Pausable ────────────────────────────────────────────────────────────


def test_pause_emits_TradingPaused(exchange, admin):
    """`pauseTrading()` emits `TradingPaused(address)` (ARC-28). The
    address arg is encoded as `uint8[32]` (puya-sol's address →
    32-byte arc4 type), so the payload is the admin's 32-byte address."""
    res = _send(exchange, "pauseTrading", sender=admin)
    assert_event_emitted(
        res, "TradingPaused", ["uint8[32]"],
        expected_payload=bytes(addr(admin)),
    )


def test_unpause_emits_TradingUnpaused(exchange, admin):
    _send(exchange, "pauseTrading", sender=admin)
    res = _send(exchange, "unpauseTrading", sender=admin)
    assert_event_emitted(
        res, "TradingUnpaused", ["uint8[32]"],
        expected_payload=bytes(addr(admin)),
    )


# ── Auth ────────────────────────────────────────────────────────────────


def test_add_admin_emits_NewAdmin(exchange, admin, henry):
    """`addAdmin(henry)` (by `admin`) emits NewAdmin(henry, admin)."""
    res = _send(exchange, "addAdmin", [addr(henry)], sender=admin)
    expected = bytes(addr(henry)) + bytes(addr(admin))
    assert_event_emitted(res, "NewAdmin", ["uint8[32]", "uint8[32]"],
                         expected_payload=expected)


def test_add_operator_emits_NewOperator(exchange, admin, henry):
    """`addOperator(henry)` (by `admin`) emits NewOperator(henry, admin)."""
    res = _send(exchange, "addOperator", [addr(henry)], sender=admin)
    expected = bytes(addr(henry)) + bytes(addr(admin))
    assert_event_emitted(res, "NewOperator", ["uint8[32]", "uint8[32]"],
                         expected_payload=expected)


def test_remove_admin_emits_RemovedAdmin(exchange, admin, henry):
    """`removeAdmin(henry)` (by `admin`) emits RemovedAdmin(henry, admin)."""
    _send(exchange, "addAdmin", [addr(henry)], sender=admin)
    res = _send(exchange, "removeAdmin", [addr(henry)], sender=admin)
    expected = bytes(addr(henry)) + bytes(addr(admin))
    assert_event_emitted(res, "RemovedAdmin", ["uint8[32]", "uint8[32]"],
                         expected_payload=expected)


def test_remove_operator_emits_RemovedOperator(exchange, admin, henry):
    _send(exchange, "addOperator", [addr(henry)], sender=admin)
    res = _send(exchange, "removeOperator", [addr(henry)], sender=admin)
    expected = bytes(addr(henry)) + bytes(addr(admin))
    assert_event_emitted(res, "RemovedOperator", ["uint8[32]", "uint8[32]"],
                         expected_payload=expected)


# ── Fees ────────────────────────────────────────────────────────────────


def test_set_fee_receiver_emits_FeeReceiverUpdated(exchange, admin, henry):
    res = _send(exchange, "setFeeReceiver", [addr(henry)], sender=admin)
    assert_event_emitted(res, "FeeReceiverUpdated", ["uint8[32]"],
                         expected_payload=bytes(addr(henry)))


def test_set_max_fee_rate_emits_MaxFeeRateUpdated(exchange, admin):
    """MaxFeeRateUpdated(uint256). 1000 = 0x3E8 in 32-byte big-endian."""
    new_rate = 1000
    res = _send(exchange, "setMaxFeeRate", [new_rate], sender=admin)
    assert_event_emitted(res, "MaxFeeRateUpdated", ["uint256"],
                         expected_payload=new_rate.to_bytes(32, "big"))


# ── User-Pause interval ────────────────────────────────────────────────


def test_set_user_pause_block_interval_emits_event(exchange, admin):
    """UserPauseBlockIntervalUpdated(uint256 oldVal, uint256 newVal).
    oldVal is the default; new is 50_000."""
    old_val = exchange.send.call(au.AppClientMethodCallParams(
        method="userPauseBlockInterval", args=[],
        extra_fee=au.AlgoAmount(micro_algo=30_000),
    )).abi_return
    new_val = 50_000
    res = _send(exchange, "setUserPauseBlockInterval", [new_val], sender=admin)
    expected = int(old_val).to_bytes(32, "big") + new_val.to_bytes(32, "big")
    assert_event_emitted(res, "UserPauseBlockIntervalUpdated",
                         ["uint256", "uint256"], expected_payload=expected)
