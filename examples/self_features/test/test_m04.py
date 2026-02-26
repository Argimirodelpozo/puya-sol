"""
M4: Struct storage and field mutation.
Verifies Config struct stored in state with individual field updates.
"""

import pytest

import algokit_utils as au
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract


@pytest.fixture(scope="module")
def storage_client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    return deploy_contract(localnet, account, "StructStorageTest")


@pytest.mark.localnet
def test_set_and_get(storage_client: au.AppClient) -> None:
    # Set config
    storage_client.send.call(
        au.AppClientMethodCallParams(method="setConfig", args=[42, True])
    )
    # Read threshold
    result = storage_client.send.call(
        au.AppClientMethodCallParams(method="getThreshold", args=[])
    )
    assert result.abi_return == 42
    # Read active
    result = storage_client.send.call(
        au.AppClientMethodCallParams(method="isActive", args=[])
    )
    assert result.abi_return is True


@pytest.mark.localnet
def test_update_threshold(storage_client: au.AppClient) -> None:
    # Set initial config
    storage_client.send.call(
        au.AppClientMethodCallParams(method="setConfig", args=[100, True])
    )
    # Update just the threshold
    storage_client.send.call(
        au.AppClientMethodCallParams(method="updateThreshold", args=[999])
    )
    # Verify threshold changed
    result = storage_client.send.call(
        au.AppClientMethodCallParams(method="getThreshold", args=[])
    )
    assert result.abi_return == 999
    # Verify active flag is still true (not corrupted)
    result = storage_client.send.call(
        au.AppClientMethodCallParams(method="isActive", args=[])
    )
    assert result.abi_return is True
