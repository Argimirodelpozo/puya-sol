"""
M39: Pre/post increment/decrement correctness (Gap 10 verification).
Tests that i++ returns old value, ++i returns new value,
for both local variables and mapping state variables.
"""

import hashlib

import pytest
import algokit_utils as au
from algosdk import encoding
from algosdk.transaction import PaymentTxn, wait_for_confirmation
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract


def mapping_box_key(mapping_name: str, *keys: bytes) -> bytes:
    concat_keys = b"".join(keys)
    key_hash = hashlib.sha256(concat_keys).digest()
    return mapping_name.encode() + key_hash


def box_ref(app_id: int, name: bytes) -> au.BoxReference:
    return au.BoxReference(app_id=app_id, name=name)


def fund_contract(
    localnet: au.AlgorandClient,
    account: SigningAccount,
    app_id: int,
    amount: int = 1_000_000,
) -> None:
    algod = localnet.client.algod
    app_addr = encoding.encode_address(
        encoding.checksum(b"appID" + app_id.to_bytes(8, "big"))
    )
    sp = algod.suggested_params()
    txn = PaymentTxn(account.address, sp, app_addr, amount)
    signed = txn.sign(account.private_key)
    txid = algod.send_transaction(signed)
    wait_for_confirmation(algod, txid, 4)


def addr_key(addr: str) -> bytes:
    return encoding.decode_address(addr)


@pytest.fixture(scope="module")
def client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    c = deploy_contract(localnet, account, "PostIncrementTest")
    fund_contract(localnet, account, c.app_id, 2_000_000)
    return c


# ── Local variable tests ──

@pytest.mark.localnet
def test_local_post_increment(client: au.AppClient) -> None:
    """x++ should return old value (10), x becomes 11."""
    result = client.send.call(
        au.AppClientMethodCallParams(method="testLocalPostInc", args=[10])
    )
    # result=old value, after_=new value
    assert result.abi_return == {"result": 10, "after_": 11}


@pytest.mark.localnet
def test_local_pre_increment(client: au.AppClient) -> None:
    """++x should return new value (11), x is 11."""
    result = client.send.call(
        au.AppClientMethodCallParams(method="testLocalPreInc", args=[10])
    )
    assert result.abi_return == {"result": 11, "after_": 11}


@pytest.mark.localnet
def test_local_post_decrement(client: au.AppClient) -> None:
    """x-- should return old value (10), x becomes 9."""
    result = client.send.call(
        au.AppClientMethodCallParams(method="testLocalPostDec", args=[10])
    )
    assert result.abi_return == {"result": 10, "after_": 9}


@pytest.mark.localnet
def test_local_pre_decrement(client: au.AppClient) -> None:
    """--x should return new value (9), x is 9."""
    result = client.send.call(
        au.AppClientMethodCallParams(method="testLocalPreDec", args=[10])
    )
    assert result.abi_return == {"result": 9, "after_": 9}


# ── Mapping post-increment tests ──

@pytest.mark.localnet
def test_mapping_post_inc_returns_old_value(
    client: au.AppClient, account: SigningAccount
) -> None:
    """nonces[owner]++ should return 0 (initial) and nonce becomes 1."""
    app_id = client.app_id
    nonce_box = box_ref(app_id, mapping_box_key("nonces", addr_key(account.address)))

    # First call: nonce is 0, returns 0, nonce becomes 1
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="useNonce", args=[account.address],
            box_references=[nonce_box],
        )
    )
    assert result.abi_return == 0

    # Verify nonce is now 1
    result2 = client.send.call(
        au.AppClientMethodCallParams(
            method="getNonce", args=[account.address],
            box_references=[nonce_box],
        )
    )
    assert result2.abi_return == 1


@pytest.mark.localnet
def test_mapping_post_inc_second_call(
    client: au.AppClient, account: SigningAccount
) -> None:
    """Second nonces[owner]++ should return 1 (from previous test), nonce becomes 2."""
    app_id = client.app_id
    nonce_box = box_ref(app_id, mapping_box_key("nonces", addr_key(account.address)))

    result = client.send.call(
        au.AppClientMethodCallParams(
            method="useNonce", args=[account.address],
            box_references=[nonce_box],
            note=b"second",
        )
    )
    assert result.abi_return == 1

    result2 = client.send.call(
        au.AppClientMethodCallParams(
            method="getNonce", args=[account.address],
            box_references=[nonce_box],
            note=b"verify2",
        )
    )
    assert result2.abi_return == 2


# ── State variable increment tests ──

@pytest.mark.localnet
def test_state_post_inc_counter(client: au.AppClient) -> None:
    """counter++ returns old value (0), counter becomes 1."""
    result = client.send.call(
        au.AppClientMethodCallParams(method="postIncCounter", args=[])
    )
    assert result.abi_return == 0

    # Verify counter is now 1
    result2 = client.send.call(
        au.AppClientMethodCallParams(method="counter", args=[], note=b"c1")
    )
    assert result2.abi_return == 1


@pytest.mark.localnet
def test_state_pre_inc_counter(client: au.AppClient) -> None:
    """++counter returns new value. Counter was 1 from previous test, now 2."""
    result = client.send.call(
        au.AppClientMethodCallParams(method="preIncCounter", args=[], note=b"pre")
    )
    assert result.abi_return == 2

    # Verify counter is now 2
    result2 = client.send.call(
        au.AppClientMethodCallParams(method="counter", args=[], note=b"c2")
    )
    assert result2.abi_return == 2


# ── Standalone post-increment (side effect only) ──

@pytest.mark.localnet
def test_standalone_post_inc(
    client: au.AppClient, account: SigningAccount
) -> None:
    """Standalone nonces[owner]++ should still increment (side effect)."""
    app_id = client.app_id
    nonce_box = box_ref(app_id, mapping_box_key("nonces", addr_key(account.address)))

    # Get current nonce
    result1 = client.send.call(
        au.AppClientMethodCallParams(
            method="getNonce", args=[account.address],
            box_references=[nonce_box],
            note=b"before_standalone",
        )
    )
    before = result1.abi_return

    # Standalone increment
    client.send.call(
        au.AppClientMethodCallParams(
            method="incrementNonce", args=[account.address],
            box_references=[nonce_box],
            note=b"standalone",
        )
    )

    # Verify it incremented
    result2 = client.send.call(
        au.AppClientMethodCallParams(
            method="getNonce", args=[account.address],
            box_references=[nonce_box],
            note=b"after_standalone",
        )
    )
    assert result2.abi_return == before + 1
