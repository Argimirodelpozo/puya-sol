"""
OpenZeppelin Nonces behavioral tests.
Tests nonce tracking, increment, and checked usage.
"""

import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def addr_bytes(address: str) -> bytes:
    return encoding.decode_address(address)


def box_ref(app_id: int, name: bytes) -> au.BoxReference:
    return au.BoxReference(app_id=app_id, name=name)


@pytest.fixture(scope="module")
def nonces(localnet, account):
    client = deploy_contract(localnet, account, "NoncesTest")
    # Initialize the nonce box for the account (AVM boxes must be created before read+write)
    nonce_key = mapping_box_key("_nonces", addr_bytes(account.address))
    client.send.call(
        au.AppClientMethodCallParams(
            method="initNonce",
            args=[account.address],
            box_references=[box_ref(client.app_id, nonce_key)],
        )
    )
    return client


def test_deploy(nonces):
    assert nonces.app_id > 0


def test_initial_nonce_is_zero(nonces, account):
    nonce_key = mapping_box_key("_nonces", addr_bytes(account.address))
    result = nonces.send.call(
        au.AppClientMethodCallParams(
            method="nonces",
            args=[account.address],
            box_references=[box_ref(nonces.app_id, nonce_key)],
        )
    )
    assert result.abi_return == 0


def test_use_nonce_and_check(nonces, account):
    """useNonce increments and we can read the updated value."""
    nonce_key = mapping_box_key("_nonces", addr_bytes(account.address))
    # useNonce increments from 0 to 1
    nonces.send.call(
        au.AppClientMethodCallParams(
            method="useNonce",
            args=[account.address],
            box_references=[box_ref(nonces.app_id, nonce_key)],
        )
    )

    # Nonce should now be 1
    result = nonces.send.call(
        au.AppClientMethodCallParams(
            method="nonces",
            args=[account.address],
            box_references=[box_ref(nonces.app_id, nonce_key)],
        )
    )
    assert result.abi_return == 1


def test_use_checked_nonce_wrong_value_fails(nonces, account):
    nonce_key = mapping_box_key("_nonces", addr_bytes(account.address))
    # Current nonce is 1, but we pass 0 — should fail
    with pytest.raises(Exception):
        nonces.send.call(
            au.AppClientMethodCallParams(
                method="useCheckedNonce",
                args=[account.address, 0],
                box_references=[box_ref(nonces.app_id, nonce_key)],
            )
        )
