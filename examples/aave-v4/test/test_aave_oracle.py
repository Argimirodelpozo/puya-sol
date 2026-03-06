"""
AAVE V4 AaveOracle tests.
"""

import pytest
import hashlib
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract


ZERO_ADDR = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAY5HFKQ"

_call_counter = 0


def _box_ref(app_id, key):
    return au.BoxReference(app_id=app_id, name=key)


def _mapping_box_key(mapping_name, key_bytes):
    return mapping_name.encode() + hashlib.sha256(key_bytes).digest()


def _biguint_key(val):
    raw = val.to_bytes((val.bit_length() + 7) // 8, 'big') if val > 0 else b'\x00'
    padded = b'\x00' * 64 + raw
    return padded[len(padded) - 64:]


def _call(client, method, *args, boxes=None):
    global _call_counter
    _call_counter += 1
    note = f"ao_{_call_counter}".encode()
    kwargs = dict(method=method, args=list(args), note=note)
    if boxes:
        kwargs["box_references"] = boxes
    result = client.send.call(au.AppClientMethodCallParams(**kwargs))
    return result.abi_return


@pytest.fixture(scope="module")
def oracle(localnet, account):
    decimals = 8
    description = b"Aave Oracle"
    return deploy_contract(
        localnet, account, "AaveOracle",
        app_args=[decimals.to_bytes(8, "big"), description],
    )


def test_deploy(oracle):
    assert oracle.app_id > 0


def test_description(oracle):
    result = _call(oracle, "DESCRIPTION")
    assert result == "Aave Oracle"


def test_spoke_initial(oracle):
    """SPOKE should be zero address initially."""
    result = _call(oracle, "SPOKE")
    assert result == ZERO_ADDR


def test_get_reserve_source_unset(oracle):
    """getReserveSource for unset reserve should return zero address."""
    reserve_id = 0
    box_key = _mapping_box_key("_reserveSources", _biguint_key(reserve_id))
    result = _call(oracle, "getReserveSource", reserve_id, boxes=[_box_ref(oracle.app_id, box_key)])
    assert result == ZERO_ADDR


@pytest.mark.xfail(reason="setSpoke requires authority inner txn (AccessManaged)")
def test_set_spoke(oracle, account):
    """setSpoke should update the spoke address."""
    _call(oracle, "setSpoke", account.address)
    result = _call(oracle, "SPOKE")
    assert result == account.address


@pytest.mark.xfail(reason="setReserveSource requires authority inner txn (AccessManaged)")
def test_set_reserve_source(oracle, account):
    """setReserveSource should store a price source for a reserve."""
    reserve_id = 1
    box_key = _mapping_box_key("_reserveSources", _biguint_key(reserve_id))
    box = _box_ref(oracle.app_id, box_key)
    _call(oracle, "setReserveSource", reserve_id, account.address, boxes=[box])
    result = _call(oracle, "getReserveSource", reserve_id, boxes=[box])
    assert result == account.address


@pytest.mark.xfail(reason="requires authority + UnitPriceFeed inner txn")
def test_get_reserve_price_with_unit_feed(oracle, localnet, account):
    """Set up a UnitPriceFeed as price source and verify assignment."""
    feed = deploy_contract(
        localnet, account, "UnitPriceFeed",
        app_args=[(8).to_bytes(8, "big"), b"Test Feed"],
    )
    feed_addr = encoding.encode_address(
        encoding.checksum(b"appID" + feed.app_id.to_bytes(8, "big"))
    )

    reserve_id = 2
    box_key = _mapping_box_key("_reserveSources", _biguint_key(reserve_id))
    box = _box_ref(oracle.app_id, box_key)
    _call(oracle, "setReserveSource", reserve_id, feed_addr, boxes=[box])

    source = _call(oracle, "getReserveSource", reserve_id, boxes=[box])
    assert source == feed_addr
