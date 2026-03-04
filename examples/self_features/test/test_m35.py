"""
M35: Mapping default values (Gap 7 verification).
Tests that reading unset mapping keys returns Solidity defaults (zero values)
instead of crashing, matching EVM behavior.
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


def biguint_key(val: int) -> bytes:
    return val.to_bytes(32, "big")


def addr_key(addr: str) -> bytes:
    return encoding.decode_address(addr)


@pytest.fixture(scope="module")
def client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    c = deploy_contract(localnet, account, "MappingDefaultsTest")
    fund_contract(localnet, account, c.app_id, 2_000_000)
    return c


# ── Reading uninitialized values should return zero/false ──

@pytest.mark.localnet
def test_uninitialized_uint256_returns_zero(client: au.AppClient) -> None:
    """Reading an unset uint256 mapping key should return 0."""
    app_id = client.app_id
    box = box_ref(app_id, mapping_box_key("_values", biguint_key(999)))

    result = client.send.call(
        au.AppClientMethodCallParams(
            method="getValue", args=[999],
            box_references=[box],
        )
    )
    assert result.abi_return == 0


@pytest.mark.localnet
def test_uninitialized_bool_returns_false(client: au.AppClient) -> None:
    """Reading an unset bool mapping key should return false."""
    app_id = client.app_id
    box = box_ref(app_id, mapping_box_key("_flags", biguint_key(999)))

    result = client.send.call(
        au.AppClientMethodCallParams(
            method="getFlag", args=[999],
            box_references=[box],
        )
    )
    assert result.abi_return is False


@pytest.mark.localnet
def test_uninitialized_struct_returns_defaults(client: au.AppClient) -> None:
    """Reading an unset struct mapping key should return (0, false)."""
    app_id = client.app_id
    box = box_ref(app_id, mapping_box_key("_infos", biguint_key(999)))

    result = client.send.call(
        au.AppClientMethodCallParams(
            method="getInfo", args=[999],
            box_references=[box],
        )
    )
    assert result.abi_return == [0, False]


@pytest.mark.localnet
def test_uninitialized_address_keyed_returns_zero(
    client: au.AppClient, account: SigningAccount
) -> None:
    """Reading an unset address-keyed mapping should return 0."""
    app_id = client.app_id
    # Use a random address that was never written to
    random_addr = account.address
    box = box_ref(app_id, mapping_box_key("_balances", addr_key(random_addr)))

    result = client.send.call(
        au.AppClientMethodCallParams(
            method="getBalance", args=[random_addr],
            box_references=[box],
        )
    )
    assert result.abi_return == 0


@pytest.mark.localnet
def test_uninitialized_nested_returns_zero(client: au.AppClient) -> None:
    """Reading an unset nested mapping should return 0."""
    app_id = client.app_id
    # Nested mapping key: _nested[10][20]
    # Key format: varName + sha256(concat(key1_normalized, key2_normalized))
    # Both keys are biguint (64 bytes each), concatenated then hashed
    composite = biguint_key(10) + biguint_key(20)
    nested_box = box_ref(
        app_id, b"_nested" + hashlib.sha256(composite).digest()
    )

    result = client.send.call(
        au.AppClientMethodCallParams(
            method="getNested", args=[10, 20],
            box_references=[nested_box],
        )
    )
    assert result.abi_return == 0


# ── Write-read isolation: writing to key A doesn't affect key B ──

@pytest.mark.localnet
def test_write_read_isolation(client: au.AppClient) -> None:
    """Writing to key 1 should not affect key 2."""
    app_id = client.app_id
    box1 = box_ref(app_id, mapping_box_key("_values", biguint_key(1)))
    box2 = box_ref(app_id, mapping_box_key("_values", biguint_key(2)))

    # Write to key 1
    client.send.call(
        au.AppClientMethodCallParams(
            method="setValue", args=[1, 42],
            box_references=[box1],
        )
    )
    # Read key 2 (uninitialized) — should be 0
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="getValue", args=[2],
            box_references=[box2],
        )
    )
    assert result.abi_return == 0

    # Read key 1 — should be 42
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="getValue", args=[1],
            box_references=[box1],
        )
    )
    assert result.abi_return == 42


# ── Conditional on default value ──

@pytest.mark.localnet
def test_is_zero_uninitialized(client: au.AppClient) -> None:
    """Comparing uninitialized mapping value to 0 should return true."""
    app_id = client.app_id
    box = box_ref(app_id, mapping_box_key("_values", biguint_key(777)))

    result = client.send.call(
        au.AppClientMethodCallParams(
            method="isZero", args=[777],
            box_references=[box],
        )
    )
    assert result.abi_return is True


# ── Increment from default (0 + 1 = 1) ──

@pytest.mark.localnet
def test_increment_from_default(client: au.AppClient) -> None:
    """Incrementing an uninitialized value should go from 0 to 1."""
    app_id = client.app_id
    box = box_ref(app_id, mapping_box_key("_values", biguint_key(555)))

    client.send.call(
        au.AppClientMethodCallParams(
            method="increment", args=[555],
            box_references=[box],
        )
    )
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="getValue", args=[555],
            box_references=[box],
        )
    )
    assert result.abi_return == 1


# ── Set then read: sanity check ──

@pytest.mark.localnet
def test_set_and_read_flag(client: au.AppClient) -> None:
    """Set a flag then read it back."""
    app_id = client.app_id
    box = box_ref(app_id, mapping_box_key("_flags", biguint_key(42)))

    client.send.call(
        au.AppClientMethodCallParams(
            method="setFlag", args=[42, True],
            box_references=[box],
        )
    )
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="getFlag", args=[42],
            box_references=[box],
        )
    )
    assert result.abi_return is True


@pytest.mark.localnet
def test_set_and_read_struct(client: au.AppClient) -> None:
    """Set a struct then read it back."""
    app_id = client.app_id
    box = box_ref(app_id, mapping_box_key("_infos", biguint_key(42)))

    client.send.call(
        au.AppClientMethodCallParams(
            method="setInfo", args=[42, 100, True],
            box_references=[box],
        )
    )
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="getInfo", args=[42],
            box_references=[box],
        )
    )
    assert result.abi_return == [100, True]
