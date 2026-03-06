"""
AAVE V4 WETH9 tests.
"""

import pytest
import hashlib
import base64
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract


ABI_RETURN_PREFIX = bytes.fromhex("151f7c75")


def _box_ref(app_id, key):
    return au.BoxReference(app_id=app_id, name=key)


def _account_key(address_str):
    return encoding.decode_address(address_str)


def _arc28_selector(signature):
    """Compute ARC-28 event selector: sha512_256(signature)[:4]."""
    return hashlib.new("sha512_256", signature.encode()).digest()[:4]


def _extract_events(confirmation):
    """Extract non-ABI-return log entries from a transaction confirmation."""
    logs = confirmation.get("logs", [])
    events = []
    for log in logs:
        raw = base64.b64decode(log)
        if not raw.startswith(ABI_RETURN_PREFIX):
            events.append(raw)
    return events


def _allowance_box_key(sender_addr, guy_addr):
    """Compute box key for allowance[sender][guy] nested mapping."""
    sender_bytes = _account_key(sender_addr)
    guy_bytes = _account_key(guy_addr)
    inner_key = hashlib.sha256(b"allowance" + hashlib.sha256(sender_bytes).digest()).digest()
    return hashlib.sha256(inner_key + hashlib.sha256(guy_bytes).digest()).digest()


def _balance_box_key(addr):
    """Compute box key for balanceOf[addr] mapping."""
    return b"balanceOf" + hashlib.sha256(_account_key(addr)).digest()


@pytest.fixture(scope="module")
def weth(localnet, account):
    return deploy_contract(localnet, account, "WETH9")


_call_counter = 0


def _call(client, method, *args, boxes=None):
    global _call_counter
    _call_counter += 1
    note = f"w9_{_call_counter}".encode()
    kwargs = dict(method=method, args=list(args), note=note)
    if boxes:
        kwargs["box_references"] = boxes
    result = client.send.call(au.AppClientMethodCallParams(**kwargs))
    return result.abi_return


def _call_with_result(client, method, *args, boxes=None):
    """Call and return the full SendAppTransactionResult (for log inspection)."""
    global _call_counter
    _call_counter += 1
    note = f"w9_{_call_counter}".encode()
    kwargs = dict(method=method, args=list(args), note=note)
    if boxes:
        kwargs["box_references"] = boxes
    return client.send.call(au.AppClientMethodCallParams(**kwargs))


def test_deploy(weth):
    assert weth.app_id > 0


def test_name(weth):
    result = _call(weth, "name")
    assert result == "Wrapped Ether"


def test_symbol(weth):
    result = _call(weth, "symbol")
    assert result == "WETH"


def test_decimals(weth):
    result = _call(weth, "decimals")
    assert result == 18


def test_totalSupply(weth):
    result = _call(weth, "totalSupply")
    assert result == 0


def test_approve(weth, account):
    box_key = _allowance_box_key(account.address, account.address)
    result = weth.send.call(
        au.AppClientMethodCallParams(
            method="approve",
            args=[account.address, 100],
            box_references=[_box_ref(weth.app_id, box_key)],
        )
    )
    assert result.abi_return in (True, 128, 0x80)


def test_approve_emits_approval_event(weth, account):
    """approve() should emit an ARC-28 Approval event with the amount."""
    box_key = _allowance_box_key(account.address, account.address)
    result = _call_with_result(
        weth, "approve", account.address, 200,
        boxes=[_box_ref(weth.app_id, box_key)],
    )
    events = _extract_events(result.confirmation)
    assert len(events) == 1

    event = events[0]
    expected_selector = _arc28_selector("Approval(address,address,uint256)")
    assert event[:4] == expected_selector

    # Data: src(32B) + guy(32B) + wad(uint256=32B) = 96 bytes
    data = event[4:]
    assert len(data) == 96
    amount = int.from_bytes(data[64:96], "big")
    assert amount == 200


def test_transfer_zero(weth, account):
    """Transfer 0 amount to self should succeed (no balance needed)."""
    balance_box = _balance_box_key(account.address)
    result = _call(
        weth, "transfer", account.address, 0,
        boxes=[
            _box_ref(weth.app_id, balance_box),
            _box_ref(weth.app_id, balance_box),
        ],
    )
    assert result in (True, 128, 0x80)


def test_transfer_emits_transfer_event(weth, account):
    """transfer() should emit an ARC-28 Transfer event with the amount."""
    balance_box = _balance_box_key(account.address)
    result = _call_with_result(
        weth, "transfer", account.address, 0,
        boxes=[
            _box_ref(weth.app_id, balance_box),
            _box_ref(weth.app_id, balance_box),
        ],
    )
    events = _extract_events(result.confirmation)
    assert len(events) == 1

    event = events[0]
    expected_selector = _arc28_selector("Transfer(address,address,uint256)")
    assert event[:4] == expected_selector

    # Data: src(32B) + dst(32B) + wad(uint256=32B) = 96 bytes
    data = event[4:]
    assert len(data) == 96
    amount = int.from_bytes(data[64:96], "big")
    assert amount == 0
