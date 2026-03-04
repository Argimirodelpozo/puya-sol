"""
M34: Struct mapping field mutation (Gap 8 verification).
Tests that writing to individual fields of structs stored in mappings persists.

Patterns tested:
  1. mapping[key].field = value      (direct inline)
  2. mapping[key].field += value     (compound inline)
  3. mapping[key].field -= value     (compound inline)
  4. Multiple field updates on same mapping entry
  5. Storage pointer field write
  6. Storage pointer compound assignment
  7. Write full struct then mutate one field
  8. Storage pointer multi-field update
  9. Independent mappings with same key
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
    """Convert int to 64-byte biguint key (matching AVM biguint representation)."""
    return val.to_bytes(32, "big")


def positions_box(key_id: int, app_id: int) -> au.BoxReference:
    """Build box reference for _positions[key_id]."""
    return box_ref(app_id, mapping_box_key("_positions", biguint_key(key_id)))


def orders_box(key_id: int, app_id: int) -> au.BoxReference:
    """Build box reference for _orders[key_id]."""
    return box_ref(app_id, mapping_box_key("_orders", biguint_key(key_id)))


@pytest.fixture(scope="module")
def client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    c = deploy_contract(localnet, account, "StructMappingTest")
    fund_contract(localnet, account, c.app_id, 2_000_000)
    return c


# ── Pattern 1: Direct inline field assignment ──

@pytest.mark.localnet
def test_set_supply(client: au.AppClient) -> None:
    """mapping[key].field = value should persist."""
    app_id = client.app_id
    box = positions_box(1, app_id)

    client.send.call(
        au.AppClientMethodCallParams(
            method="setSupply", args=[1, 100],
            box_references=[box],
        )
    )
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="getSupply", args=[1],
            box_references=[box],
        )
    )
    assert result.abi_return == 100


# ── Pattern 2: Compound inline field assignment ──

@pytest.mark.localnet
def test_add_supply(client: au.AppClient) -> None:
    """mapping[key].field += value should accumulate."""
    app_id = client.app_id
    box = positions_box(2, app_id)

    # Set initial
    client.send.call(
        au.AppClientMethodCallParams(
            method="setSupply", args=[2, 50],
            box_references=[box],
        )
    )
    # Add
    client.send.call(
        au.AppClientMethodCallParams(
            method="addSupply", args=[2, 30],
            box_references=[box],
        )
    )
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="getSupply", args=[2],
            box_references=[box],
        )
    )
    assert result.abi_return == 80


@pytest.mark.localnet
def test_sub_supply(client: au.AppClient) -> None:
    """mapping[key].field -= value should decrease."""
    app_id = client.app_id
    box = positions_box(3, app_id)

    client.send.call(
        au.AppClientMethodCallParams(
            method="setSupply", args=[3, 200],
            box_references=[box],
        )
    )
    client.send.call(
        au.AppClientMethodCallParams(
            method="subSupply", args=[3, 75],
            box_references=[box],
        )
    )
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="getSupply", args=[3],
            box_references=[box],
        )
    )
    assert result.abi_return == 125


# ── Pattern 3: Multiple field updates on same struct ──

@pytest.mark.localnet
def test_update_position(client: au.AppClient) -> None:
    """Setting all three fields of a struct via inline assignment."""
    app_id = client.app_id
    box = positions_box(4, app_id)

    client.send.call(
        au.AppClientMethodCallParams(
            method="updatePosition", args=[4, 1000, 500, 250],
            box_references=[box],
        )
    )
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="getPosition", args=[4],
            box_references=[box],
        )
    )
    assert result.abi_return == [1000, 500, 250]


# ── Pattern 4: Field independence — updating one field doesn't clobber others ──

@pytest.mark.localnet
def test_field_independence(client: au.AppClient) -> None:
    """After setting all fields, updating one field preserves the others."""
    app_id = client.app_id
    box = positions_box(5, app_id)

    # Set all fields
    client.send.call(
        au.AppClientMethodCallParams(
            method="updatePosition", args=[5, 100, 200, 300],
            box_references=[box],
        )
    )
    # Update only supply
    client.send.call(
        au.AppClientMethodCallParams(
            method="setSupply", args=[5, 999],
            box_references=[box],
        )
    )
    # All other fields should be unchanged
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="getPosition", args=[5],
            box_references=[box],
        )
    )
    assert result.abi_return == [999, 200, 300]


# ── Pattern 5: Storage pointer field write ──

@pytest.mark.localnet
def test_storage_pointer_write(client: au.AppClient) -> None:
    """Position storage p = _positions[id]; p.borrow = newBorrow; should persist."""
    app_id = client.app_id
    box = positions_box(6, app_id)

    # Set initial position
    client.send.call(
        au.AppClientMethodCallParams(
            method="updatePosition", args=[6, 10, 20, 30],
            box_references=[box],
        )
    )
    # Update borrow via storage pointer
    client.send.call(
        au.AppClientMethodCallParams(
            method="updateViaPointer", args=[6, 999],
            box_references=[box],
        )
    )
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="getPosition", args=[6],
            box_references=[box],
        )
    )
    # supply=10, borrow=999, collateral=30
    assert result.abi_return == [10, 999, 30]


# ── Pattern 6: Storage pointer compound assignment ──

@pytest.mark.localnet
def test_storage_pointer_compound(client: au.AppClient) -> None:
    """Position storage p = _positions[id]; p.collateral += amount; should accumulate."""
    app_id = client.app_id
    box = positions_box(7, app_id)

    client.send.call(
        au.AppClientMethodCallParams(
            method="updatePosition", args=[7, 5, 10, 100],
            box_references=[box],
        )
    )
    client.send.call(
        au.AppClientMethodCallParams(
            method="addCollateralViaPointer", args=[7, 50],
            box_references=[box],
        )
    )
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="getPosition", args=[7],
            box_references=[box],
        )
    )
    # supply=5, borrow=10, collateral=150
    assert result.abi_return == [5, 10, 150]


# ── Pattern 7: Write full struct then mutate single field ──

@pytest.mark.localnet
def test_create_and_modify(client: au.AppClient) -> None:
    """Write full Order struct, then set filled=true inline."""
    app_id = client.app_id
    box = orders_box(1, app_id)

    client.send.call(
        au.AppClientMethodCallParams(
            method="createAndModify", args=[1, 50, 10],
            box_references=[box],
        )
    )
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="getOrder", args=[1],
            box_references=[box],
        )
    )
    # price=50, quantity=10, filled=true
    assert result.abi_return == [50, 10, True]


# ── Pattern 8: Storage pointer multi-field update ──

@pytest.mark.localnet
def test_fill_order_via_pointer(client: au.AppClient) -> None:
    """Order storage o = _orders[id]; o.price = X; o.filled = true; should update both."""
    app_id = client.app_id
    box = orders_box(2, app_id)

    # Create order
    client.send.call(
        au.AppClientMethodCallParams(
            method="createAndModify", args=[2, 100, 25],
            box_references=[box],
        )
    )
    # Fill with new price via storage pointer
    client.send.call(
        au.AppClientMethodCallParams(
            method="fillOrder", args=[2, 75],
            box_references=[box],
        )
    )
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="getOrder", args=[2],
            box_references=[box],
        )
    )
    # price=75 (updated), quantity=25 (unchanged), filled=true
    assert result.abi_return == [75, 25, True]


# ── Pattern 9: Independent mappings, same key ──

@pytest.mark.localnet
def test_independent_mappings(client: au.AppClient) -> None:
    """Setting fields in different mappings with the same key should be independent."""
    app_id = client.app_id
    pos_box = positions_box(10, app_id)
    ord_box = orders_box(10, app_id)

    client.send.call(
        au.AppClientMethodCallParams(
            method="setBoth", args=[10, 777, 888],
            box_references=[pos_box, ord_box],
        )
    )
    # Check position supply
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="getSupply", args=[10],
            box_references=[pos_box],
        )
    )
    assert result.abi_return == 777

    # Check order price
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="getOrder", args=[10],
            box_references=[ord_box],
        )
    )
    # price=888, quantity=0, filled=false (default for unset fields)
    assert result.abi_return == [888, 0, False]
